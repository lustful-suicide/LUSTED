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

using Clock = std::chrono::steady_clock;

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
    // Each depth level fans out over a bounded worker set (results[i] = children of
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
            unsigned nthreads = std::clamp(thw ? thw : 1u, 1u, 8u);
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
    // Keep the process scan bounded on machines with many logical CPUs; the
    // per-job buffers and remote reads otherwise multiply peak memory use.
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw : 1u, 1u, 8u);
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
    unsigned threads = std::clamp(hw ? hw : 1u, 1u, 8u);
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

