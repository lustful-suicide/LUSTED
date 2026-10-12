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
    // 2 MiB windows: transfer is trap-bound, so fewer windows win outright.
    // A failed window falls straight to pages; guard-split regions fail
    // narrowly there. One retry covers transient heap churn (streaming map).
    constexpr size_t kChunk = 0x200000;
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

// FAST PATH: level-parallel BFS from DataModel via live children vectors.
// 2 bulk reads per node, zero per-child verification, zero heap transfer.
// Each depth level fans out over a bounded worker set (results[i] = children of
// frontier[i], merged at the level barrier), so a 120k-node tree costs
// ~240k small reads at 8-wide parallelism instead of serially.
// Vectors are authoritative live data; the heap sweep unions in
// hidden vectors (Workspace/Players map content) afterwards.
void InstanceStore::RunTraversal(uintptr_t dm, Clock::time_point t0) {
    if (!dm || indexCancel_.load(std::memory_order_relaxed)) return;
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
    const unsigned maxThreads = std::clamp(thw ? thw : 1u, 1u, 8u);
    std::vector<uintptr_t> next;
    next.reserve(1 << 16);
    while (!frontier.empty()) {
        if (indexCancel_.load(std::memory_order_relaxed)) break;
        if (visited.size() > 120000) break;
        const size_t F = frontier.size();
        // Per-level width: the old code shrank the shared nthreads on narrow
        // levels and never restored it, so one thin level near the root ran
        // every later wide level nearly serially.
        const unsigned levelThreads = std::min<unsigned>(maxThreads, (unsigned)F);
        std::vector<std::vector<uintptr_t>> results(F);
        std::atomic<size_t> cur{ 0 };
        const int64_t csOff = oChildrenStart_;
        Memory* m = mem_;
        std::vector<std::thread> workers;
        workers.reserve(levelThreads);
        for (unsigned t = 0; t < levelThreads; ++t) {
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
        std::ostringstream os;
        os << "[index-fast] " << count << " instances / " << fast.size()
           << " parents via traversal in " << (int)tfms << "ms (total " << (int)ms << "ms)";
        std::cout << os.str() << std::endl;
        // Fall through to the heap sweep below: traversal misses hidden
        // vectors (Workspace/Players map content) and garbage caps drop
        // whole subtrees, so the sweep unions the rest in. Ready is
        // already set, so scripts run on the warm snapshot meanwhile.
    }
    // else the heap sweep covers it (<1000 reachable: probably in menu / loading)
}

bool InstanceStore::BuildIndex(DWORD pid, bool force, bool deep) {
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

    // Deep mode runs the vector traversal first (warm snapshot + links the
    // heap pattern can't see); default is sweep-only. On forced-2GB heaps the
    // traversal's ~240k small reads cost more than the whole sweep, and the
    // sweep already observes the large majority of instances, so the default
    // build is the sweep. Pass deep=true (refresh_index with 2nd arg true)
    // when vector-only links are missing.
    if (deep) {
        uintptr_t dm = GetDataModel(pid);
        if (dm && !indexCancel_.load(std::memory_order_relaxed))
            RunTraversal(dm, t0);
    }

    uintptr_t lo = 0, hi = 0;
    AddrRange(lo, hi);
    const auto tWalk0 = Clock::now();

    struct Reg { uintptr_t base; size_t size; DWORD type; };
    std::vector<Reg> regs;
    regs.reserve(2048);
    size_t privateBytes = 0, mappedBytes = 0;
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!MemDescribe(h, a, mbi)) break;
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
    const auto tXfer0 = Clock::now();

    // Job-split: a single 100 MiB region used to pin one thread while the rest
    // idled. Slicing every region into ~512 KiB jobs keeps all workers fed;
    // overlapping jobs dedupe globally at merge.
    struct Job { uintptr_t base; size_t size; size_t regIdx; };
    std::vector<Job> jobs;
    jobs.reserve(regs.size() * 2);
    size_t total = 0;
    for (size_t ri = 0; ri < regs.size(); ++ri) {
        total += regs[ri].size;
        constexpr size_t kJob = 0x200000; // 2 MiB jobs over 2 MiB windows
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
    // 16-wide: the transfer phase is trap-bound (threads sleep in the kernel
    // waiting on the target), so oversubscribing physical cores still gains
    // throughput up to target-side saturation.
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = std::clamp(hw ? hw : 1u, 1u, 16u);
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
    const auto tSort0 = Clock::now();

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
    const auto tMerge0 = Clock::now();

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const auto msSince = [](const auto& from, const auto& to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    const double walkMs = msSince(tWalk0, tXfer0);
    const double xferMs = msSince(tXfer0, tSort0);
    // Union with the previous snapshot. A pass can miss whole regions (reads
    // fail while the heap moves), and partial results are still correct data,
    // so merging keeps every child list monotonic instead of losing entries.
    // Built fully OFF the lock; the exclusive section merges parent runs
    // straight into the slots. flat arrives sorted by (parent,child), so each
    // run merges once - no intermediate map, no second pass over the pairs.
    size_t parents = 0;
    {
        std::unique_lock lk(indexMutex_);
        children_.reserve(children_.size() + flat.size() / 4 + 1024);
        auto mergeRun = [&](uintptr_t parent, std::vector<uintptr_t>& kids) {
            auto& slot = children_[parent];
            if (slot.empty()) {
                slot = std::move(kids);
                return;
            }
            slot.insert(slot.end(), kids.begin(), kids.end());
            std::sort(slot.begin(), slot.end());
            slot.erase(std::unique(slot.begin(), slot.end()), slot.end());
        };
        std::vector<uintptr_t> run;
        uintptr_t curParent = 0;
        bool inRun = false;
        for (auto& [parent, child] : flat) {
            if (!inRun || parent != curParent) {
                if (inRun) mergeRun(curParent, run);
                run.clear();
                curParent = parent;
                inRun = true;
            }
            run.push_back(child);
        }
        if (inRun) mergeRun(curParent, run);
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
    std::ostringstream os;
    os << "[index] " << count << " instances / " << parents << " parents from "
       << (total >> 20) << " MiB in " << (int)ms << "ms (pass " << flatAll.size() << ")"
       << " [private " << (privateBytes >> 20) << "MiB/" << privateHits
       << " mapped " << (mappedBytes >> 20) << "MiB/0]"
       << " [walk " << (int)walkMs << " xfer " << (int)xferMs
       << " sort " << (int)msSince(tSort0, tMerge0)
       << " merge " << (int)msSince(tMerge0, Clock::now()) << "]";
    std::cout << os.str() << std::endl;
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

uint64_t InstanceStore::IndexAgeMs() const {
    std::shared_lock lk(indexMutex_);
    if (!indexReady_) return UINT64_MAX;
    return GetTickCount64() - indexStamp_;
}
