#include "instance.h"
#include "memory.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <tlhelp32.h>
#include <nlohmann/json.hpp>

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ offsets

bool InstanceStore::Load(const std::string& path) {
    table_.clear();
    loaded_ = false;
    manualDm_ = 0;
    cachedDm_ = 0;
    InvalidateIndex();
    std::ifstream f(path);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        auto off = j.contains("Offsets") ? j["Offsets"] : j;
        table_.reserve(off.size() * 2);
        for (auto& cls : off.items()) {
            auto& dst = table_[cls.key()];
            for (auto& m : cls.value().items())
                if (m.value().is_number()) dst[m.key()] = m.value().get<int64_t>();
        }
        loaded_ = true;
        return true;
    } catch (...) { return false; }
}

std::optional<int64_t> InstanceStore::OffsetOf(const std::string& c, const std::string& m) const {
    auto it = table_.find(c);
    if (it == table_.end()) return std::nullopt;
    auto jt = it->second.find(m);
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}
int64_t InstanceStore::OffsetOr(const std::string& c, const std::string& m, int64_t fb) const {
    auto o = OffsetOf(c, m);
    return o ? *o : fb;
}

// ------------------------------------------------------------------ move

InstanceStore::InstanceStore(InstanceStore&& o) noexcept
    : mem_(o.mem_), loaded_(o.loaded_), table_(std::move(o.table_)),
      manualDm_(o.manualDm_), cachedDm_(o.cachedDm_),
      children_(std::move(o.children_)), leafStamps_(std::move(o.leafStamps_)),
      instances_(std::move(o.instances_)),
      indexReady_(o.indexReady_), indexStamp_(o.indexStamp_), indexPid_(o.indexPid_), lastPlayersRefresh_(o.lastPlayersRefresh_),
      lastServiceRefresh_(o.lastServiceRefresh_),
      indexBytes_(o.indexBytes_), indexMs_(o.indexMs_),
      nameCache_(std::move(o.nameCache_)) {}

InstanceStore& InstanceStore::operator=(InstanceStore&& o) noexcept {
    if (this == &o) return *this;
    mem_ = o.mem_; loaded_ = o.loaded_; table_ = std::move(o.table_);
    manualDm_ = o.manualDm_; cachedDm_ = o.cachedDm_;
    children_ = std::move(o.children_); leafStamps_ = std::move(o.leafStamps_);
    instances_ = std::move(o.instances_);
    indexReady_ = o.indexReady_; indexStamp_ = o.indexStamp_; indexPid_ = o.indexPid_;
    lastServiceRefresh_ = o.lastServiceRefresh_;
    lastPlayersRefresh_ = o.lastPlayersRefresh_;
    indexBytes_ = o.indexBytes_; indexMs_ = o.indexMs_;
    nameCache_ = std::move(o.nameCache_);
    return *this;
}

// ------------------------------------------------------------------ raw reads

uintptr_t InstanceStore::ReadPtr(uintptr_t a) {
    uintptr_t v = 0;
    if (!mem_ || !a || !mem_->CustomRead(a, &v, sizeof(v))) return 0;
    return v;
}
bool InstanceStore::IsReadablePtr(uintptr_t p) const {
    return p > 0x10000 && p < 0x0000800000000000ULL;
}

// Roblox string layout: [0]=ptr-or-cap, [8..0x17]=inline chars (16), [0x18]=size
std::string InstanceStore::ReadRobloxString(uintptr_t addr, size_t maxLen) const {
    if (!mem_ || !addr) return "";
    if (maxLen > 256) maxLen = 256;
    uint8_t buf[0x20] = {};
    if (!mem_->CustomRead(addr, buf, sizeof(buf))) return "";
    auto printable = [](const char* s, size_t n) {
        if (n == 0 || n > 256) return false;
        for (size_t i = 0; i < n; ++i)
            if (!isprint((unsigned char)s[i]) && s[i] != '\0') return false;
        return true;
    };
    uint64_t len = *(uint64_t*)(buf + 0x18);
    // inline: short names live in the 16-byte buffer
    if (len > 0 && len <= 16 && buf[8 + len] == '\0' && printable((char*)buf + 8, (size_t)len))
        return std::string((char*)buf + 8, (size_t)len);
    // heap: pointer in the union slot
    uintptr_t ptr = *(uintptr_t*)buf;
    if (len > 0 && len <= maxLen && IsReadablePtr(ptr)) {
        std::string s((size_t)len, '\0');
        if (mem_->CustomRead(ptr, s.data(), (size_t)len) && printable(s.data(), (size_t)len)) {
            size_t z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            return s;
        }
    }
    // last resort: NUL-terminated chars right here
    char raw[64] = {};
    if (mem_->CustomRead(addr, raw, sizeof(raw))) {
        size_t n = strnlen(raw, sizeof(raw));
        if (n > 0 && n <= maxLen && printable(raw, n)) return std::string(raw, n);
    }
    return "";
}

std::string InstanceStore::GetName(uintptr_t inst) {
    if (!inst) return "";
    {
        std::shared_lock lk(metaMutex_);
        auto it = nameCache_.find(inst);
        if (it != nameCache_.end() && !it->second.first.empty()) return it->second.first;
    }
    const int64_t nameOff = OffsetOr("Instance", "Name", 8);
    const int64_t containerOff = OffsetOr("Instance", "NameContainer", 112);
    std::string name;
    uintptr_t container = ReadPtr(inst + (uintptr_t)containerOff);
    if (IsReadablePtr(container)) {
        name = ReadRobloxString(container + (uintptr_t)nameOff);
        if (name.empty()) name = ReadRobloxString(container);
    }
    if (name.empty()) name = ReadRobloxString(inst + (uintptr_t)containerOff);
    if (name.empty()) {
        uintptr_t p = ReadPtr(inst + (uintptr_t)nameOff);
        if (IsReadablePtr(p)) name = ReadRobloxString(p);
    }
    if (!name.empty()) {
        std::unique_lock lk(metaMutex_);
        if (nameCache_.size() > 300000) nameCache_.clear();
        auto& slot = nameCache_[inst];
        if (slot.first.empty()) slot.first = name;
        else name = slot.first;
    }
    return name;
}

std::string InstanceStore::GetClass(uintptr_t inst) {
    if (!inst) return "";
    {
        std::shared_lock lk(metaMutex_);
        auto it = nameCache_.find(inst);
        if (it != nameCache_.end() && !it->second.second.empty()) return it->second.second;
    }
    const int64_t descOff = OffsetOr("Instance", "ClassDescriptor", 24);
    const int64_t cnOff = OffsetOr("Instance", "ClassName", 8);
    // A class name is a short printable ASCII token; anything else means the
    // descriptor pointer was misread, so reject it instead of returning junk.
    auto validClass = [](const std::string& s) {
        if (s.empty() || s.size() > 48) return false;
        for (char c : s)
            if (!isalnum((unsigned char)c)) return false;
        return true;
    };
    std::string cls;
    uintptr_t desc = ReadPtr(inst + (uintptr_t)descOff);
    if (IsReadablePtr(desc)) {
        cls = ReadRobloxString(desc + (uintptr_t)cnOff);
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (IsReadablePtr(p)) cls = ReadRobloxString(p);
        }
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (IsReadablePtr(p)) {
                uintptr_t p2 = ReadPtr(p);
                if (IsReadablePtr(p2)) cls = ReadRobloxString(p2);
            }
        }
        if (!validClass(cls)) cls.clear();
    }
    if (!cls.empty()) {
        std::unique_lock lk(metaMutex_);
        if (nameCache_.size() > 300000) nameCache_.clear();
        auto& slot = nameCache_[inst];
        if (slot.second.empty()) slot.second = cls;
        else cls = slot.second;
    }
    return cls;
}

uintptr_t InstanceStore::GetParent(uintptr_t inst) {
    if (!inst) return 0;
    uintptr_t p = ReadPtr(inst + (uintptr_t)OffsetOr("Instance", "Parent", 104));
    return IsReadablePtr(p) ? p : 0;
}

bool InstanceStore::IsValidInstance(uintptr_t inst) {
    if (!IsReadablePtr(inst)) return false;
    if (ReadPtr(inst + 8) != inst) return false;                     // This
    if (!IsReadablePtr(ReadPtr(inst + (uintptr_t)OffsetOr("Instance", "ClassDescriptor", 24)))) return false;
    if (!IsReadablePtr(ReadPtr(inst + (uintptr_t)OffsetOr("Instance", "NameContainer", 112)))) return false;
    return true;
}

// ------------------------------------------------------------------ index

void InstanceStore::InvalidateIndex() {
    std::unique_lock lk(indexMutex_);
    children_.clear();
    leafStamps_.clear();
    instances_.clear();
    indexReady_ = false;
    indexStamp_ = 0;
    indexBytes_ = 0;
    indexMs_ = 0;
    lastServiceRefresh_ = 0;
    {
        std::unique_lock mk(metaMutex_);
        nameCache_.clear();
    }
}

bool InstanceStore::IsIndexReady() const {
    std::shared_lock lk(indexMutex_);
    return indexReady_;
}
size_t InstanceStore::IndexInstanceCount() const {
    std::shared_lock lk(indexMutex_);
    return instances_.size();
}

// Scans one region for Instance objects. `This` lives at inst+8, so a slot at A
// is a hit when *A == A-8; Parent is then read straight out of the same chunk.
std::vector<std::pair<uintptr_t, uintptr_t>> InstanceStore::ScanHeapRegion(
    HANDLE h, uintptr_t base, size_t size) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    const int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    const int64_t descOff = OffsetOr("Instance", "ClassDescriptor", 24);
    const int64_t contOff = OffsetOr("Instance", "NameContainer", 112);
    if (parentOff < 8) return out;
    const size_t thisShift = 8;
    const size_t needSlots = 1 + (size_t)((parentOff - thisShift) / 8) + 1;
    const size_t validateSlots = 1 + (size_t)(std::max<int64_t>(descOff, contOff) / 8) + 1;
    const size_t requiredSlots = std::max(needSlots, validateSlots);
    constexpr size_t kChunk = 0x1000;
    std::vector<uint8_t> buf(kChunk);
    auto scan = [&](uintptr_t chunkBase, size_t chunk) {
        size_t n = chunk / 8;
        const uint64_t* q = (const uint64_t*)buf.data();
        for (size_t i = 0; i + requiredSlots < n; ++i) {
            uintptr_t self = chunkBase + i * 8;
            if (q[i] != self - thisShift) continue;
            uintptr_t inst = self - thisShift;
            uintptr_t parent = q[i + (size_t)((parentOff - thisShift) / 8)];
            if (parent && !IsReadablePtr(parent)) continue;
            uintptr_t desc = q[i - 1 + (size_t)(descOff / 8)];
            if (!IsReadablePtr(desc)) continue;
            uintptr_t cont = q[i - 1 + (size_t)(contOff / 8)];
            if (!IsReadablePtr(cont)) continue;
            out.emplace_back(parent, inst);
        }
    };
    // The heap is sparse (free/guard pages inside one allocation region), so a
    // large read often fails and would drop the whole window. Page-sized reads
    // succeed far more often and only cost one syscall per 4KiB.
    for (size_t off = 0; off + requiredSlots * 8 <= size; off += kChunk - requiredSlots * 8) {
        size_t chunk = std::min(kChunk, size - off);
        if (chunk < requiredSlots * 8) break;
        if (!mem_->CustomRead(base + off, buf.data(), chunk)) continue;
        scan(base + off, chunk);
    }
    (void)h;
    return out;
}

bool InstanceStore::BuildIndex(DWORD pid, bool force) {
    if (!pid) pid = indexPid_;
    if (!mem_ || !mem_->IsOpen() || !pid) return false;
    if (!force && IsIndexReady()) return true;
    indexPid_ = pid;
    const auto t0 = Clock::now();
    HANDLE h = mem_->Handle();
    if (!h) return false;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    if (hi > 0x0000800000000000ULL) hi = 0x0000800000000000ULL;

    struct Reg { uintptr_t base; size_t size; };
    std::vector<Reg> regs;
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, (LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        a = next;
        if (mbi.State != MEM_COMMIT || (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED)) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        DWORD prot = mbi.Protect & 0xFF;
        if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
              prot == PAGE_EXECUTE_WRITECOPY)) continue;
        if (mbi.RegionSize < 0x1000 || mbi.RegionSize > (SIZE_T)256 * 1024 * 1024) continue;
        regs.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize });
    }
    if (regs.empty()) return false;

    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw - 1u : 3u, 2u, 12u);
    std::atomic<size_t> cursor{ 0 };
    std::vector<std::vector<std::pair<uintptr_t, uintptr_t>>> results(regs.size());
    std::atomic<bool> failed{ false };

    auto worker = [&]() {
        for (;;) {
            size_t i = cursor.fetch_add(1);
            if (i >= regs.size() || failed.load(std::memory_order_relaxed)) return;
            results[i] = ScanHeapRegion(h, regs[i].base, regs[i].size);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    (void)failed;

    std::unordered_map<uintptr_t, std::vector<uintptr_t>> next;
    std::unordered_set<uintptr_t> all;
    size_t total = 0;
    for (auto& r : regs) total += r.size;
    next.reserve(1 << 16);
    all.reserve(1 << 17);
    for (auto& vec : results) {
        for (auto& [parent, child] : vec) {
            all.insert(child);
            if (parent) next[parent].push_back(child);
        }
    }
    
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    // Union with the previous snapshot. A pass can miss whole regions (reads
    // fail while the heap moves), and partial results are still correct data,
    // so merging keeps every child list monotonic instead of losing entries.
    size_t parents = 0;
    {
        std::unique_lock lk(indexMutex_);
        for (auto& [parent, kids] : next) {
            auto& slot = children_[parent];
            for (uintptr_t kid : kids) {
                bool dup = false;
                for (uintptr_t seen : slot)
                    if (seen == kid) { dup = true; break; }
                if (!dup) slot.push_back(kid);
            }
        }
        for (uintptr_t inst : all) {
            instances_.insert(inst);
            // a node in this pass that lists no children is a confirmed leaf
            if (children_.find(inst) == children_.end()) leafStamps_[inst] = GetTickCount64();
        }
        // drop leaves that gained children in this pass
        for (auto& [parent, kids] : children_) {
            if (!kids.empty()) leafStamps_.erase(parent);
        }
        parents = children_.size();
        indexReady_ = true;
        indexStamp_ = GetTickCount64();
        indexBytes_ = total;
        indexMs_ = ms;
        const size_t count = instances_.size();
        // report under the lock scope, then log below
        {
            std::unique_lock mk(metaMutex_);
            nameCache_.clear();
        }
        std::cout << "[index] " << count << " instances / " << parents << " parents from "
                  << (total >> 20) << " MiB in " << (int)ms << "ms (pass " << all.size() << ")"
                  << std::endl;
    }
    return true;
}

std::string InstanceStore::IndexStats(DWORD pid) const {
    std::shared_lock lk(indexMutex_);
    std::ostringstream os;
    os << "ready=" << (indexReady_ ? "yes" : "no")
       << " instances=" << instances_.size()
       << " parents=" << children_.size()
       << " scanned=" << (indexBytes_ >> 20) << "MiB"
       << " build=" << (long long)indexMs_ << "ms"
       << " age=" << (indexReady_ ? (GetTickCount64() - indexStamp_) : 0) << "ms"
       << " pid=" << pid;
    return os.str();
}

// ------------------------------------------------------------------ tree

// A snapshot is only trusted while it is fresh; older ones fall back to live
// reads so newly loaded objects are still reachable.
// Returns the children of `inst`, using the heap snapshot as a candidate list
// and confirming each candidate against its live Parent field. Nothing is
// returned from the snapshot alone, so a stale or partial snapshot can never
// hide or invent children.
std::vector<uintptr_t> InstanceStore::GetChildren(uintptr_t inst, bool allowScan) {
    std::vector<uintptr_t> out;
    if (!inst || !mem_) return out;
    const int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    auto hasParent = [&](uintptr_t c) {
        return ReadPtr(c + (uintptr_t)parentOff) == inst;
    };
    // dedupe + validate in one pass
    auto accept = [&](const std::vector<uintptr_t>& candidates) {
        for (uintptr_t c : candidates) {
            if (!IsReadablePtr(c)) continue;
            bool dup = false;
            for (uintptr_t seen : out)
                if (seen == c) { dup = true; break; }
            if (dup) continue;
            if (hasParent(c)) out.push_back(c);
        }
    };

    // 1) snapshot candidates (fast path, always verified against live memory)
    {
        std::shared_lock lk(indexMutex_);
        if (indexReady_) {
            auto it = children_.find(inst);
            if (it != children_.end()) accept(it->second);
        }
    }
    // 2) live children vector. Some services store the pair reversed, so accept
    //    either ordering. Always merged with the snapshot: a partial snapshot
    //    must not hide children the vector knows about, and vice versa.
    const int64_t startOff = OffsetOr("Instance", "ChildrenStart", 120);
    for (int d = 0; d < 6; ++d) {
        uintptr_t a = 0, b = 0;
        if (!mem_->CustomRead(inst + (uintptr_t)startOff + d * 8, &a, sizeof(a))) continue;
        if (!mem_->CustomRead(inst + (uintptr_t)startOff + d * 8 + 8, &b, sizeof(b))) continue;
        uintptr_t s = a, e = b;
        if (e < s) std::swap(s, e);
        if (!IsReadablePtr(s) || !IsReadablePtr(e)) continue;
        size_t n = (size_t)((e - s) / 8);
        if (n == 0 || n > 100000) continue;
        std::vector<uintptr_t> vec(n);
        if (!mem_->CustomRead(s, vec.data(), n * 8)) continue;
        accept(vec);
    }
    if (!out.empty()) return out;

    // 3) DataModel.Workspace is a direct pointer even when the vector is hidden
    if (IsValidDataModelCheap(inst)) {
        uintptr_t ws = ReadPtr(inst + (uintptr_t)OffsetOr("DataModel", "Workspace", 336));
        if (IsReadablePtr(ws) && hasParent(ws)) return { ws };
    }

    // 4) heap scan (direct lookups only), then cache so repeats are instant
    if (!allowScan) return out;
    auto scanned = ScanChildrenByParent(inst);
    for (uintptr_t c : scanned) {
        if (hasParent(c)) out.push_back(c);
    }
    {
        std::unique_lock lk(indexMutex_);
        children_[inst] = out;
        leafStamps_[inst] = GetTickCount64();
    }
    return out;
}

// Scans are cached per parent by GetChildren, so a miss costs one pass and
// every later lookup of that parent is instant.
uintptr_t InstanceStore::FindFirstChild(uintptr_t inst, const std::string& name) {
    for (auto c : GetChildren(inst))
        if (GetName(c) == name) return c;
    return DirectChildByName(inst, name);
}

uintptr_t InstanceStore::FindFirstChildOfClass(uintptr_t inst, const std::string& cls) {
    for (auto c : GetChildren(inst))
        if (GetClass(c) == cls) return c;
    return 0;
}

void InstanceStore::GetDescendants(uintptr_t inst, std::vector<uintptr_t>& out, int depth) {
    if (depth <= 0 || !inst) return;
    // Only the first level may trigger a heap scan; deeper walks use the
    // snapshot so a deep traversal cannot rescan per node.
    for (auto c : GetChildren(inst, depth >= 32)) {
        out.push_back(c);
        GetDescendants(c, out, depth - 1);
        if (out.size() > 100000) return;
    }
}

uintptr_t InstanceStore::WaitForChild(uintptr_t inst, const std::string& name, int ms) {
    const auto t0 = Clock::now();
    for (;;) {
        if (uintptr_t f = FindFirstChild(inst, name)) return f;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() >= ms) break;
        if (IsIndexReady()) BuildIndex(0, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    return 0;
}

uintptr_t InstanceStore::ResolvePath(uintptr_t root, const std::string& dotted) {
    uintptr_t cur = root;
    size_t s = 0;
    while (cur && s < dotted.size()) {
        size_t dot = dotted.find('.', s);
        std::string part = dotted.substr(s, dot == std::string::npos ? std::string::npos : dot - s);
        if (!part.empty()) {
            cur = FindFirstChild(cur, part);
            if (!cur) return 0;
        }
        if (dot == std::string::npos) break;
        s = dot + 1;
    }
    return cur;
}

// legacy single-parent scan (only used if the index is unavailable)
std::vector<uintptr_t> InstanceStore::ScanChildrenByParent(uintptr_t parent) {
    std::vector<uintptr_t> out;
    if (!mem_ || !mem_->IsOpen() || !parent) return out;
    HANDLE h = mem_->Handle();
    if (!h) return out;
    const int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::vector<std::pair<uintptr_t, uintptr_t>> pairs;
    uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    if (hi > 0x0000800000000000ULL) hi = 0x0000800000000000ULL;
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, (LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        a = next;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        DWORD prot = mbi.Protect & 0xFF;
        if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
              prot == PAGE_EXECUTE_WRITECOPY)) continue;
        if (mbi.RegionSize < 0x1000 || mbi.RegionSize > (SIZE_T)256 * 1024 * 1024) continue;
        auto found = ScanHeapRegion(h, (uintptr_t)mbi.BaseAddress, mbi.RegionSize);
        pairs.insert(pairs.end(), found.begin(), found.end());
    }
    for (auto& [p, child] : pairs) {
        if (p != parent) continue;
        if (ReadPtr(child + (uintptr_t)parentOff) == parent) out.push_back(child);
    }
    return out;
}

// ------------------------------------------------------------------ properties

namespace {

// Compact Roblox inheritance for property lookup (offsets.json is flat).
const std::unordered_map<std::string, std::string>& ParentMap() {
    static const std::unordered_map<std::string, std::string> map = {
        {"Instance","Instance"},{"PVInstance","Instance"},{"Model","PVInstance"},
        {"BasePart","PVInstance"},{"Part","BasePart"},{"WedgePart","Part"},
        {"TrussPart","BasePart"},{"SpawnLocation","Part"},{"Seat","Part"},
        {"VehicleSeat","Seat"},{"MeshPart","BasePart"},{"UnionOperation","BasePart"},
        {"Humanoid","PVInstance"},{"Tool","Item"},{"Item","PVInstance"},
        {"Accessory","PVInstance"},{"Clothing","PVInstance"},{"Decal","PVInstance"},
        {"Texture","PVInstance"},{"Script","LuaSourceContainer"},
        {"LuaSourceContainer","Instance"},{"LocalScript","LuaSourceContainer"},
        {"ModuleScript","LuaSourceContainer"},{"ClickDetector","Instance"},
        {"ProximityPrompt","GuiBase"},{"GuiBase","Instance"},{"GuiBase3d","Instance"},
        {"GuiBase2d","GuiBase3d"},{"LayerCollector","GuiBase3d"},{"GuiObject","LayerCollector"},
        {"Frame","GuiObject"},{"TextLabel","GuiObject"},{"TextButton","GuiObject"},
        {"TextBox","GuiObject"},{"ImageLabel","GuiObject"},{"ImageButton","GuiObject"},
        {"Player","PVInstance"},{"Team","PVInstance"},{"ForceField","PVInstance"},
        {"Attachment","PVInstance"},{"Weld","Instance"},{"Constraint","Instance"},
        {"AnimationTrack","Instance"},{"Animator","Instance"},{"Sound","Instance"},
        {"Lighting","PVInstance"},{"Sky","PVInstance"},{"Workspace","PVInstance"},
        {"Players","PVInstance"},{"ReplicatedStorage","PVInstance"},
        {"StarterGui","StarterPack"},{"StarterPack","PVInstance"},{"Lighting","PVInstance"},
        {"Terrain","PVInstance"},{"TerrainRegion","PVInstance"},{"Baseplate","Part"},
        {"Bone","Instance"},{"Motor6D","Instance"},{"WeldConstraint","Instance"},
        {"BillboardGui","LayerCollector"},{"SurfaceGui","GuiBase3d"},
        {"BodyMover","PVInstance"},{"BodyPosition","BodyMover"},{"BodyVelocity","BodyMover"},
        {"BodyGyro","BodyMover"},{"BodyThrust","BodyMover"},{"BodyForce","BodyMover"},
        {"AlignPosition","PVInstance"},{"AlignOrientation","PVInstance"},
        {"AngularVelocity","PVInstance"},{"LinearVelocity","PVInstance"},
        {"HumanoidRootPart","Part"},{"IntValue","ValueBase"},{"ValueBase","Instance"},
        {"NumberValue","ValueBase"},{"StringValue","ValueBase"},{"BoolValue","ValueBase"},
        {"Camera","PVInstance"},{"Selection","PVInstance"},{"SoundService","PVInstance"},
    };
    return map;
}

// Members stored as three consecutive floats.
const std::unordered_set<std::string>& Vec3Pairs() {
    static const std::unordered_set<std::string> set = {
        "Primitive.Position","Primitive.Size","Primitive.AssemblyLinearVelocity",
        "Primitive.AssemblyAngularVelocity","Attachment.Position","Attachment.Scale",
        "SpecialMesh.Scale","DataModelMesh.Scale","AirProperties.GlobalWind",
        "Humanoid.CameraOffset","Humanoid.MoveDirection","Humanoid.TargetPoint",
        "Humanoid.MoveToPoint","Humanoid.WalkDirection","Lighting.LightDirection",
        "Lighting.SunPosition","Lighting.MoonPosition","DragDetector.MaxDragTranslation",
        "DragDetector.MinDragTranslation","Sky.SkyboxOrientation","Ray.CFrame",
    };
    return set;
}

const std::unordered_set<std::string>& Color3Pairs() {
    static const std::unordered_set<std::string> set = {
        "Atmosphere.Color","Atmosphere.Decay","ColorCorrectionEffect.TintColor",
        "GuiObject.BackgroundColor3","GuiObject.BorderColor3","GuiObject.TextColor3",
        "BasePart.Color3","GuiBase3d.Color3","Color3Value.Value","BodyColors.HeadColor",
        "BodyColors.LeftArmColor3","BodyColors.RightLegColor3","SurfaceAppearance.Color",
        "SurfaceAppearance.EmissiveTint","Sparkles.SparkleColor","Light.Color",
        "Lighting.Ambient","Lighting.FogColor","Lighting.OutdoorAmbient","Lighting.ShadowColor",
        "Lighting.ColorShift_Top","Lighting.ColorShift_Bottom","Lighting.GradientTop",
        "Lighting.GradientBottom","Lighting.LightColor","Terrain.WaterColor",
        "Clothing.Color3","SelectionBox.SurfaceColor3","SelectionSphere.SurfaceColor3",
        "TextLabel.TextColor3","Frame.BackgroundColor3","BillboardGui.TextColor3",
        "ParticleEmitter.Color","Fire.Color","Smoke.Color","Beam.Color3",
    };
    return set;
}

const std::unordered_set<std::string>& RbxStringNames() {
    static const std::unordered_set<std::string> set = {
        "DisplayName","Name","JobId","ServerIP","ScriptGuid","GUID","Hash","Source",
        "LocaleId","MouseIcon","CursorIcon","ActivatedCursorIcon","SoundId","MeshId",
        "BaseTextureId","OverlayTextureId","Texture","Icon","Tooltip","Text","RichText",
        "Template","SkyboxBk","SkyboxDn","SkyboxFt","SkyboxLf","SkyboxRt","SkyboxUp",
        "SunTextureId","MoonTextureId","ColorMap","ColorMapContent","EmissiveMaskContent",
        "MetalnessMap","MetalnessMapContent","NormalMap","NormalMapContent","RoughnessMap",
        "RoughnessMapContent","EmissiveTint","LinkedSource","CachedRemoteSource",
        "ClipsDescendants","StateName","KeyName","AnimationId","WalkAnimation","RunAnimation",
        "IdleAnimation","JumpAnimation","FallAnimation","SwimAnimation","ClimbAnimation",
        "Shirt","Pants","GraphicTShirt","Face",
    };
    return set;
}

const std::unordered_set<std::string>& BoolNames() {
    static const std::unordered_set<std::string> set = {
        "Anchored","CanCollide","CanTouch","CanQuery","Locked","Massless","CastShadow",
        "Looped","IsPlaying","Enabled","Visible","Seated","Jump","Sit","PlatformStand",
        "RequiresNeck","EvaluateStateMachine","BreakJointsOnDeath","LocalPlayer",
        "GlobalWind","Frozen","Shadows","CloudsEnabled","AutoRotate","AutoJumpEnabled",
        "UseJumpPower","StreaksOn","CharacterAutoLoads","Neutral","Loaded",
        "Archivable","Robust","Persistent","KeepVelocity","AssemblyLinearVelocity",
        "TouchedGui","Grounded","Stiffness","Adornee","Bevel","ClipsDescendants",
        "ScrollBarInset","AutomaticSize","Interactable","Selectable","Draggable",
        "UseJumpPower","AutoLoadCharacter","HumanoidOnlySetCollisionsOnStateChange",
    };
    return set;
}

const std::unordered_set<std::string>& IntNames() {
    static const std::unordered_set<std::string> set = {
        "TeamColor","BrickColor","Size","Shape","Material","RigType","HealthDisplayType",
        "HealthDisplayDistance","NameDisplayDistance","NameOcclusion","DisplayDistanceType",
        "FloorMaterial","MaxSlopeAngle","CameraType","ZoomDistance","UserId","AccountAge",
        "PlaceId","GameId","CameraMode","CameraMinZoomDistance","CameraMaxZoomDistance",
        "TimeScale","Rotation","FontSize","TextSize","LayoutOrder","ZIndex","Priority",
        "PlaceVersion","JobId","MouseIcon","CameraMinZoom","SimulationRadius",
    };
    return set;
}

const std::unordered_set<std::string>& PtrNames() {
    static const std::unordered_set<std::string> set = {
        "Value","Primitive","Parent","Workspace","Team","SeatPart","HumanoidRootPart",
        "Texture","Script","LinkedSource","Adornee","Handle","MeshId","BaseTextureId",
        "OverlayTextureId","Template","SoundId","SkyboxBk","SkyboxDn","SkyboxFt","SkyboxLf",
        "SkyboxRt","SkyboxUp","SunTextureId","MoonTextureId","CameraSubject",
        "LocalPlayer","ModelInstance","Mouse","Icon","World","AirProperties","Character",
        "Animation","Animator","PrimaryPart","Part0","Part1","SoundGroup","ToolTip",
    };
    return set;
}

} // namespace

std::vector<std::string> InstanceStore::ClassChain(const std::string& cls) const {
    std::vector<std::string> chain;
    std::string cur = cls;
    for (int guard = 0; guard < 16 && !cur.empty(); ++guard) {
        chain.push_back(cur);
        auto it = ParentMap().find(cur);
        if (it == ParentMap().end() || it->second == cur) break;
        cur = it->second;
    }
    if (chain.empty()) chain.push_back(cls);
    return chain;
}

std::vector<std::string> InstanceStore::SubObjectClasses() const {
    // A sub-object class is any class name that also appears as a member name
    // (BasePart.Primitive -> class Primitive, Workspace.World -> class World).
    static std::vector<std::string> cached;
    static size_t cachedFor = 0;
    if (cachedFor == table_.size() && !cached.empty()) return cached;
    cached.clear();
    std::unordered_set<std::string> found;
    for (const auto& [c, members] : table_) {
        for (const auto& [member, off] : members) {
            (void)off;
            if (member == c) continue;
            if (table_.count(member)) found.insert(member);
        }
    }
    cached.assign(found.begin(), found.end());
    std::sort(cached.begin(), cached.end());
    cachedFor = table_.size();
    return cached;
}

PropInfo InstanceStore::ResolveProp(uintptr_t inst, const std::string& clsIn,
                                     const std::string& member) const {
    PropInfo out;
    std::string cls = clsIn;
    if (cls.empty() && inst) {
        // class names are cached, so this stays cheap
        cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    }
    if (cls.empty() || member.empty()) return out;

    auto chain = ClassChain(cls);
    auto typeFor = [&](const std::string& c, const std::string& m) {
        PropInfo p;
        const std::string key = c + "." + m;
        if (Vec3Pairs().count(key)) p.type = PropType::Vec3;
        else if (Color3Pairs().count(key)) p.type = PropType::C3;
        else if (RbxStringNames().count(m)) p.type = PropType::RbxString;
        else if (BoolNames().count(m)) p.type = PropType::Bool;
        else if (PtrNames().count(m)) p.type = PropType::Ptr;
        else if (IntNames().count(m)) p.type = PropType::I32;
        else p.type = PropType::F32;
        return p;
    };

    // CFrame is not a field of its own: Primitive stores a 3x3 rotation
    // (9 floats) immediately followed by the position (3 floats). offsets.json
    // is authoritative, so the block is only accepted when the two entries it
    // reports are actually adjacent - otherwise there is no contiguous CFrame.
    if (member == "CFrame") {
        auto rot = OffsetOf("Primitive", "Rotation");
        auto pos = OffsetOf("Primitive", "Position");
        if (rot && pos && *pos == *rot + (int64_t)(sizeof(float) * 9)) {
            for (const auto& c : chain) {
                auto hop = OffsetOf(c, "Primitive");
                if (!hop) continue;
                out.type = PropType::CF;
                out.offset = *rot;
                out.hops = { *hop };
                return out;
            }
        }
    }

    // 1) direct field on the class or an ancestor
    for (const auto& c : chain) {
        auto o = OffsetOf(c, member);
        if (!o) continue;
        out = typeFor(c, member);
        out.offset = *o;
        out.hops.clear();
        return out;
    }

    // 2) sub-object chains: Part -> BasePart.Primitive -> Primitive.Position,
    //    Workspace -> World -> World.Gravity. Each hop is a pointer member.
    {
        const auto subs = SubObjectClasses();
        std::vector<int64_t> hops;
        std::vector<std::string> frontier = chain;
        for (int depth = 0; depth < 3 && !frontier.empty(); ++depth) {
            std::vector<std::string> next;
            for (const auto& c : frontier) {
                for (const auto& sub : subs) {
                    auto hop = OffsetOf(c, sub);
                    if (!hop) continue;
                    auto field = OffsetOf(sub, member);
                    if (!field) continue;
                    PropInfo p = typeFor(sub, member);
                    p.offset = *field;
                    p.hops = hops;
                    p.hops.push_back(*hop);
                    return p;
                }
            }
            for (const auto& c : frontier) {
                for (const auto& sub : subs) {
                    if (!OffsetOf(c, sub)) continue;
                    auto hop = OffsetOf(c, sub);
                    hops.push_back(*hop);
                    next.push_back(sub);
                    break;
                }
            }
            frontier = next;
        }
    }

    // 3) unique global match (member exists on exactly one class)
    {
        const std::string* owner = nullptr;
        int count = 0;
        for (const auto& [c, members] : table_) {
            if (!members.count(member)) continue;
            ++count;
            owner = &c;
        }
        if (count == 1 && owner) {
            out = typeFor(*owner, member);
            out.offset = *OffsetOf(*owner, member);
            out.hops.clear();
            return out;
        }
    }
    return out;
}

bool InstanceStore::HasProp(const std::string& cls, const std::string& member) const {
    return ResolveProp(0, cls, member).ok();
}

uintptr_t InstanceStore::PropBase(const PropInfo& p, uintptr_t inst) const {
    if (!p.ok() || !inst) return 0;
    uintptr_t cur = inst;
    for (int64_t hop : p.hops) {
        uintptr_t next = 0;
        if (!mem_->CustomRead(cur + (uintptr_t)hop, &next, sizeof(next))) return 0;
        if (!IsReadablePtr(next)) return 0;
        cur = next;
    }
    return cur;
}

bool InstanceStore::ReadPropFloat(const PropInfo& p, uintptr_t inst, float& out) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, &out, sizeof(float));
}
bool InstanceStore::WritePropFloat(const PropInfo& p, uintptr_t inst, float v) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->Write<float>(base + (uintptr_t)p.offset, v);
}
bool InstanceStore::ReadPropVec3(const PropInfo& p, uintptr_t inst, float out[3]) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, out, sizeof(float) * 3);
}
bool InstanceStore::WritePropVec3(const PropInfo& p, uintptr_t inst, const float v[3]) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomWrite(base + (uintptr_t)p.offset, v, sizeof(float) * 3);
}
bool InstanceStore::ReadPropCFrame(const PropInfo& p, uintptr_t inst, float out[12]) const {
    if (!p.ok() || p.type != PropType::CF || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, out, sizeof(float) * 12);
}
bool InstanceStore::WritePropCFrame(const PropInfo& p, uintptr_t inst, const float v[12]) const {
    if (!p.ok() || p.type != PropType::CF || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomWrite(base + (uintptr_t)p.offset, v, sizeof(float) * 12);
}
std::string InstanceStore::ReadPropString(const PropInfo& p, uintptr_t inst) const {
    if (!p.ok() || !inst) return "";
    uintptr_t base = PropBase(p, inst);
    if (!base) return "";
    return ReadRobloxString(base + (uintptr_t)p.offset, 128);
}

// ------------------------------------------------------------------ DataModel

bool InstanceStore::IsValidDataModelCheap(uintptr_t dm) {
    if (!IsReadablePtr(dm)) return false;
    uintptr_t ws = ReadPtr(dm + (uintptr_t)OffsetOr("DataModel", "Workspace", 336));
    if (!IsReadablePtr(ws)) return false;
    if (ReadPtr(ws + (uintptr_t)OffsetOr("Instance", "Parent", 104)) != dm) return false;
    return true;
}

bool InstanceStore::IsValidDataModel(uintptr_t dm) { return IsValidDataModelCheap(dm); }

uintptr_t InstanceStore::TryFakePointer(DWORD, uintptr_t base) {
    auto fakePtrOff = OffsetOf("FakeDataModel", "Pointer");
    auto realOff = OffsetOf("FakeDataModel", "RealDataModel");
    if (!fakePtrOff || !realOff) return 0;
    uintptr_t fake = ReadPtr(base + (uintptr_t)*fakePtrOff);
    if (!IsReadablePtr(fake)) return 0;
    uintptr_t real = ReadPtr(fake + (uintptr_t)*realOff);
    if (IsValidDataModelCheap(real)) return real;
    if (IsValidDataModelCheap(fake)) return fake;
    return 0;
}

uintptr_t InstanceStore::TryVisualEngine(DWORD, uintptr_t base) {
    auto vePtrOff = OffsetOf("VisualEngine", "Pointer");
    auto veFakeOff = OffsetOf("VisualEngine", "FakeDataModel");
    auto realOff = OffsetOf("FakeDataModel", "RealDataModel");
    if (!vePtrOff || !veFakeOff || !realOff) return 0;
    uintptr_t ve = ReadPtr(base + (uintptr_t)*vePtrOff);
    if (!IsReadablePtr(ve)) return 0;
    uintptr_t fake = ReadPtr(ve + (uintptr_t)*veFakeOff);
    if (!IsReadablePtr(fake)) return 0;
    uintptr_t real = ReadPtr(fake + (uintptr_t)*realOff);
    return IsValidDataModelCheap(real) ? real : 0;
}

uintptr_t InstanceStore::TryTaskScheduler(DWORD, uintptr_t base) {
    auto tsPtrOff = OffsetOf("TaskScheduler", "Pointer");
    auto startOff = OffsetOf("TaskScheduler", "JobStart");
    auto endOff = OffsetOf("TaskScheduler", "JobEnd");
    auto renderRealOff = OffsetOf("RenderJob", "RealDataModel");
    if (!tsPtrOff || !startOff || !endOff || !renderRealOff) return 0;
    uintptr_t ts = ReadPtr(base + (uintptr_t)*tsPtrOff);
    if (!IsReadablePtr(ts)) return 0;
    uintptr_t start = ReadPtr(ts + (uintptr_t)*startOff);
    uintptr_t end = ReadPtr(ts + (uintptr_t)*endOff);
    if (!IsReadablePtr(start) || !IsReadablePtr(end) || end <= start) return 0;
    size_t n = (size_t)((end - start) / 8);
    if (n == 0 || n > 500) return 0;
    std::vector<uintptr_t> jobs(n);
    if (!mem_->CustomRead(start, jobs.data(), n * 8)) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (!IsReadablePtr(jobs[i])) continue;
        uintptr_t real = ReadPtr(jobs[i] + (uintptr_t)*renderRealOff);
        if (IsValidDataModelCheap(real)) return real;
    }
    return 0;
}

uintptr_t InstanceStore::GetDataModel(DWORD pid) {
    if (manualDm_ && IsValidDataModelCheap(manualDm_)) return manualDm_;
    if (cachedDm_ && IsValidDataModelCheap(cachedDm_)) return cachedDm_;
    cachedDm_ = 0;
    if (!mem_ || !mem_->IsOpen() || !pid) return 0;
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleInfo(pid, "RobloxPlayerBeta.exe", base, size) || !base) return 0;
    std::cout << "[datamodel] base=0x" << std::hex << base << " size=0x" << size << std::dec
              << std::endl;
    if (uintptr_t dm = TryFakePointer(pid, base)) { cachedDm_ = dm; return dm; }
    if (uintptr_t dm = TryVisualEngine(pid, base)) { cachedDm_ = dm; return dm; }
    if (uintptr_t dm = TryTaskScheduler(pid, base)) { cachedDm_ = dm; return dm; }
    std::cout << "[datamodel] all chains failed" << std::endl;
    return 0;
}

uintptr_t InstanceStore::GetWorkspace(DWORD pid) {
    uintptr_t dm = GetDataModel(pid);
    if (!dm) return 0;
    uintptr_t ws = ReadPtr(dm + (uintptr_t)OffsetOr("DataModel", "Workspace", 336));
    return IsReadablePtr(ws) ? ws : 0;
}

uintptr_t InstanceStore::DirectChildByName(uintptr_t parent, const std::string& name) {
    // Named lookups do not need the whole child list: probe the children vector
    // slots and match names one at a time.
    if (!parent || !mem_) return 0;
    const int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    const int64_t startOff = OffsetOr("Instance", "ChildrenStart", 120);
    for (int d = 0; d < 6; ++d) {
        uintptr_t a = 0, b = 0;
        if (!mem_->CustomRead(parent + (uintptr_t)startOff + d * 8, &a, sizeof(a))) continue;
        if (!mem_->CustomRead(parent + (uintptr_t)startOff + d * 8 + 8, &b, sizeof(b))) continue;
        uintptr_t s = a, e = b;
        if (e < s) std::swap(s, e);
        if (!IsReadablePtr(s) || !IsReadablePtr(e)) continue;
        size_t n = (size_t)((e - s) / 8);
        if (n == 0 || n > 100000) continue;
        for (size_t i = 0; i < n; ++i) {
            uintptr_t p2 = 0;
            if (!mem_->CustomRead(s + i * 8, &p2, sizeof(p2))) break;
            if (!IsReadablePtr(p2)) continue;
            if (GetName(p2) == name) return p2;
        }
    }
    return 0;
}

// GetChildren caches per parent, so resolving a service does not need a full
// snapshot rebuild (which is what made first lookup take seconds).
uintptr_t InstanceStore::GetService(DWORD pid, const std::string& name) {
    uintptr_t dm = GetDataModel(pid);
    if (!dm) return 0;
    if (uintptr_t f = DirectChildByName(dm, name)) return f;
    return FindFirstChild(dm, name);
}

// Player containers hide their children vector, so the cache fills from a
// targeted scan of the heap on first use (a few seconds once, then instant).
std::vector<uintptr_t> InstanceStore::GetPlayers(DWORD pid) {
    std::vector<uintptr_t> out;
    uintptr_t players = GetService(pid, "Players");
    if (!players) return out;
    const int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    for (uintptr_t c : GetChildren(players)) {
        if (!IsReadablePtr(c)) continue;
        if (ReadPtr(c + (uintptr_t)parentOff) != players) continue;
        std::string cls = GetClass(c);
        if (cls.empty() || cls == "Player") out.push_back(c);
    }
    return out;
}

uintptr_t InstanceStore::GetLocalPlayer(DWORD pid) {
    // Players.LocalPlayer is the authoritative pointer.
    uintptr_t players = GetService(pid, "Players");
    if (!players) return 0;
    const int64_t lpOff = OffsetOr("Player", "LocalPlayer", 288);
    uintptr_t lp = ReadPtr(players + (uintptr_t)lpOff);
    if (IsReadablePtr(lp) && ReadPtr(lp + 8) == lp) return lp;
    // Fallback: the per-player flag at the same offset.
    for (auto p : GetPlayers(pid)) {
        uint8_t flag = 0;
        if (mem_->CustomRead(p + (uintptr_t)lpOff, &flag, 1) && flag) return p;
    }
    return 0;
}

uintptr_t InstanceStore::GetCharacter(DWORD pid) {
    uintptr_t lp = GetLocalPlayer(pid);
    if (lp) {
        uintptr_t ch = ReadPtr(lp + (uintptr_t)OffsetOr("Player", "ModelInstance", 648));
        if (IsReadablePtr(ch) && ReadPtr(ch + 8) == ch) return ch;
    }
    uintptr_t ws = GetWorkspace(pid);
    if (!ws) return 0;
    if (uintptr_t m = FindFirstChildOfClass(ws, "Model")) return m;
    return 0;
}

uintptr_t InstanceStore::GetHumanoid(DWORD pid) {
    uintptr_t ch = GetCharacter(pid);
    if (!ch) return 0;
    if (uintptr_t h = FindFirstChildOfClass(ch, "Humanoid")) return h;
    return FindFirstChild(ch, "Humanoid");
}

// HumanoidRootPart is not always a direct child (R6 nests it under Torso),
// so search one level deeper when the fast paths miss.
uintptr_t InstanceStore::GetRootPart(DWORD pid) {
    uintptr_t ch = GetCharacter(pid);
    if (!ch) return 0;
    if (uintptr_t rp = FindFirstChild(ch, "HumanoidRootPart")) return rp;
    for (auto limb : GetChildren(ch)) {
        if (uintptr_t rp = FindFirstChild(limb, "HumanoidRootPart")) return rp;
    }
    // last resort: first Part that holds the Humanoid
    for (auto limb : GetChildren(ch)) {
        if (GetClass(limb) == "Part") return limb;
    }
    return 0;
}

bool InstanceStore::ModuleInfo(DWORD pid, const std::string& mod, uintptr_t& base, size_t& size) {
    base = 0; size = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    bool ok = false;
    if (Module32First(snap, &me)) do {
#ifdef UNICODE
        std::wstring w(mod.begin(), mod.end());
        if (_wcsicmp(me.szModule, w.c_str()) == 0) {
#else
        if (_stricmp(me.szModule, mod.c_str()) == 0) {
#endif
            base = (uintptr_t)me.modBaseAddr; size = me.modBaseSize; ok = true; break;
        }
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    return ok;
}
uintptr_t InstanceStore::ModuleBase(DWORD pid, const std::string& mod) {
    uintptr_t b = 0; size_t s = 0;
    ModuleInfo(pid, mod, b, s);
    return b;
}