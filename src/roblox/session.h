#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <atomic>
#include <thread>
#include <string>
class Memory;
class InstanceStore;
class LuauManager;

// Tracks both process exit and experience transitions inside a still-running
// Roblox client. The local-player check uses a direct pointer read so home-screen
// polling never triggers a heap sweep.
class SessionMonitor {
public:
    SessionMonitor(Memory* mem, InstanceStore* inst, LuauManager* luau, DWORD* pidOut,
                   std::filesystem::path autoDir);
    ~SessionMonitor();
    void Poll();

private:
    bool ProcExited() const;
    bool HasExperiencePlayer() const;
    void OnLeft();
    void OnExperienceLeft();
    void StartExperience();
    void StartIndexRefresh(DWORD pid);
    void StopIndexRefresh();
    void TryRejoin();
    Memory* mem_ = nullptr;
    InstanceStore* inst_ = nullptr;
    LuauManager* luau_ = nullptr;
    DWORD* pid_ = nullptr;
    std::filesystem::path autoDir_;
    bool waiting_ = false;
    uint64_t missingPlayerSinceMs_ = 0;
    std::string experienceIdentity_;
    std::string candidateIdentity_;
    unsigned candidateIdentityChecks_ = 0;
    std::atomic<bool> stopIndexRefresh_{ true };
    std::thread indexRefreshThread_;
    std::atomic<int> experienceBuildState_{ 0 }; // 0 idle, 1 running, 2 ready, 3 failed
    uint64_t nextBuildRetryMs_ = 0;
    std::thread experienceBuildThread_;
    uint64_t nextCheckMs_ = 0;
    uint64_t nextRetryMs_ = 0;
};
