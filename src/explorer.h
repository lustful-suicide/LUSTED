#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>
#include <windows.h>

class InstanceStore;

// Standalone Roblox instance explorer (own file by design): left tree lazily
// lists DataModel children, clicking an instance loads its properties on the
// right. Snapshot fills instantly; hidden vectors (Players/Workspace) deep
// load on a background thread so the UI never stalls on a heap sweep.
class Explorer {
public:
    Explorer() = default;
    ~Explorer();

    void SetStore(InstanceStore* store) { store_ = store; }
    void SetPidPtr(DWORD* pid) { pid_ = pid; }
    void SetOpen(bool open) { open_.store(open); }
    bool IsOpen() const { return open_.load(); }

    void Render();

private:
    struct Node {
        std::string name;
        std::string cls;
        bool kidsLoaded = false;
        bool deepLoading = false;
        bool deepDone = false;
        std::vector<uintptr_t> kids;
        std::vector<std::string> kidNames;
        std::vector<std::string> kidCls;
        bool truncated = false;
    };

    void DrawNode(uintptr_t addr, int depth);
    void LoadKids(uintptr_t addr);
    void LoadProps(uintptr_t addr);
    // Background deep load: full lookup (heap-sweep gated) off the UI thread
    // for hidden vectors (Players/Workspace). Never blocks Render.
    void QueueDeepLoad(uintptr_t addr);
    void LoaderLoop();
    // Live pass: re-reads open nodes (adds arrivals, drops the freed,
    // retries "?" names). Runs on the loader thread, never the UI thread.
    void RefreshExpanded();

    InstanceStore* store_ = nullptr;
    DWORD* pid_ = nullptr;
    std::atomic<bool> open_{ true };
    mutable std::mutex mtx_;
    std::unordered_map<uintptr_t, Node> nodes_;
    std::unordered_set<uintptr_t> expanded_;
    std::unordered_set<uintptr_t> deepQueued_;
    std::deque<uintptr_t> deepQueue_;
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::thread loader_;
    std::once_flag loaderOnce_;
    std::atomic<bool> stopLoader_{ false };
    uintptr_t root_ = 0;
    DWORD rootPid_ = 0;
    uintptr_t selected_ = 0;
    bool firstShown_ = false;
    // selected header + property rows (cached, refreshed on select/Refresh)
    std::string selName_, selCls_, selAddr_, selParent_, selCounts_;
    std::vector<std::pair<std::string, std::string>> selProps_;
    char filter_[128] = {};
};
