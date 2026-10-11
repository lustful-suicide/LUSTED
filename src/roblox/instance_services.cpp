#include "roblox/instance.h"
#include "platform/windows/memory.h"
#include "support/log.h"
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
