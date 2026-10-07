#include "instance.h"
#include "memory.h"
#include "log.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <tlhelp32.h>
#include <immintrin.h>
#include <nlohmann/json.hpp>

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ offsets

static std::string LowerKey(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

void InstanceStore::RefreshCachedOffsets() {
    auto pick = [&](const char* c, const char* m, int64_t fb) -> int64_t {
        auto o = OffsetOf(c, m);
        return o ? *o : fb;
    };
    oParent_ = pick("Instance", "Parent", 104);
    oDesc_ = pick("Instance", "ClassDescriptor", 24);
    oCont_ = pick("Instance", "NameContainer", 112);
    oName_ = pick("Instance", "Name", 8);
    oClassName_ = pick("Instance", "ClassName", 8);
    oChildrenStart_ = pick("Instance", "ChildrenStart", 120);
    oWorkspace_ = pick("DataModel", "Workspace", 336);
    oLocalPlayer_ = pick("Player", "LocalPlayer", 288);
    oModelInstance_ = pick("Player", "ModelInstance", 648);
    // Sub-object classes: any member name that is also a class name.
    // Computed once here (was rebuilt + copied per ResolveProp miss).
    subClasses_.clear();
    {
        std::unordered_set<std::string> found;
        found.reserve(table_.size());
        for (const auto& [c, members] : table_) {
            for (const auto& [member, off] : members) {
                (void)off;
                if (member == c) continue;
                if (table_.find(member) != table_.end()) found.insert(member);
            }
        }
        subClasses_.assign(found.begin(), found.end());
        std::sort(subClasses_.begin(), subClasses_.end());
    }
}

bool InstanceStore::Load(const std::string& path) {
    table_.clear();
    lowerTable_.clear();
    { std::unique_lock lk(propMutex_); propCache_.clear(); }
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
        // Case-insensitive mirror of the whole table, built once here so that a
        // spelling difference between the dump and the runtime costs one hash
        // lookup instead of scanning every class and member.
        lowerTable_.reserve(table_.size() * 2);
        for (const auto& [cls, members] : table_) {
            auto& dst = lowerTable_[LowerKey(cls)];
            dst.reserve(members.size());
            for (const auto& [m, off] : members)
                dst.emplace(LowerKey(m), OffsetLookup{ cls, m, off });
        }
        loaded_ = true;
        RefreshCachedOffsets();
        { std::unique_lock lk(propMutex_); propCache_.reserve(1u << 14); }
        { std::unique_lock lk(metaMutex_); nameCache_.reserve(1u << 17); }
        return true;
    } catch (...) { return false; }
}

std::optional<OffsetLookup> InstanceStore::LookupLower(const std::string& cls,
                                                       const std::string& member) const {
    auto it = lowerTable_.find(LowerKey(cls));
    if (it == lowerTable_.end()) return std::nullopt;
    auto jt = it->second.find(LowerKey(member));
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}

std::string InstanceStore::CacheKey(const std::string& cls, const std::string& member) {
    std::string k;
    k.reserve(cls.size() + member.size() + 1);
    k.append(cls).push_back('\x1f');
    k.append(member);
    return k;
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
      lowerTable_(std::move(o.lowerTable_)), propCache_(std::move(o.propCache_)),
      manualDm_(o.manualDm_), cachedDm_(o.cachedDm_),
      oParent_(o.oParent_), oDesc_(o.oDesc_), oCont_(o.oCont_),
      oName_(o.oName_), oClassName_(o.oClassName_),
      oChildrenStart_(o.oChildrenStart_), oWorkspace_(o.oWorkspace_),
      oLocalPlayer_(o.oLocalPlayer_), oModelInstance_(o.oModelInstance_),
      children_(std::move(o.children_)),
      instances_(std::move(o.instances_)),
      indexReady_(o.indexReady_), indexStamp_(o.indexStamp_), indexPid_(o.indexPid_),
      indexBytes_(o.indexBytes_), indexMs_(o.indexMs_),
      subClasses_(std::move(o.subClasses_)),
      nameCache_(std::move(o.nameCache_)) {
    indexBuilding_.store(false, std::memory_order_relaxed);
    indexCancel_.store(false, std::memory_order_relaxed);
    lastSweepMs_.store(o.lastSweepMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    lastIndexRebuildMs_.store(o.lastIndexRebuildMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

InstanceStore& InstanceStore::operator=(InstanceStore&& o) noexcept {
    if (this == &o) return *this;
    mem_ = o.mem_; loaded_ = o.loaded_; table_ = std::move(o.table_);
    lowerTable_ = std::move(o.lowerTable_);
    {
        std::unique_lock lk(propMutex_);
        std::unique_lock lk2(o.propMutex_);
        propCache_ = std::move(o.propCache_);
    }
    manualDm_ = o.manualDm_; cachedDm_ = o.cachedDm_;
    oParent_ = o.oParent_; oDesc_ = o.oDesc_; oCont_ = o.oCont_;
    oName_ = o.oName_; oClassName_ = o.oClassName_;
    oChildrenStart_ = o.oChildrenStart_; oWorkspace_ = o.oWorkspace_;
    oLocalPlayer_ = o.oLocalPlayer_; oModelInstance_ = o.oModelInstance_;
    children_ = std::move(o.children_);
    instances_ = std::move(o.instances_);
    indexReady_ = o.indexReady_; indexStamp_ = o.indexStamp_; indexPid_ = o.indexPid_;
    indexBytes_ = o.indexBytes_; indexMs_ = o.indexMs_;
    subClasses_ = std::move(o.subClasses_);
    lastSweepMs_.store(o.lastSweepMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    lastIndexRebuildMs_.store(o.lastIndexRebuildMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    indexBuilding_.store(false, std::memory_order_relaxed);
    indexCancel_.store(false, std::memory_order_relaxed);
    {
        std::unique_lock mk(metaMutex_);
        std::unique_lock mk2(o.metaMutex_);
        nameCache_ = std::move(o.nameCache_);
    }
    return *this;
}

// ------------------------------------------------------------------ raw reads

uintptr_t InstanceStore::ReadPtr(uintptr_t a) {
    uintptr_t v = 0;
    if (!mem_ || !a || !mem_->CustomRead(a, &v, sizeof(v))) return 0;
    return v;
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
    const int64_t nameOff = oName_;
    const int64_t containerOff = oCont_;
    std::string name;
    uintptr_t container = ReadPtr(inst + (uintptr_t)containerOff);
    if (FastReadable(container)) {
        name = ReadRobloxString(container + (uintptr_t)nameOff);
        if (name.empty()) name = ReadRobloxString(container);
    }
    if (name.empty()) name = ReadRobloxString(inst + (uintptr_t)containerOff);
    if (name.empty()) {
        uintptr_t p = ReadPtr(inst + (uintptr_t)nameOff);
        if (FastReadable(p)) name = ReadRobloxString(p);
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
    const int64_t descOff = oDesc_;
    const int64_t cnOff = oClassName_;
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
    if (FastReadable(desc)) {
        cls = ReadRobloxString(desc + (uintptr_t)cnOff);
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (FastReadable(p)) cls = ReadRobloxString(p);
        }
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (FastReadable(p)) {
                uintptr_t p2 = ReadPtr(p);
                if (FastReadable(p2)) cls = ReadRobloxString(p2);
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
    uintptr_t p = ReadPtr(inst + (uintptr_t)oParent_);
    return FastReadable(p) ? p : 0;
}

bool InstanceStore::IsValidInstance(uintptr_t inst) {
    if (!FastReadable(inst)) return false;
    if (ReadPtr(inst + 8) != inst) return false;                     // This
    if (!FastReadable(ReadPtr(inst + (uintptr_t)oDesc_))) return false;
    if (!FastReadable(ReadPtr(inst + (uintptr_t)oCont_))) return false;
    return true;
}

// ------------------------------------------------------------------ index

void InstanceStore::InvalidateIndex() {
    std::unique_lock lk(indexMutex_);
    children_.clear();
    instances_.clear();
    indexReady_ = false;
    indexStamp_ = 0;
    indexBytes_ = 0;
    indexMs_ = 0;
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
// Hot loop: 1 MiB windows (4x fewer syscalls than 256 KiB), 4x-unrolled scalar
// scan with prefetch, 2-level fallback (chunk -> 4K pages, no 64K middle that
// re-read the same bytes a third time). Offsets are passed in: the old per-
// region OffsetOr cost 6 string hashes per region per thread.
std::vector<std::pair<uintptr_t, uintptr_t>> InstanceStore::ScanHeapRegion(
    HANDLE h, uintptr_t base, size_t size,
    int64_t parentOff, int64_t descOff, int64_t contOff) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    if (parentOff < 8) return out;
    out.reserve(size >> 12); // ~1 hit per 4K is a generous cap; avoids regrow
    const size_t thisShift = 8;
    const size_t needSlots = 1 + (size_t)((parentOff - (int64_t)thisShift) / 8) + 1;
    const size_t validateSlots = 1 + (size_t)(std::max<int64_t>(descOff, contOff) / 8) + 1;
    const size_t requiredSlots = std::max(needSlots, validateSlots);
    constexpr size_t kPage = 0x1000;
    // 256 KiB windows: under heap churn (streaming map) a big window almost
    // always contains a changed/guarded page and fails wholesale; small
    // windows fail narrowly and the retry below covers transient races.
    constexpr size_t kChunk = 0x40000;
    if (requiredSlots * 8 >= kPage) return out;
    const size_t reqBytes = requiredSlots * 8;
    const size_t pageStride = kPage - reqBytes;
    const size_t parentIdx = (size_t)((parentOff - (int64_t)thisShift) / 8);
    const size_t descBase = (size_t)(descOff / 8); // inst-relative slot
    const size_t contBase = (size_t)(contOff / 8);
    thread_local std::vector<uint8_t> buf;
    if (buf.size() < kChunk) buf.resize(kChunk);
    auto scan = [&](uintptr_t chunkBase, size_t chunk) {
        const size_t n = chunk / 8;
        const uint64_t* __restrict q = (const uint64_t*)buf.data();
        const size_t limit = (n > requiredSlots) ? n - requiredSlots : 0;
        // Start at 1: slot 0 has no inst slot (i-1 underflows); the old loop
        // read out of bounds on a slot-0 hit.
        size_t i = 1;
        // 4x unroll: 4 independent chains, prefetched. The expected value is
        // linear (chunkBase-8+i*8) so no table lookup; mispredicts collapse to
        // the single != continue.
        for (; i + 4 <= limit; i += 4) {
            _mm_prefetch((const char*)(q + i + 64), _MM_HINT_T0);
            const uint64_t e0 = (uint64_t)(chunkBase + i * 8 - thisShift);
            const uint64_t e1 = (uint64_t)(chunkBase + (i + 1) * 8 - thisShift);
            const uint64_t e2 = (uint64_t)(chunkBase + (i + 2) * 8 - thisShift);
            const uint64_t e3 = (uint64_t)(chunkBase + (i + 3) * 8 - thisShift);
            if (q[i] == e0) {
                uintptr_t parent = (uintptr_t)q[i + parentIdx];
                uintptr_t desc = (uintptr_t)q[i - 1 + descBase];
                uintptr_t cont = (uintptr_t)q[i - 1 + contBase];
                if ((!parent || FastReadable(parent)) && FastReadable(desc) && FastReadable(cont))
                    out.emplace_back(parent, (uintptr_t)(chunkBase + i * 8 - thisShift));
            }
            if (q[i + 1] == e1) {
                uintptr_t parent = (uintptr_t)q[i + 1 + parentIdx];
                uintptr_t desc = (uintptr_t)q[i + 1 - 1 + descBase];
                uintptr_t cont = (uintptr_t)q[i + 1 - 1 + contBase];
                if ((!parent || FastReadable(parent)) && FastReadable(desc) && FastReadable(cont))
                    out.emplace_back(parent, (uintptr_t)(chunkBase + (i + 1) * 8 - thisShift));
            }
            if (q[i + 2] == e2) {
                uintptr_t parent = (uintptr_t)q[i + 2 + parentIdx];
                uintptr_t desc = (uintptr_t)q[i + 2 - 1 + descBase];
                uintptr_t cont = (uintptr_t)q[i + 2 - 1 + contBase];
                if ((!parent || FastReadable(parent)) && FastReadable(desc) && FastReadable(cont))
                    out.emplace_back(parent, (uintptr_t)(chunkBase + (i + 2) * 8 - thisShift));
            }
            if (q[i + 3] == e3) {
                uintptr_t parent = (uintptr_t)q[i + 3 + parentIdx];
                uintptr_t desc = (uintptr_t)q[i + 3 - 1 + descBase];
                uintptr_t cont = (uintptr_t)q[i + 3 - 1 + contBase];
                if ((!parent || FastReadable(parent)) && FastReadable(desc) && FastReadable(cont))
                    out.emplace_back(parent, (uintptr_t)(chunkBase + (i + 3) * 8 - thisShift));
            }
        }
        for (; i < limit; ++i) {
            if (q[i] != (uint64_t)(chunkBase + i * 8 - thisShift)) continue;
            uintptr_t parent = (uintptr_t)q[i + parentIdx];
            if (parent && !FastReadable(parent)) continue;
            uintptr_t desc = (uintptr_t)q[i - 1 + descBase];
            if (!FastReadable(desc)) continue;
            uintptr_t cont = (uintptr_t)q[i - 1 + contBase];
            if (!FastReadable(cont)) continue;
            out.emplace_back(parent, (uintptr_t)(chunkBase + i * 8 - thisShift));
        }
    };
    for (size_t off = 0; off + reqBytes <= size; off += kChunk - reqBytes) {
        size_t chunk = std::min(kChunk, size - off);
        if (chunk < reqBytes) break;
        // One retry: the target heap churns while we read (streaming), so a
        // transient failure is retried before paying page-by-page fallback.
        if (mem_->CustomRead(base + off, buf.data(), chunk) ||
            mem_->CustomRead(base + off, buf.data(), chunk)) {
            scan(base + off, chunk);
            continue;
        }
        // Sparse fallback: straight to pages. The old 64 KiB middle tier
        // re-read every byte a second time before paging; pages alone cover
        // guard-split regions with fewer total syscalls.
        for (size_t p = 0; p + reqBytes <= chunk; p += pageStride) {
            size_t pg = std::min(kPage, chunk - p);
            if (pg < reqBytes) break;
            if (indexCancel_.load(std::memory_order_relaxed)) return out;
            if (mem_->CustomRead(base + off + p, buf.data(), pg))
                scan(base + off + p, pg);
        }
    }
    (void)h;
    return out;
}

bool InstanceStore::BuildIndex(DWORD pid, bool force) {
    if (!pid) pid = indexPid_;
    if (!mem_ || !mem_->IsOpen() || !pid) return false;
    if (!force && IsIndexReady()) return true;
    // One pass at a time: a second caller (WaitForChild, a rescan, a second
    // startup) just observes instead of stacking another 2 GiB sweep.
    bool expected = false;
    if (!indexBuilding_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return IsIndexReady();
    struct BuildGuard {
        std::atomic<bool>& flag;
        ~BuildGuard() { flag.store(false, std::memory_order_release); }
    } guard{ indexBuilding_ };
    indexCancel_.store(false, std::memory_order_relaxed);
    indexPid_ = pid;
    const auto t0 = Clock::now();
    HANDLE h = mem_->Handle();
    if (!h) return false;

    // FAST PATH: level-parallel BFS from DataModel via live children vectors.
    // 2 bulk reads per node, zero per-child verification, zero heap transfer.
    // Each depth level fans out over all cores (results[i] = children of
    // frontier[i], merged at the level barrier), so a 120k-node tree costs
    // ~240k small reads at 16-wide parallelism instead of serially.
    // Vectors are authoritative live data; the heap sweep below unions in
    // hidden vectors (Workspace/Players map content) afterwards.
    {
        uintptr_t dm = GetDataModel(pid);
        if (dm && !indexCancel_.load(std::memory_order_relaxed)) {
            const auto tf0 = Clock::now();
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> fast;
            std::unordered_set<uintptr_t> visited;
            fast.reserve(1 << 15);
            visited.reserve(1 << 17);
            std::vector<uintptr_t> frontier;
            frontier.reserve(1 << 16);
            frontier.push_back(dm);
            visited.insert(dm);
            // Workspace ptr even when vector hidden
            {
                uintptr_t ws = ReadPtr(dm + (uintptr_t)oWorkspace_);
                if (FastReadable(ws) && visited.insert(ws).second)
                    frontier.push_back(ws);
            }
            unsigned thw = std::thread::hardware_concurrency();
            unsigned nthreads = std::clamp(thw ? thw : 8u, 4u, 16u);
            std::vector<uintptr_t> next;
            next.reserve(1 << 16);
            while (!frontier.empty()) {
                if (indexCancel_.load(std::memory_order_relaxed)) break;
                if (visited.size() > 120000) break;
                const size_t F = frontier.size();
                if (F < nthreads) nthreads = (unsigned)F;
                std::vector<std::vector<uintptr_t>> results(F);
                std::atomic<size_t> cur{ 0 };
                const int64_t csOff = oChildrenStart_;
                Memory* m = mem_;
                std::vector<std::thread> workers;
                workers.reserve(nthreads);
                for (unsigned t = 0; t < nthreads; ++t) {
                    workers.emplace_back([&, t]() {
                        thread_local std::vector<uintptr_t> buf;
                        if (buf.capacity() < 2048) buf.reserve(2048);
                        uintptr_t w[7];
                        for (;;) {
                            size_t i = cur.fetch_add(1, std::memory_order_relaxed);
                            if (i >= F || indexCancel_.load(std::memory_order_relaxed)) return;
                            uintptr_t node = frontier[i];
                            if (!m->CustomRead(node + (uintptr_t)csOff, w, sizeof(w)))
                                continue;
                            for (int d = 0; d < 6; ++d) {
                                uintptr_t s = w[d], e = w[d + 1];
                                if (e < s) std::swap(s, e);
                                if (s <= 0x10000 || e >= 0x0000800000000000ULL) continue;
                                if ((s & 7) || (e & 7)) continue;
                                size_t n = (size_t)((e - s) / 8);
                                if (n == 0 || n > 2048) continue;
                                if (buf.capacity() < n) buf.reserve(n);
                                buf.resize(n);
                                if (!m->CustomRead(s, buf.data(), n * 8)) continue;
                                auto& out = results[i];
                                if (out.capacity() < out.size() + n)
                                    out.reserve(out.size() + n);
                                for (size_t k = 0; k < n; ++k) {
                                    uintptr_t c = buf[k];
                                    if (c <= 0x10000 || c >= 0x0000800000000000ULL) continue;
                                    if (c & 7) continue;
                                    out.push_back(c);
                                }
                            }
                        }
                    });
                }
                for (auto& t : workers) t.join();
                if (indexCancel_.load(std::memory_order_relaxed)) break;
                next.clear();
                for (size_t i = 0; i < F; ++i) {
                    if (results[i].empty()) continue;
                    uintptr_t node = frontier[i];
                    auto& slot = fast[node];
                    for (uintptr_t c : results[i]) {
                        slot.push_back(c);
                        if (visited.insert(c).second) {
                            if (visited.size() > 120000) break;
                            next.push_back(c);
                        }
                    }
                    if (visited.size() > 120000) break;
                }
                frontier.swap(next);
            }
            // Players hides its vector: pull its children from the heap snapshot
            // later via GetChildren fallback; don't fail the fast path for it.
            const double tfms = std::chrono::duration<double, std::milli>(Clock::now() - tf0).count();
            if (visited.size() >= 1000 && !indexCancel_.load(std::memory_order_relaxed)) {
                // dedupe per-parent (vectors can list a child twice across slots)
                for (auto& [p, kids] : fast) {
                    if (kids.size() > 1) {
                        std::sort(kids.begin(), kids.end());
                        kids.erase(std::unique(kids.begin(), kids.end()), kids.end());
                    }
                }
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                {
                    std::unique_lock lk(indexMutex_);
                    children_.reserve(children_.size() + fast.size());
                    for (auto& [p, kids] : fast) {
                        auto& slot = children_[p];
                        if (slot.empty()) slot = std::move(kids);
                        else {
                            slot.insert(slot.end(), kids.begin(), kids.end());
                            std::sort(slot.begin(), slot.end());
                            slot.erase(std::unique(slot.begin(), slot.end()), slot.end());
                        }
                    }
                    instances_.reserve(instances_.size() + visited.size());
                    for (uintptr_t v : visited) instances_.insert(v);
                    indexReady_ = true;
                    indexStamp_ = GetTickCount64();
                    indexBytes_ = 0;
                    indexMs_ = ms;
                }
                size_t count = 0;
                { std::unique_lock mk(metaMutex_); count = instances_.size(); nameCache_.clear(); }
                std::cout << "[index-fast] " << count << " instances / " << fast.size()
                          << " parents via traversal in " << (int)tfms << "ms (total " << (int)ms << "ms)"
                          << std::endl;
                // Fall through to the heap sweep below: traversal misses hidden
                // vectors (Workspace/Players map content) and garbage caps drop
                // whole subtrees, so the sweep unions the rest in. Ready is
                // already set, so scripts run on the warm snapshot meanwhile.
            }
            // else fall through to heap sweep (<1000 reachable: probably in menu / loading)
        }
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    if (hi > 0x0000800000000000ULL) hi = 0x0000800000000000ULL;

    struct Reg { uintptr_t base; size_t size; DWORD type; };
    std::vector<Reg> regs;
    regs.reserve(2048);
    size_t privateBytes = 0, mappedBytes = 0;
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, (LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        a = next;
        // Private heap only. A measured pass found 263 MiB of mapped regions
        // holding 2 hits against 1323 MiB / 57901 hits in private memory, so
        // mapped regions cost ~17% of the sweep for noise-level signal.
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        DWORD prot = mbi.Protect & 0xFF;
        if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
              prot == PAGE_EXECUTE_WRITECOPY)) continue;
        if (mbi.RegionSize < 0x1000 || mbi.RegionSize > (SIZE_T)1024 * 1024 * 1024) continue;
        regs.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize, mbi.Type });
        if (mbi.Type == MEM_PRIVATE) privateBytes += mbi.RegionSize;
        else mappedBytes += mbi.RegionSize;
    }
    if (regs.empty()) return IsIndexReady();

    // Job-split: a single 100 MiB region used to pin one thread while the rest
    // idled. Slicing every region into ~512 KiB jobs keeps all workers fed;
    // overlapping jobs dedupe globally at merge.
    struct Job { uintptr_t base; size_t size; size_t regIdx; };
    std::vector<Job> jobs;
    jobs.reserve(regs.size() * 2);
    size_t total = 0;
    for (size_t ri = 0; ri < regs.size(); ++ri) {
        total += regs[ri].size;
        constexpr size_t kJob = 0x80000; // 512 KiB jobs over 256 KiB windows
        constexpr size_t kOverlap = 512; // > max instance stride; deduped globally
        for (size_t off = 0; off < regs[ri].size; off += kJob) {
            size_t n = std::min(kJob + kOverlap, regs[ri].size - off);
            jobs.push_back({ regs[ri].base + off, n, ri });
            if (off + kJob >= regs[ri].size) break;
            if (n == 0) break;
        }
    }
    // Syscall-bound, not CPU-bound: use all cores (old hw/2 capped at 6 left
    // half the NT-read throughput on the table on 8-16 core boxes).
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw : 8u, 4u, 16u);
    if (jobs.size() < threads) threads = (unsigned)jobs.size();
    const int64_t pOff = oParent_, dOff = oDesc_, cOff = oCont_;
    std::atomic<size_t> cursor{ 0 };
    std::vector<std::vector<std::pair<uintptr_t, uintptr_t>>> results(jobs.size());

    auto worker = [&]() {
        for (;;) {
            size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size() || indexCancel_.load(std::memory_order_relaxed)) return;
            results[i] = ScanHeapRegion(h, jobs[i].base, jobs[i].size, pOff, dOff, cOff);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (indexCancel_.load(std::memory_order_relaxed)) return false;

    // Flat merge: one global sort+unique over (parent,child) replaces 10k+
    // per-slot sorts and per-insert unordered_map hashing (~50k rehashes).
    // 50k pairs sort in <10 ms; the old path spent more in hashing alone.
    size_t estPairs = 0;
    for (auto& r : results) estPairs += r.size();
    std::vector<std::pair<uintptr_t, uintptr_t>> flat;
    flat.reserve(estPairs + 1024);
    std::vector<uintptr_t> flatAll;
    flatAll.reserve(estPairs + 1024);
    size_t privateHits = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        for (auto& pr : results[i]) {
            flatAll.push_back(pr.second);
            if (pr.first) { flat.emplace_back(pr.first, pr.second); ++privateHits; }
        }
    }
    // Sort by (parent,child), dedupe globally (covers overlap double-reports).
    std::sort(flat.begin(), flat.end(), [](const auto& a, const auto& b) {
        return a.first < b.first || (a.first == b.first && a.second < b.second);
    });
    flat.erase(std::unique(flat.begin(), flat.end()), flat.end());
    std::sort(flatAll.begin(), flatAll.end());
    flatAll.erase(std::unique(flatAll.begin(), flatAll.end()), flatAll.end());

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    // Union with the previous snapshot. A pass can miss whole regions (reads
    // fail while the heap moves), and partial results are still correct data,
    // so merging keeps every child list monotonic instead of losing entries.
    // Built fully OFF the lock; the exclusive section is a pointer-swap merge.
    std::unordered_map<uintptr_t, std::vector<uintptr_t>> next;
    next.reserve((flat.size() / 4 + 1024) | 1024);
    for (auto& [parent, child] : flat)
        next[parent].push_back(child); // already sorted+unique: per-parent runs arrive ordered
    size_t parents = 0;
    {
        std::unique_lock lk(indexMutex_);
        children_.reserve(children_.size() + next.size());
        for (auto& [parent, kids] : next) {
            auto& slot = children_[parent];
            if (slot.empty()) { slot = std::move(kids); continue; }
            slot.insert(slot.end(), kids.begin(), kids.end());
            std::sort(slot.begin(), slot.end());
            slot.erase(std::unique(slot.begin(), slot.end()), slot.end());
        }
        instances_.reserve(instances_.size() + flatAll.size());
        for (uintptr_t inst : flatAll) instances_.insert(inst);
        parents = children_.size();
        indexReady_ = true;
        indexStamp_ = GetTickCount64();
        indexBytes_ = total;
        indexMs_ = ms;
    }
    size_t count = 0;
    {
        std::unique_lock mk(metaMutex_);
        count = instances_.size();
        nameCache_.clear();
    }
    std::cout << "[index] " << count << " instances / " << parents << " parents from "
              << (total >> 20) << " MiB in " << (int)ms << "ms (pass " << flatAll.size() << ")"
              << " [private " << (privateBytes >> 20) << "MiB/" << privateHits
              << " mapped " << (mappedBytes >> 20) << "MiB/0]"
              << std::endl;
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
    const uint64_t t0 = GetTickCount64();
    const char* won = "empty";
    const int64_t parentOff = oParent_;
    auto hasParent = [&](uintptr_t c) {
        return ReadPtr(c + (uintptr_t)parentOff) == inst;
    };
    // O(1) dedupe: old linear `for seen in out` was O(N^2) — 10k-child
    // Workspace took 100M comparisons. Hash set keeps it linear.
    std::unordered_set<uintptr_t> seen;
    seen.reserve(256);
    auto accept = [&](const std::vector<uintptr_t>& candidates) {
        for (uintptr_t c : candidates) {
            if (!FastReadable(c)) continue;
            if (!seen.insert(c).second) continue;
            if (hasParent(c)) out.push_back(c);
        }
    };

    // 1) snapshot candidates (fast path, always verified against live memory).
    //    Copied under the lock; verification reads remote memory and must not
    //    hold it (the index merge takes the exclusive side of this same lock).
    std::vector<uintptr_t> snap;
    {
        std::shared_lock lk(indexMutex_);
        if (indexReady_) {
            auto it = children_.find(inst);
            if (it != children_.end()) snap = it->second;
        }
    }
    if (!snap.empty()) { seen.reserve(snap.size() * 2); }
    accept(snap);
    const size_t nSnap = out.size();
    // 2) live children vector. Some services store the pair reversed, so accept
    //    either ordering. Always merged with the snapshot: a partial snapshot
    //    must not hide children the vector knows about, and vice versa.
    //    One 48-byte bulk read for the 6 candidate (start,end) pairs instead of
    //    12 syscalls.
    const int64_t startOff = oChildrenStart_;
    {
        // 7 qwords -> 6 sliding (start,end) pairs, same as the old per-slot
        // reads (slot d = qwords[d],qwords[d+1]).
        uintptr_t w[7] = {};
        if (mem_->CustomRead(inst + (uintptr_t)startOff, w, sizeof(w))) {
            for (int d = 0; d < 6; ++d) {
                uintptr_t s = w[d], e = w[d + 1];
                if (e < s) std::swap(s, e);
                if (!FastReadable(s) || !FastReadable(e)) continue;
                size_t n = (size_t)((e - s) / 8);
                if (n == 0 || n > 100000) continue;
                std::vector<uintptr_t> vec(n);
                if (!mem_->CustomRead(s, vec.data(), n * 8)) continue;
                accept(vec);
            }
        } else {
            for (int d = 0; d < 6; ++d) {
                uintptr_t a = 0, b = 0;
                if (!mem_->CustomRead(inst + (uintptr_t)startOff + d * 8, &a, sizeof(a))) continue;
                if (!mem_->CustomRead(inst + (uintptr_t)startOff + d * 8 + 8, &b, sizeof(b))) continue;
                uintptr_t s = a, e = b;
                if (e < s) std::swap(s, e);
                if (!FastReadable(s) || !FastReadable(e)) continue;
                size_t n = (size_t)((e - s) / 8);
                if (n == 0 || n > 100000) continue;
                std::vector<uintptr_t> vec(n);
                if (!mem_->CustomRead(s, vec.data(), n * 8)) continue;
                accept(vec);
            }
        }
    }
    if (!out.empty()) {
        won = (nSnap > 0 && out.size() == nSnap) ? "snap" : "vec";
        LogSlowLookup(inst, t0, won);
        return out;
    }

    // 3) DataModel.Workspace is a direct pointer even when the vector is hidden
    if (IsValidDataModelCheap(inst)) {
        uintptr_t ws = ReadPtr(inst + (uintptr_t)oWorkspace_);
        if (FastReadable(ws) && hasParent(ws)) {
            LogSlowLookup(inst, t0, "wsptr");
            return { ws };
        }
    }

    // 4) heap scan (direct lookups only), then cache so repeats are instant.
    //    A sweep costs seconds, so misses share instead of each paying for a
    //    pass: while a background build runs, wait for it (it fills this same
    //    snapshot); otherwise at most one sweep per throttle window sweeps and
    //    everyone else takes the cached result.
    if (!allowScan) { LogSlowLookup(inst, t0, won); return out; }
    if (IsIndexBuilding()) {
        const uint64_t t0 = GetTickCount64();
        while (IsIndexBuilding() && GetTickCount64() - t0 < 8000)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::vector<uintptr_t> fresh;
        {
            std::shared_lock lk(indexMutex_);
            auto it = children_.find(inst);
            if (it == children_.end()) return out;
            fresh = it->second;
        }
        accept(fresh);
        if (!out.empty()) won = "wait+snap";
        LogSlowLookup(inst, t0, won);
        return out;
    }
    {
        const uint64_t now = GetTickCount64();
        uint64_t last = lastSweepMs_.load(std::memory_order_relaxed);
        if (now - last < 3000) { LogSlowLookup(inst, t0, "throttled"); return out; }
        // Do not let every miss on a deep tree node trigger a full heap sweep.
        // Only likely root containers are allowed to hit the expensive fallback
        // while the index is stale or still warming.
        std::string cls = GetClass(inst);
        const bool likelyRoot = cls == "DataModel" || cls == "Workspace" ||
                               cls == "Players" || cls == "Player" ||
                               cls == "Folder" || cls == "Model" ||
                               cls == "PlayerGui" || cls == "Tool";
        if (!likelyRoot && !IsIndexReady()) {
            LogSlowLookup(inst, t0, "scan-gated");
            return out;
        }
        if (!lastSweepMs_.compare_exchange_strong(last, now, std::memory_order_relaxed))
            { LogSlowLookup(inst, t0, "throttled"); return out; }
    }
    auto scanned = ScanChildrenByParent(inst);
    for (uintptr_t c : scanned) {
        if (hasParent(c)) out.push_back(c);
    }
    if (!out.empty()) won = "sweep";
    {
        std::unique_lock lk(indexMutex_);
        children_[inst] = out;
    }
    LogSlowLookup(inst, t0, won);
    return out;
}

// Anything over 50 ms names its winner so a slow tree lookup points at the
// mechanism that served it (snapshot, vector, workspace ptr, sweep, stall).
void InstanceStore::LogSlowLookup(uintptr_t inst, uint64_t t0, const char* won) const {
    const uint64_t dt = GetTickCount64() - t0;
    if (dt < 50) return;
    std::string cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    std::string nm = const_cast<InstanceStore*>(this)->GetName(inst);
    if (nm.size() > 40) nm.resize(40);
    LustedLogf("SLOW lookup %-24s cls=%-14s via=%s took=%llums",
               nm.c_str(), cls.c_str(), won, (unsigned long long)dt);
}

// Scans are cached per parent by GetChildren, so a miss costs one pass and
// every later lookup of that parent is instant.
uintptr_t InstanceStore::FindFirstChild(uintptr_t inst, const std::string& name) {
    for (auto c : GetChildren(inst))
        if (GetName(c) == name) return c;
    return DirectChildByName(inst, name);
}

uintptr_t InstanceStore::FindChildCached(uintptr_t inst, const std::string& name) {
    for (auto c : GetChildren(inst, false))
        if (GetName(c) == name) return c;
    return DirectChildByName(inst, name);
}

uint64_t InstanceStore::IndexAgeMs() const {
    std::shared_lock lk(indexMutex_);
    if (!indexReady_) return UINT64_MAX;
    return GetTickCount64() - indexStamp_;
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
        // A missing child is usually streaming in, not a stale snapshot. Full
        // rebuilds are intentionally rate-limited: a stale miss should not keep
        // paying a multi-second heap sweep every 120 ms while the index is old.
        if (!IsIndexBuilding()) {
            const uint64_t now = GetTickCount64();
            uint64_t last = lastIndexRebuildMs_.load(std::memory_order_relaxed);
            if (IndexAgeMs() > 10000 && now - last > 5000) {
                if (lastIndexRebuildMs_.compare_exchange_strong(last, now, std::memory_order_relaxed))
                    BuildIndex(0, true);
            }
        }
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
    const int64_t parentOff = oParent_;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    struct Reg { uintptr_t base; size_t size; };
    std::vector<Reg> regs;
    regs.reserve(1024);
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
        if (mbi.RegionSize < 0x1000 || mbi.RegionSize > (SIZE_T)1024 * 1024 * 1024) continue;
        regs.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize });
    }
    // Job-split + full core count, same as BuildIndex.
    struct Job { uintptr_t base; size_t size; };
    std::vector<Job> jobs;
    jobs.reserve(regs.size() * 2);
    for (auto& r : regs) {
        constexpr size_t kJob = 0x80000;
        constexpr size_t kOverlap = 512;
        for (size_t off = 0; off < r.size; off += kJob) {
            size_t n = std::min(kJob + kOverlap, r.size - off);
            jobs.push_back({ r.base + off, n });
            if (off + kJob >= r.size) break;
            if (n == 0) break;
        }
    }
    if (jobs.empty()) return out;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw : 8u, 4u, 16u);
    if (jobs.size() < threads) threads = (unsigned)jobs.size();
    const int64_t pOff = parentOff, dOff = oDesc_, cOff = oCont_;
    std::atomic<size_t> cursor{ 0 };
    std::vector<std::vector<std::pair<uintptr_t, uintptr_t>>> results(jobs.size());
    auto worker = [&]() {
        for (;;) {
            size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size() || indexCancel_.load(std::memory_order_relaxed)) return;
            results[i] = ScanHeapRegion(h, jobs[i].base, jobs[i].size, pOff, dOff, cOff);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (indexCancel_.load(std::memory_order_relaxed)) return out;
    size_t est = 0;
    for (auto& v : results) est += v.size();
    out.reserve(std::min<size_t>(est, 4096));
    for (auto& vec : results) {
        for (auto& [p, child] : vec) {
            if (p != parent) continue;
            if (ReadPtr(child + (uintptr_t)parentOff) == parent) out.push_back(child);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
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
    chain.reserve(4);
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

PropInfo InstanceStore::ResolveProp(uintptr_t inst, const std::string& clsIn,
                                     const std::string& member) const {
    PropInfo out;
    std::string cls = clsIn;
    if (cls.empty() && inst) {
        // class names are cached, so this stays cheap
        cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    }
    if (cls.empty() || member.empty()) return out;

    // table_ is immutable after Load, so the answer for a (class, member) pair
    // never changes. Property reads happen on every script access, and the
    // resolution walks ancestors, sub-object chains and the whole table, so
    // the first miss pays for it and every later hit is a hash lookup.
    // Zero-alloc hit path: stack key + transparent lookup (old CacheKey
    // allocated a std::string per call, even on hits).
    char stack[256];
    std::string heap;
    std::string_view keyView;
    size_t need = cls.size() + 1 + member.size();
    if (need <= sizeof(stack)) {
        memcpy(stack, cls.data(), cls.size());
        stack[cls.size()] = '\x1f';
        memcpy(stack + cls.size() + 1, member.data(), member.size());
        keyView = std::string_view(stack, need);
    } else {
        heap.reserve(need);
        heap.append(cls).push_back('\x1f');
        heap.append(member);
        keyView = std::string_view(heap);
    }
    {
        std::shared_lock lk(propMutex_);
        auto it = propCache_.find(keyView);
        if (it != propCache_.end()) return it->second;
    }
    PropInfo r = ResolvePropRaw(cls, member);
    {
        std::unique_lock lk(propMutex_);
        if (propCache_.size() < (1u << 16)) {
            std::string owned;
            owned.reserve(need);
            owned.append(cls).push_back('\x1f');
            owned.append(member);
            propCache_.emplace(std::move(owned), r);
        }
    }
    return r;
}

PropInfo InstanceStore::ResolvePropRaw(const std::string& cls,
                                       const std::string& member) const {
    PropInfo out;
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
        if (auto o = OffsetOf(c, member)) {
            out = typeFor(c, member);
            out.offset = *o;
            out.hops.clear();
            return out;
        }
        // offsets.json spells some members differently by case from the runtime
        // name (dump "Walkspeed" vs Roblox "WalkSpeed"); exact match won above,
        // so this only fires when there is no exact spelling to prefer.
        if (auto hit = LookupLower(c, member)) {
            out = typeFor(hit->realClass, hit->realMember);
            out.offset = hit->offset;
            out.hops.clear();
            return out;
        }
    }

    // 2) sub-object chains: Part -> BasePart.Primitive -> Primitive.Position,
    //    Workspace -> World -> World.Gravity. Each hop is a pointer member.
    //    subs is the Load-time cached list (old code rebuilt+copied it per miss).
    {
        const auto& subs = subClasses_;
        std::vector<int64_t> hops;
        hops.reserve(3);
        std::vector<std::string> frontier = chain;
        for (int depth = 0; depth < 3 && !frontier.empty(); ++depth) {
            std::vector<std::string> next;
            for (const auto& c : frontier) {
                for (const auto& sub : subs) {
                    auto hop = OffsetOf(c, sub);
                    if (!hop) continue;
                    int64_t fieldOff = 0;
                    std::string realMember = member;
                    if (auto field = OffsetOf(sub, member)) fieldOff = *field;
                    else if (auto hit = LookupLower(sub, member)) {
                        fieldOff = hit->offset;
                        realMember = hit->realMember;
                    } else continue;
                    PropInfo p = typeFor(sub, realMember);
                    p.offset = fieldOff;
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
        if (count == 0) {
            // no exact spelling anywhere: accept an unambiguous case-only match
            const std::string lm = LowerKey(member);
            const OffsetLookup* hit = nullptr;
            int hits = 0;
            for (const auto& [lowerCls, members] : lowerTable_) {
                auto it = members.find(lm);
                if (it == members.end()) continue;
                ++hits;
                hit = &it->second;
            }
            if (hits == 1 && hit) {
                out = typeFor(hit->realClass, hit->realMember);
                out.offset = hit->offset;
                out.hops.clear();
                return out;
            }
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
        if (!FastReadable(next)) return 0;
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
std::optional<int64_t> InstanceStore::OffsetOfInsensitive(const std::string& cls, const std::string& member) const {
    if (auto o = OffsetOf(cls, member)) return o;
    auto hit = LookupLower(cls, member);
    return hit ? std::optional<int64_t>(hit->offset) : std::nullopt;
}
bool InstanceStore::WritePropFloatMirrored(uintptr_t inst, const std::string& clsIn, const std::string& memberIn, float value) const {
    if (!inst || !mem_) return false;
    std::string cls = clsIn;
    if (cls.empty() && inst) cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    if (cls.empty()) cls = "Humanoid";
    std::string lc = LowerKey(cls), lm = LowerKey(memberIn);
    std::vector<std::string> mirrors;
    if (lc == "humanoid") {
        if (lm == "walkspeed" || lm == "walkspeedcheck") mirrors = {"Walkspeed", "WalkspeedCheck"};
        else if (lm == "jumppower" || lm == "jumpheight") mirrors = {"JumpPower", "JumpHeight"};
    }
    if (!mirrors.empty()) {
        std::vector<std::pair<std::string, PropInfo>> targets;
        for (auto& mm : mirrors) {
            PropInfo p = ResolveProp(inst, cls, mm);
            if (!p.ok()) {
                auto off = OffsetOfInsensitive(cls, mm);
                if (!off) continue;
                p.type = PropType::F32;
                p.offset = *off;
                p.hops.clear();
            }
            targets.emplace_back(mm, p);
        }
        if (targets.empty()) return false;
        bool allOk = true;
        for (auto& [name, p] : targets) {
            uintptr_t base = PropBase(p, inst);
            if (!base) { allOk = false; continue; }
            if (!mem_->Write<float>(base + (uintptr_t)p.offset, value)) allOk = false;
        }
        return allOk;
    }
    PropInfo p = ResolveProp(inst, cls, memberIn);
    if (!p.ok()) {
        auto off = OffsetOfInsensitive(cls, memberIn);
        if (!off) return false;
        p.type = PropType::F32;
        p.offset = *off;
        p.hops.clear();
    }
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->Write<float>(base + (uintptr_t)p.offset, value);
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
    if (!FastReadable(dm)) return false;
    uintptr_t ws = ReadPtr(dm + (uintptr_t)oWorkspace_);
    if (!FastReadable(ws)) return false;
    if (ReadPtr(ws + (uintptr_t)oParent_) != dm) return false;
    return true;
}

bool InstanceStore::IsValidDataModel(uintptr_t dm) { return IsValidDataModelCheap(dm); }

uintptr_t InstanceStore::TryFakePointer(DWORD, uintptr_t base) {
    auto fakePtrOff = OffsetOf("FakeDataModel", "Pointer");
    auto realOff = OffsetOf("FakeDataModel", "RealDataModel");
    if (!fakePtrOff || !realOff) return 0;
    uintptr_t fake = ReadPtr(base + (uintptr_t)*fakePtrOff);
    if (!FastReadable(fake)) return 0;
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
    if (!FastReadable(ve)) return 0;
    uintptr_t fake = ReadPtr(ve + (uintptr_t)*veFakeOff);
    if (!FastReadable(fake)) return 0;
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
    if (!FastReadable(ts)) return 0;
    uintptr_t start = ReadPtr(ts + (uintptr_t)*startOff);
    uintptr_t end = ReadPtr(ts + (uintptr_t)*endOff);
    if (!FastReadable(start) || !FastReadable(end) || end <= start) return 0;
    size_t n = (size_t)((end - start) / 8);
    if (n == 0 || n > 500) return 0;
    std::vector<uintptr_t> jobs(n);
    if (!mem_->CustomRead(start, jobs.data(), n * 8)) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (!FastReadable(jobs[i])) continue;
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
    uintptr_t ws = ReadPtr(dm + (uintptr_t)oWorkspace_);
    return FastReadable(ws) ? ws : 0;
}

uintptr_t InstanceStore::DirectChildByName(uintptr_t parent, const std::string& name) {
    // Named lookups do not need the whole child list: probe the children vector
    // slots and match names one at a time. Bulk-read each vector (1 syscall)
    // instead of per-element reads (old: N syscalls per lookup).
    // 7 qwords -> 6 sliding pairs (slot d = w[d],w[d+1]), same as GetChildren.
    if (!parent || !mem_ || name.empty()) return 0;
    const int64_t startOff = oChildrenStart_;
    uintptr_t w[7] = {};
    bool haveW = mem_->CustomRead(parent + (uintptr_t)startOff, w, sizeof(w));
    for (int d = 0; d < 6; ++d) {
        uintptr_t s = 0, e = 0;
        if (haveW) { s = w[d]; e = w[d + 1]; }
        else {
            uintptr_t a = 0, b = 0;
            if (!mem_->CustomRead(parent + (uintptr_t)startOff + d * 8, &a, sizeof(a))) continue;
            if (!mem_->CustomRead(parent + (uintptr_t)startOff + d * 8 + 8, &b, sizeof(b))) continue;
            s = a; e = b;
        }
        if (e < s) std::swap(s, e);
        if (!FastReadable(s) || !FastReadable(e)) continue;
        size_t n = (size_t)((e - s) / 8);
        if (n == 0 || n > 100000) continue;
        // Cap the probe: a corrupted vector length costs one bounded bulk read,
        // not a 100k-name walk. Direct hits are near the front.
        const size_t probe = n > 2048 ? 2048 : n;
        std::vector<uintptr_t> buf(probe);
        if (!mem_->CustomRead(s, buf.data(), probe * 8)) continue;
        for (size_t i = 0; i < probe; ++i) {
            uintptr_t p2 = buf[i];
            if (!FastReadable(p2)) continue;
            if (GetName(p2) == name) {
                // Verify parent (stale vector slot) before returning.
                if (ReadPtr(p2 + (uintptr_t)oParent_) == parent) return p2;
            }
        }
        if (n > probe) {
            // Tail in 2K chunks without re-reading the head.
            for (size_t base = probe; base < n; base += 2048) {
                size_t m = std::min<size_t>(2048, n - base);
                if (!mem_->CustomRead(s + base * 8, buf.data(), m * 8)) break;
                for (size_t i = 0; i < m; ++i) {
                    uintptr_t p2 = buf[i];
                    if (!FastReadable(p2)) continue;
                    if (GetName(p2) == name) {
                        if (ReadPtr(p2 + (uintptr_t)oParent_) == parent) return p2;
                    }
                }
            }
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
    const int64_t parentOff = oParent_;
    for (uintptr_t c : GetChildren(players)) {
        if (!FastReadable(c)) continue;
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
    const int64_t lpOff = oLocalPlayer_;
    uintptr_t lp = ReadPtr(players + (uintptr_t)lpOff);
    if (FastReadable(lp) && ReadPtr(lp + 8) == lp) return lp;
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
        uintptr_t ch = ReadPtr(lp + (uintptr_t)oModelInstance_);
        if (FastReadable(ch) && ReadPtr(ch + 8) == ch) return ch;
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