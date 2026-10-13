#include "roblox/instance.h"
#include "platform/windows/memory.h"
#include "platform/windows/process.h"
#include "support/log.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <immintrin.h>
#include <nlohmann/json.hpp>

using Clock = std::chrono::steady_clock;

using Clock = std::chrono::steady_clock;

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
std::vector<uintptr_t> InstanceStore::ScanChildrenByParent(uintptr_t parent, unsigned maxThreads) {
    std::vector<uintptr_t> out;
    if (!mem_ || !mem_->IsOpen() || !parent) return out;
    HANDLE h = mem_->Handle();
    if (!h) return out;
    const int64_t parentOff = oParent_;
    uintptr_t lo = 0, hi = 0;
    AddrRange(lo, hi);
    struct Reg { uintptr_t base; size_t size; };
    std::vector<Reg> regs;
    regs.reserve(1024);
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!MemDescribe(h, a, mbi)) break;
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
    unsigned threads = std::clamp(hw ? hw : 1u, 1u, std::max(1u, maxThreads));
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

