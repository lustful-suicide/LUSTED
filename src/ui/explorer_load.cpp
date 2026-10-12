#include "ui/explorer.h"
#include "roblox/instance.h"
#include "imgui.h"
#include "support/log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

// Shared with explorer_view.cpp (same cap; guarded so src/ui/explorer.cpp
// can include both TUs without a redefinition).
#ifndef LUSTED_EXPLORER_KMAXKIDS_DEFINED
#define LUSTED_EXPLORER_KMAXKIDS_DEFINED
constexpr size_t kMaxKids = 2000;
#endif
constexpr size_t kMaxProps = 400;

std::string AddrStr(uintptr_t a) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", (unsigned long long)a);
    return b;
}

} // namespace

void Explorer::LoadKids(uintptr_t addr) {
    InstanceStore* st = store_.load(std::memory_order_acquire);
    if (!st || !addr) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = nodes_.find(addr);
        if (it != nodes_.end() && it->second.kidsLoaded) return;
    }
    // Remote reads run lock-free (the loader thread shares nodes_).
    const std::string name = st->GetName(addr);
    const std::string cls = st->GetClass(addr);
    // Snapshot + live vectors only: instant, never a heap sweep.
    std::vector<uintptr_t> kids = st->GetChildren(addr, false);
    const bool truncated = kids.size() > kMaxKids;
    if (truncated) kids.resize(kMaxKids);
    std::vector<std::string> names(kids.size()), clss(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        names[i] = st->GetName(kids[i]);
        clss[i] = st->GetClass(kids[i]);
    }
    bool needDeep = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Node& node = nodes_[addr];
        if (node.kidsLoaded) return; // background loader beat us
        if (node.name.empty()) node.name = name;
        if (node.cls.empty()) node.cls = cls;
        node.kids = std::move(kids);
        node.kidNames = std::move(names);
        node.kidCls = std::move(clss);
        node.truncated = truncated;
        node.kidsLoaded = true;
        // Empty after snapshot: hidden vector (Players/Workspace hold 1.5GB
        // of instances behind concealed vectors). Deep load covers it.
        if (node.kids.empty() && !node.deepDone && !node.deepLoading) {
            node.deepLoading = true;
            needDeep = true;
        }
    }
    if (needDeep) QueueDeepLoad(addr);
}

void Explorer::QueueDeepLoad(uintptr_t addr) {
    if (!store_.load(std::memory_order_acquire) || !addr) return;
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        if (!deepQueued_.insert(addr).second) return;
        deepQueue_.push_back(addr);
    }
    std::call_once(loaderOnce_, [&] { loader_ = std::thread(&Explorer::LoaderLoop, this); });
    qcv_.notify_one();
}

void Explorer::LoaderLoop() {
    for (;;) {
        uintptr_t addr = 0;
        bool haveWork = false;
        {
            std::unique_lock<std::mutex> lk(qmtx_);
            // Idle 5s with no deep loads -> run the live refresh pass instead
            // of sleeping: open nodes stay current as instances spawn/despawn.
            qcv_.wait_for(lk, std::chrono::seconds(5),
                          [&] { return stopLoader_.load() || !deepQueue_.empty(); });
            if (stopLoader_.load()) return;
            if (!deepQueue_.empty()) {
                addr = deepQueue_.front();
                deepQueue_.pop_front();
                haveWork = true;
            }
        }
        if (!haveWork) {
            std::shared_lock storeLock(storeAccessMutex_);
            if (open_.load()) RefreshExpanded();
            continue;
        }
        // Full lookup: snapshot + vectors + one throttled sweep shared with
        // every other misser. Runs here, never on the UI thread.
        std::vector<uintptr_t> kids;
        std::vector<std::string> names, clss;
        size_t found = 0;
        {
            std::shared_lock storeLock(storeAccessMutex_);
            InstanceStore* st = store_.load(std::memory_order_acquire);
            if (st && addr) {
                kids = st->GetChildren(addr, true);
                if (kids.size() > kMaxKids) kids.resize(kMaxKids);
                names.resize(kids.size());
                clss.resize(kids.size());
                for (size_t i = 0; i < kids.size(); ++i) {
                    names[i] = st->GetName(kids[i]);
                    clss[i] = st->GetClass(kids[i]);
                }
                found = kids.size();
            }
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            Node& node = nodes_[addr];
            node.kids = std::move(kids);
            node.kidNames = std::move(names);
            node.kidCls = std::move(clss);
            node.kidsLoaded = true;
            node.deepLoading = false;
            node.deepDone = true;
        }
        {
            std::lock_guard<std::mutex> lk(qmtx_);
            deepQueued_.erase(addr);
        }
        if (found) {
            char b[96];
            snprintf(b, sizeof(b), "explorer deep %zu for 0x%llX",
                     found, (unsigned long long)addr);
            LustedLog(b);
        }
    }
}

Explorer::~Explorer() {
    stopLoader_.store(true);
    qcv_.notify_all();
    if (loader_.joinable()) loader_.join();
}

void Explorer::WithStoreExclusive(const std::function<void()>& action) {
    std::unique_lock lock(storeAccessMutex_);
    action();
}

void Explorer::SetStore(InstanceStore* store) {
    std::unique_lock lock(storeAccessMutex_);
    store_.store(store, std::memory_order_release);
}

void Explorer::ClearStore() {
    std::unique_lock lock(storeAccessMutex_);
    store_.store(nullptr, std::memory_order_release);
}

void Explorer::RefreshExpanded() {
    InstanceStore* st = store_.load(std::memory_order_acquire);
    if (!st) return;
    std::vector<uintptr_t> addrs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        addrs.assign(expanded_.begin(), expanded_.end());
    }
    for (uintptr_t addr : addrs) {
        if (stopLoader_.load()) return;
        // Dead parent: drop the whole subtree from the tree.
        if (!st->IsAlive(addr)) {
            std::lock_guard<std::mutex> lk(mtx_);
            nodes_.erase(addr);
            expanded_.erase(addr);
            if (selected_ == addr) {
                selected_ = 0;
                selProps_.clear();
                selName_.clear(); selCls_.clear();
            }
            continue;
        }
        std::vector<uintptr_t> fresh = st->GetChildren(addr, false);
        std::unordered_set<uintptr_t> freshSet(fresh.begin(), fresh.end());
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = nodes_.find(addr);
        if (it == nodes_.end()) continue;
        Node& node = it->second;
        // Drop only proven-dead children (stale snapshot reads must not flap
        // live rows away); re-read "?" names that failed transiently.
        for (size_t i = 0; i < node.kids.size();) {
            uintptr_t c = node.kids[i];
            bool dead = !st->IsAlive(c) || st->GetParent(c) != addr;
            if (dead) {
                if (selected_ == c) {
                    selected_ = 0;
                    selProps_.clear();
                    selName_.clear(); selCls_.clear();
                }
                node.kids.erase(node.kids.begin() + (ptrdiff_t)i);
                node.kidNames.erase(node.kidNames.begin() + (ptrdiff_t)i);
                node.kidCls.erase(node.kidCls.begin() + (ptrdiff_t)i);
                continue;
            }
            if (node.kidNames[i].empty()) {
                node.kidNames[i] = st->GetName(c);
                if (node.kidCls[i].empty()) node.kidCls[i] = st->GetClass(c);
            }
            ++i;
        }
        // Arrivals.
        std::unordered_set<uintptr_t> have(node.kids.begin(), node.kids.end());
        for (uintptr_t c : fresh) {
            if (stopLoader_.load()) return;
            if (!have.insert(c).second) continue;
            if (!st->IsAlive(c) || st->GetParent(c) != addr) continue;
            node.kids.push_back(c);
            node.kidNames.push_back(st->GetName(c));
            node.kidCls.push_back(st->GetClass(c));
        }
        if (selected_ == addr) LoadProps(addr);
    }
}

void Explorer::LoadProps(uintptr_t addr) {
    selProps_.clear();
    selName_.clear(); selCls_.clear(); selAddr_.clear();
    selParent_.clear(); selCounts_.clear();
    InstanceStore* st = store_.load(std::memory_order_acquire);
    if (!st || !addr) return;
    selCls_ = st->GetClass(addr);
    selName_ = st->GetName(addr);
    selAddr_ = AddrStr(addr);
    uintptr_t parent = st->GetParent(addr);
    if (parent) selParent_ = st->GetName(parent) + " (" + AddrStr(parent) + ")";
    else selParent_ = "(none)";
    auto members = st->MembersOfClass(selCls_);
    // child count is cheap-ish (cached vectors); show it in the header.
    size_t nkids = 0;
    {
        auto it = nodes_.find(addr);
        if (it != nodes_.end() && it->second.kidsLoaded) nkids = it->second.kids.size();
        else nkids = st->GetChildren(addr, false).size();
    }
    char cb[96];
    snprintf(cb, sizeof(cb), "%zu children, %zu members", nkids, members.size());
    selCounts_ = cb;
    const size_t n = std::min(members.size(), kMaxProps);
    selProps_.reserve(n);
    for (size_t i = 0; i < n; ++i)
        selProps_.emplace_back(members[i], st->ReadPropDisplay(addr, selCls_, members[i]));
    if (members.size() > kMaxProps) selProps_.emplace_back("...", "(truncated)");
}
