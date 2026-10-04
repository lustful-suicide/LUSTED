#include "instance.h"
#include "memory.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <iostream>
#include <tlhelp32.h>
#include <nlohmann/json.hpp>

bool InstanceStore::Load(const std::string& path) {
    table_.clear(); loaded_ = false;
    manualDm_ = 0;
    cachedDm_ = 0;
    childCache_.clear();
    std::ifstream f(path);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        auto off = j.contains("Offsets") ? j["Offsets"] : j;
        for (auto& cls : off.items()) {
            for (auto& m : cls.value().items()) {
                if (m.value().is_number())
                    table_[cls.key()][m.key()] = m.value().get<int64_t>();
            }
        }
        loaded_ = true;
        return true;
    } catch (...) { return false; }
}

std::optional<int64_t> InstanceStore::OffsetOf(const std::string& c, const std::string& m) {
    auto it = table_.find(c);
    if (it == table_.end()) return std::nullopt;
    auto jt = it->second.find(m);
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}
int64_t InstanceStore::OffsetOr(const std::string& c, const std::string& m, int64_t fb) {
    auto o = OffsetOf(c, m);
    return o ? *o : fb;
}

uintptr_t InstanceStore::ReadPtr(uintptr_t a) {
    uintptr_t v = 0;
    if (!mem_ || !mem_->CustomRead(a, &v, sizeof(v))) return 0;
    return v;
}
bool InstanceStore::IsReadablePtr(uintptr_t p) {
    if (p < 0x10000) return false;
    if (p >= 0x0000800000000000ULL) return false;
    return true;
}

std::string InstanceStore::ReadRobloxString(uintptr_t addr, size_t maxLen) {
    if (!mem_ || addr == 0) return "";
    if (maxLen > 256) maxLen = 256;
    uintptr_t ptr = ReadPtr(addr);
    uint64_t len = 0;
    mem_->CustomRead(addr + 8, &len, sizeof(len));
    char inlineBuf[256] = {};
    if (!mem_->CustomRead(addr, inlineBuf, sizeof(inlineBuf))) return "";
    auto looksAscii = [](const char* s, size_t n) {
        if (n == 0 || n > 128) return false;
        for (size_t i = 0; i < n; ++i)
            if (!isprint((unsigned char)s[i]) && s[i] != '\0') return false;
        return true;
    };
    if (IsReadablePtr(ptr) && len > 0 && len <= maxLen) {
        std::string s(len, '\0');
        if (mem_->CustomRead(ptr, s.data(), len) && looksAscii(s.data(), len)) {
            auto z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            return s;
        }
    }
    uint64_t len2 = 0;
    mem_->CustomRead(addr + 16, &len2, sizeof(len2));
    if (IsReadablePtr(ptr) && len2 > 0 && len2 <= maxLen) {
        std::string s((size_t)len2, '\0');
        if (mem_->CustomRead(ptr, s.data(), (size_t)len2) && looksAscii(s.data(), (size_t)len2)) {
            auto z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            return s;
        }
    }
    {
        char buf[129] = {};
        if (mem_->CustomRead(addr, buf, 128)) {
            buf[128] = '\0';
            size_t n = strnlen(buf, 128);
            if (n > 0 && n <= maxLen && looksAscii(buf, n)) return std::string(buf, n);
        }
        if (looksAscii(inlineBuf, strnlen(inlineBuf, 64)))
            return std::string(inlineBuf, strnlen(inlineBuf, 64));
    }
    return "";
}

std::string InstanceStore::GetName(uintptr_t inst) {
    if (!inst) return "";
    int64_t nameOff = OffsetOr("Instance", "Name", 8);
    int64_t containerOff = OffsetOr("Instance", "NameContainer", 112);
    uintptr_t container = ReadPtr(inst + (uintptr_t)containerOff);
    if (IsReadablePtr(container)) {
        std::string s = ReadRobloxString(container + (uintptr_t)nameOff);
        if (!s.empty()) return s;
        s = ReadRobloxString(container);
        if (!s.empty()) return s;
    }
    {
        std::string s = ReadRobloxString(inst + (uintptr_t)containerOff);
        if (!s.empty()) return s;
    }
    {
        uintptr_t p = ReadPtr(inst + (uintptr_t)nameOff);
        if (IsReadablePtr(p)) {
            std::string s = ReadRobloxString(p);
            if (!s.empty()) return s;
        }
    }
    return "";
}

std::string InstanceStore::GetClass(uintptr_t inst) {
    if (!inst) return "";
    int64_t descOff = OffsetOr("Instance", "ClassDescriptor", 24);
    int64_t cnOff = OffsetOr("Instance", "ClassName", 8);
    uintptr_t desc = ReadPtr(inst + (uintptr_t)descOff);
    if (IsReadablePtr(desc)) {
        std::string s = ReadRobloxString(desc + (uintptr_t)cnOff);
        if (!s.empty()) return s;
        // desc+8 is a pointer to the name string object (double deref)
        uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
        if (IsReadablePtr(p)) {
            s = ReadRobloxString(p);
            if (!s.empty()) return s;
            // one more level (TString wrapper)
            uintptr_t p2 = ReadPtr(p);
            if (IsReadablePtr(p2)) {
                s = ReadRobloxString(p2);
                if (!s.empty()) return s;
            }
        }
        std::string s2 = ReadRobloxString(desc);
        if (!s2.empty() && s2.size() < 64) return s2;
    }
    return "";
}

uintptr_t InstanceStore::GetParent(uintptr_t inst) {
    if (!inst) return 0;
    int64_t off = OffsetOr("Instance", "Parent", 104);
    uintptr_t p = ReadPtr(inst + (uintptr_t)off);
    return IsReadablePtr(p) ? p : 0;
}

bool InstanceStore::IsValidInstance(uintptr_t inst) {
    if (!IsReadablePtr(inst)) return false;
    // This at +8 must equal self (strong validator from live dump)
    uintptr_t self = ReadPtr(inst + 8);
    if (self != inst) return false;
    uintptr_t desc = ReadPtr(inst + (uintptr_t)OffsetOr("Instance", "ClassDescriptor", 24));
    if (!IsReadablePtr(desc)) return false;
    uintptr_t cont = ReadPtr(inst + (uintptr_t)OffsetOr("Instance", "NameContainer", 112));
    if (!IsReadablePtr(cont)) return false;
    return true;
}

std::vector<uintptr_t> InstanceStore::ScanChildrenByParent(uintptr_t parent) {
    std::vector<uintptr_t> out;
    if (!mem_ || !mem_->IsOpen() || !parent) return out;
    HANDLE h = mem_->Handle();
    if (!h) return out;
    int64_t parentOff = OffsetOr("Instance", "Parent", 104);
    // cache 10s for hits, no cache for misses (heap moves)
    uint64_t now = (uint64_t)GetTickCount64();
    auto it = childCache_.find(parent);
    if (it != childCache_.end() && now - it->second.first < 10000 && !it->second.second.empty())
        return it->second.second;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    if (hi > 0x800000000000ULL) hi = 0x800000000000ULL;
    // collect private heap regions first, closest to parent first
    struct Reg { uintptr_t base; size_t size; };
    std::vector<Reg> regs;
    regs.reserve(2048);
    for (uintptr_t addr = lo; addr < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi))) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
        if (mbi.State != MEM_COMMIT) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        DWORD prot = mbi.Protect & 0xFF;
        bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                         prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                         prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) continue;
        if (mbi.Type != MEM_PRIVATE) continue;
        if (mbi.RegionSize < 0x1000 || mbi.RegionSize > 128 * 1024 * 1024) continue;
        regs.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize });
    }
    std::sort(regs.begin(), regs.end(), [parent](const Reg& a, const Reg& b) {
        uintptr_t da = a.base > parent ? a.base - parent : parent - a.base;
        uintptr_t db = b.base > parent ? b.base - parent : parent - b.base;
        return da < db;
    });
    size_t totalRead = 0;
    const size_t kCap = 4ULL * 1024 * 1024 * 1024; // 4GB cap
    std::vector<uint8_t> buf;
    buf.reserve(4 * 1024 * 1024);
    for (auto& r : regs) {
        if (totalRead >= kCap) break;
        if (out.size() >= 2000) break;
        for (size_t off = 0; off < r.size; off += 4 * 1024 * 1024) {
            size_t chunk = r.size - off;
            if (chunk > 4 * 1024 * 1024) chunk = 4 * 1024 * 1024;
            buf.resize(chunk);
            if (!mem_->CustomRead(r.base + off, buf.data(), chunk)) continue;
            totalRead += chunk;
            size_t n = chunk / 8;
            uintptr_t* q = (uintptr_t*)buf.data();
            for (size_t i = 0; i < n; ++i) {
                if (q[i] != parent) continue;
                uintptr_t hitAddr = r.base + off + i * 8;
                if (hitAddr < (uintptr_t)parentOff) continue;
                uintptr_t cand = hitAddr - (uintptr_t)parentOff;
                if (!IsReadablePtr(cand)) continue;
                bool dup = false;
                for (auto v : out) if (v == cand) { dup = true; break; }
                if (dup) continue;
                if (IsValidInstance(cand)) {
                    if (ReadPtr(cand + (uintptr_t)parentOff) == parent) {
                        out.push_back(cand);
                        if (out.size() >= 2000) break;
                    }
                }
            }
            if (out.size() >= 2000) break;
        }
    }
    if (!out.empty()) childCache_[parent] = { now, out };
    return out;
}

std::vector<uintptr_t> InstanceStore::GetChildren(uintptr_t inst) {
    std::vector<uintptr_t> out;
    if (!inst || !mem_) return out;
    int64_t startOff = OffsetOr("Instance", "ChildrenStart", 120);
    int64_t endOff = OffsetOr("Instance", "ChildrenEnd", 8);
    uintptr_t start = 0, end = 0;
    if (endOff == 8 || endOff <= startOff) {
        start = ReadPtr(inst + (uintptr_t)startOff);
        mem_->CustomRead(inst + (uintptr_t)startOff + 8, &end, sizeof(end));
    } else {
        start = ReadPtr(inst + (uintptr_t)startOff);
        end = ReadPtr(inst + (uintptr_t)endOff);
    }
    auto tryRange = [&](uintptr_t s, uintptr_t e, std::vector<uintptr_t>& dst) -> bool {
        if (!IsReadablePtr(s) || !IsReadablePtr(e) || e < s) return false;
        size_t n = (size_t)((e - s) / 8);
        if (n == 0 || n > 10000) return false;
        size_t cap = n > 512 ? 512 : n;
        std::vector<uintptr_t> b(cap);
        if (!mem_->CustomRead(s, b.data(), cap * 8)) return false;
        for (auto v : b) if (IsReadablePtr(v)) dst.push_back(v);
        return !dst.empty();
    };
    std::vector<uintptr_t> vec;
    if (tryRange(start, end, vec)) {
        // validate: vector is only trusted if majority point back via Parent
        int ok = 0, checked = 0;
        for (size_t i = 0; i < vec.size() && i < 10; ++i) {
            if (!IsValidInstance(vec[i])) continue;
            ++checked;
            if (ReadPtr(vec[i] + (uintptr_t)OffsetOr("Instance", "Parent", 104)) == inst) ++ok;
        }
        if (checked > 0 && ok * 2 >= checked) return vec;
        // else fall through to parent scan (encrypted/moved vector)
    }
    // Fallback: parent-scan (works when Children vector is encrypted)
    auto scanned = ScanChildrenByParent(inst);
    // Augment: DataModel.Workspace is a direct pointer and may live in a
    // region the heap scan misses; ensure it is present so
    // FindFirstChild("Workspace") works.
    {
        int64_t wsOff = OffsetOr("DataModel", "Workspace", 336);
        uintptr_t ws = ReadPtr(inst + (uintptr_t)wsOff);
        if (IsValidInstance(ws)) {
            bool has = false;
            for (auto v : scanned) if (v == ws) { has = true; break; }
            if (!has) {
                // only attach Workspace to its true parent
                if (ReadPtr(ws + (uintptr_t)OffsetOr("Instance", "Parent", 104)) == inst)
                    scanned.push_back(ws);
            }
        }
    }
    if (!scanned.empty()) return scanned;
    // last resort: neighbouring slots
    for (int d = 0; d < 3 && out.empty(); ++d) {
        uintptr_t s = 0, e = 0;
        mem_->CustomRead(inst + (uintptr_t)startOff + d * 8, &s, sizeof(s));
        mem_->CustomRead(inst + (uintptr_t)startOff + d * 8 + 8, &e, sizeof(e));
        std::vector<uintptr_t> tmp;
        if (tryRange(s, e, tmp)) {
            int ok = 0, checked = 0;
            for (size_t i = 0; i < tmp.size() && i < 10; ++i) {
                if (!IsValidInstance(tmp[i])) continue;
                ++checked;
                if (ReadPtr(tmp[i] + (uintptr_t)OffsetOr("Instance", "Parent", 104)) == inst) ++ok;
            }
            if (checked > 0 && ok * 2 >= checked) return tmp;
        }
    }
    return scanned;
}

uintptr_t InstanceStore::FindFirstChild(uintptr_t inst, const std::string& name) {
    for (auto c : GetChildren(inst))
        if (GetName(c) == name) return c;
    return 0;
}
uintptr_t InstanceStore::FindFirstChildOfClass(uintptr_t inst, const std::string& cls) {
    for (auto c : GetChildren(inst))
        if (GetClass(c) == cls) return c;
    return 0;
}
void InstanceStore::GetDescendants(uintptr_t inst, std::vector<uintptr_t>& out, int depth) {
    if (depth <= 0 || !inst) return;
    for (auto c : GetChildren(inst)) {
        out.push_back(c);
        GetDescendants(c, out, depth - 1);
        if (out.size() > 10000) return;
    }
}
uintptr_t InstanceStore::WaitForChild(uintptr_t inst, const std::string& name, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < ms) {
        uintptr_t f = FindFirstChild(inst, name);
        if (f) return f;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
uintptr_t InstanceStore::ResolvePath(uintptr_t root, const std::string& dotted) {
    uintptr_t cur = root;
    size_t s = 0;
    while (cur && s < dotted.size()) {
        size_t dot = dotted.find('.', s);
        std::string part = dotted.substr(s, dot == std::string::npos ? dot : dot - s);
        if (!part.empty()) {
            cur = FindFirstChild(cur, part);
            if (!cur) return 0;
        }
        if (dot == std::string::npos) break;
        s = dot + 1;
    }
    return cur;
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
        if (_wcsicmp(me.szModule, w.c_str()) == 0) { base = (uintptr_t)me.modBaseAddr; size = me.modBaseSize; ok = true; break; }
#else
        if (_stricmp(me.szModule, mod.c_str()) == 0) { base = (uintptr_t)me.modBaseAddr; size = me.modBaseSize; ok = true; break; }
#endif
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    return ok;
}
uintptr_t InstanceStore::ModuleBase(DWORD pid, const std::string& mod) {
    uintptr_t b = 0; size_t s = 0;
    ModuleInfo(pid, mod, b, s);
    return b;
}

bool InstanceStore::IsValidDataModel(uintptr_t dm) {
    if (!IsReadablePtr(dm)) return false;
    int64_t wsOff = OffsetOr("DataModel", "Workspace", 336);
    uintptr_t ws = ReadPtr(dm + (uintptr_t)wsOff);
    if (!IsReadablePtr(ws)) return false;
    // PlaceId/GameId should be non-zero in a live game
    int64_t placeOff = OffsetOr("DataModel", "PlaceId", 392);
    int64_t gameOff = OffsetOr("DataModel", "GameId", 384);
    uint64_t place = 0, game = 0;
    mem_->CustomRead(dm + (uintptr_t)placeOff, &place, sizeof(place));
    mem_->CustomRead(dm + (uintptr_t)gameOff, &game, sizeof(game));
    auto kids = GetChildren(dm);
    if (kids.size() < 3 || kids.size() > 3000) {
        // still allow if ids look sane (studio edge case)
        if (place == 0 && game == 0) return false;
    }
    // Require at least Workspace readable + one id OR several children
    if (place == 0 && game == 0 && kids.size() < 5) return false;
    // Strong signal: Workspace child or service names resolve
    int hits = 0;
    size_t check = kids.size() > 40 ? 40 : kids.size();
    for (size_t i = 0; i < check; ++i) {
        std::string n = GetName(kids[i]);
        if (n == "Workspace" || n == "Players" || n == "Lighting" ||
            n == "ReplicatedFirst" || n == "StarterGui" || n == "SoundService")
            ++hits;
    }
    // If names are broken (decryption drift) still accept on ids+workspace
    if (hits >= 1) return true;
    if (place != 0 || game != 0) return true;
    return kids.size() >= 8;
}

uintptr_t InstanceStore::TryFakePointer(DWORD pid, uintptr_t base) {
    auto fakePtrOff = OffsetOf("FakeDataModel", "Pointer");
    auto realOff = OffsetOf("FakeDataModel", "RealDataModel");
    if (!fakePtrOff || !realOff) return 0;
    uintptr_t at = base + (uintptr_t)*fakePtrOff;
    uintptr_t fake = ReadPtr(at);
    std::cout << "[datamodel] fakeptr: base=0x" << std::hex << base << " off=0x" << (uintptr_t)*fakePtrOff
              << " at=0x" << at << " fake=0x" << fake << std::dec << "\n";
    if (!IsReadablePtr(fake)) return 0;
    uintptr_t real = ReadPtr(fake + (uintptr_t)*realOff);
    std::cout << "[datamodel] fake->real: fake+0x" << std::hex << (uintptr_t)*realOff
              << " real=0x" << real << std::dec << "\n";
    if (IsValidDataModel(real)) { std::cout << "[datamodel] valid via FakePointer\n"; return real; }
    if (IsValidDataModel(fake)) { std::cout << "[datamodel] valid (fake is dm)\n"; return fake; }
    return 0;
}

uintptr_t InstanceStore::TryVisualEngine(DWORD pid, uintptr_t base) {
    auto vePtrOff = OffsetOf("VisualEngine", "Pointer");
    auto veFakeOff = OffsetOf("VisualEngine", "FakeDataModel");
    auto realOff = OffsetOf("FakeDataModel", "RealDataModel");
    if (!vePtrOff || !veFakeOff || !realOff) return 0;
    uintptr_t ve = ReadPtr(base + (uintptr_t)*vePtrOff);
    std::cout << "[datamodel] visualengine: ve=0x" << std::hex << ve << std::dec << "\n";
    if (!IsReadablePtr(ve)) return 0;
    uintptr_t fake = ReadPtr(ve + (uintptr_t)*veFakeOff);
    std::cout << "[datamodel] ve->fake: 0x" << std::hex << fake << std::dec << "\n";
    if (!IsReadablePtr(fake)) return 0;
    uintptr_t real = ReadPtr(fake + (uintptr_t)*realOff);
    std::cout << "[datamodel] fake->real: 0x" << std::hex << real << std::dec << "\n";
    if (IsValidDataModel(real)) { std::cout << "[datamodel] valid via VisualEngine\n"; return real; }
    return 0;
}

uintptr_t InstanceStore::TryTaskScheduler(DWORD pid, uintptr_t base) {
    auto tsPtrOff = OffsetOf("TaskScheduler", "Pointer");
    auto startOff = OffsetOf("TaskScheduler", "JobStart");
    auto endOff = OffsetOf("TaskScheduler", "JobEnd");
    auto renderRealOff = OffsetOf("RenderJob", "RealDataModel");
    auto renderFakeOff = OffsetOf("RenderJob", "FakeDataModel");
    if (!tsPtrOff || !startOff || !endOff || !renderRealOff) return 0;
    uintptr_t ts = ReadPtr(base + (uintptr_t)*tsPtrOff);
    std::cout << "[datamodel] taskscheduler: ts=0x" << std::hex << ts << std::dec << "\n";
    if (!IsReadablePtr(ts)) return 0;
    uintptr_t start = ReadPtr(ts + (uintptr_t)*startOff);
    uintptr_t end = ReadPtr(ts + (uintptr_t)*endOff);
    std::cout << "[datamodel] jobs: start=0x" << std::hex << start << " end=0x" << end << std::dec << "\n";
    if (!IsReadablePtr(start) || !IsReadablePtr(end) || end <= start) return 0;
    size_t n = (size_t)((end - start) / 8);
    if (n == 0 || n > 500) { std::cout << "[datamodel] job count insane: " << n << "\n"; return 0; }
    std::vector<uintptr_t> jobs(n);
    if (!mem_->CustomRead(start, jobs.data(), n * 8)) return 0;
    // Pass 1: named RenderJob
    auto nameOff = OffsetOf("TaskScheduler", "JobName");
    for (size_t i = 0; i < n; ++i) {
        uintptr_t job = jobs[i];
        if (!IsReadablePtr(job)) continue;
        std::string jn;
        if (nameOff) jn = ReadRobloxString(job + (uintptr_t)*nameOff, 64);
        if (jn.find("Render") != std::string::npos) {
            uintptr_t real = ReadPtr(job + (uintptr_t)*renderRealOff);
            std::cout << "[datamodel] renderjob[" << i << "] name=" << jn << " real=0x" << std::hex << real << std::dec << "\n";
            if (IsValidDataModel(real)) { std::cout << "[datamodel] valid via TaskScheduler(Render)\n"; return real; }
            if (renderFakeOff) {
                uintptr_t fake = ReadPtr(job + (uintptr_t)*renderFakeOff);
                uintptr_t r2 = IsReadablePtr(fake) ? ReadPtr(fake + 504) : 0;
                if (IsValidDataModel(r2)) { std::cout << "[datamodel] valid via TaskScheduler(RenderFake)\n"; return r2; }
            }
        }
    }
    // Pass 2: blind - any job whose +Real slot validates as DataModel
    for (size_t i = 0; i < n; ++i) {
        uintptr_t job = jobs[i];
        if (!IsReadablePtr(job)) continue;
        uintptr_t real = ReadPtr(job + (uintptr_t)*renderRealOff);
        if (IsValidDataModel(real)) {
            std::cout << "[datamodel] valid via TaskScheduler(blind job " << i << ")\n";
            return real;
        }
    }
    return 0;
}

uintptr_t InstanceStore::GetDataModel(DWORD pid) {
    if (manualDm_ && IsValidDataModel(manualDm_)) return manualDm_;
    if (cachedDm_ && IsValidDataModel(cachedDm_)) return cachedDm_;
    cachedDm_ = 0;
    if (!mem_ || !mem_->IsOpen() || !pid) return 0;
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleInfo(pid, "RobloxPlayerBeta.exe", base, size) || !base) {
        std::cout << "[datamodel] no module base\n";
        return 0;
    }
    std::cout << "[datamodel] base=0x" << std::hex << base << " size=0x" << size << std::dec << "\n";
    if (uintptr_t dm = TryFakePointer(pid, base)) { cachedDm_ = dm; return dm; }
    if (uintptr_t dm = TryVisualEngine(pid, base)) { cachedDm_ = dm; return dm; }
    if (uintptr_t dm = TryTaskScheduler(pid, base)) { cachedDm_ = dm; return dm; }
    std::cout << "[datamodel] all chains failed (offsets loaded=" << (loaded_ ? "yes" : "no") << ")\n";
    return 0;
}
uintptr_t InstanceStore::GetWorkspace(DWORD pid) {
    uintptr_t dm = GetDataModel(pid);
    if (!dm) return 0;
    int64_t off = OffsetOr("DataModel", "Workspace", 336);
    uintptr_t ws = ReadPtr(dm + (uintptr_t)off);
    return IsReadablePtr(ws) ? ws : 0;
}
uintptr_t InstanceStore::GetService(DWORD pid, const std::string& name) {
    uintptr_t dm = GetDataModel(pid);
    if (!dm) return 0;
    return FindFirstChild(dm, name);
}
uintptr_t InstanceStore::GetLocalPlayer(DWORD pid) {
    uintptr_t players = GetService(pid, "Players");
    if (!players) return 0;
    auto kids = GetChildren(players);
    int64_t lpOff = OffsetOr("Player", "LocalPlayer", 288);
    for (auto p : kids) {
        uint8_t flag = 0;
        if (mem_->CustomRead(p + (uintptr_t)lpOff, &flag, 1) && flag) return p;
    }
    return kids.empty() ? 0 : kids[0];
}
uintptr_t InstanceStore::GetCharacter(DWORD pid) {
    uintptr_t lp = GetLocalPlayer(pid);
    if (!lp) return 0;
    int64_t off = OffsetOr("Player", "ModelInstance", 648);
    uintptr_t ch = ReadPtr(lp + (uintptr_t)off);
    if (IsReadablePtr(ch)) return ch;
    return FindFirstChildOfClass(ResolvePath(GetDataModel(pid), "Workspace"), "Model");
}
uintptr_t InstanceStore::GetHumanoid(DWORD pid) {
    uintptr_t ch = GetCharacter(pid);
    if (!ch) return 0;
    uintptr_t h = FindFirstChildOfClass(ch, "Humanoid");
    return h;
}
