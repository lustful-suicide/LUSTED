#include "roblox/session.h"
#include "platform/windows/syscalls.h"
#include "platform/windows/memory.h"
#include "roblox/instance.h"
#include "runtime/luau_manager.h"
#include "roblox/roblox_finder.h"
#include "app/startup.h"
#include "support/log.h"
#include <cstring>
#include <iostream>
#include <chrono>

namespace {
constexpr LONG kStillActive = 0x103;
constexpr uint64_t kCheckIntervalMs = 1000;
constexpr uint64_t kRetryIntervalMs = 2000;
constexpr uint64_t kMissingPlayerGraceMs = 5000;
constexpr uint64_t kPlayerIndexRefreshMs = 10000;
}

SessionMonitor::~SessionMonitor() {
    StopIndexRefresh();
    if (experienceBuildThread_.joinable()) experienceBuildThread_.join();
}

SessionMonitor::SessionMonitor(Memory* mem, InstanceStore* inst, LuauManager* luau,
                               DWORD* pidOut, std::filesystem::path autoDir)
    : mem_(mem), inst_(inst), luau_(luau), pid_(pidOut), autoDir_(std::move(autoDir)) {
    waiting_ = !HasExperiencePlayer();
    nextCheckMs_ = GetTickCount64();
    if (waiting_ && luau_) luau_->ResetSessionScripts();
    else if (pid_ && *pid_) {
        if (inst_) inst_->ResetCachedDataModel();
        const uintptr_t dm = inst_ ? inst_->GetDataModel(*pid_) : 0;
        std::string id = inst_ ? inst_->GetExperienceIdentity(*pid_) : std::string();
        if (id.empty()) id = "dm:" + std::to_string(dm);
        else id += "|dm:" + std::to_string(dm);
        experienceIdentity_ = id;
        StartIndexRefresh(*pid_);
    }
}

bool SessionMonitor::ProcExited() const {
    if (!mem_ || !mem_->IsOpen()) return true;
    HANDLE h = mem_->Handle();
    if (!h) return true;
    uint8_t pbi[8] = {};
    ULONG rl = 0;
    if (!SYS_OK(ProcQuery(h, 0, pbi, sizeof(pbi), &rl))) return false;
    LONG status = 0;
    memcpy(&status, pbi, sizeof(status));
    return status != kStillActive;
}

bool SessionMonitor::HasExperiencePlayer() const {
    return mem_ && mem_->IsOpen() && pid_ && *pid_ && inst_ &&
           inst_->GetLocalPlayerFast(*pid_) != 0;
}

void SessionMonitor::Poll() {
    const uint64_t now = GetTickCount64();
    if (now < nextCheckMs_) return;
    nextCheckMs_ = now + kCheckIntervalMs;

    const bool attached = mem_ && mem_->IsOpen() && pid_ && *pid_;
    if (attached && ProcExited()) {
        OnLeft();
        return;
    }

    if (!attached) {
        if (now >= nextRetryMs_) {
            nextRetryMs_ = now + kRetryIntervalMs;
            TryRejoin();
        }
        return;
    }

    if (HasExperiencePlayer()) {
        missingPlayerSinceMs_ = 0;
        if (!waiting_) {
            if (inst_) inst_->ResetCachedDataModel();
            const uintptr_t currentDm = inst_ ? inst_->GetDataModel(*pid_) : 0;
            std::string identity = inst_ ? inst_->GetExperienceIdentity(*pid_) : std::string();
            if (identity.empty())
                identity = "dm:" + std::to_string(currentDm);
            else
                identity += "|dm:" + std::to_string(currentDm);
            if (!identity.empty() && !experienceIdentity_.empty() &&
                identity != experienceIdentity_) {
                if (candidateIdentity_ == identity) ++candidateIdentityChecks_;
                else {
                    candidateIdentity_ = identity;
                    candidateIdentityChecks_ = 1;
                }
                if (candidateIdentityChecks_ >= 2) {
                    experienceIdentity_ = identity;
                    candidateIdentity_.clear();
                    candidateIdentityChecks_ = 0;
                    OnExperienceLeft();
                    return;
                }
            } else {
                candidateIdentity_.clear();
                candidateIdentityChecks_ = 0;
                if (experienceIdentity_.empty()) experienceIdentity_ = identity;
            }
        }
        if (waiting_) StartExperience();
        return;
    }

    if (missingPlayerSinceMs_ == 0) missingPlayerSinceMs_ = now;
    if (!waiting_ && now - missingPlayerSinceMs_ >= kMissingPlayerGraceMs)
        OnExperienceLeft();
}

void SessionMonitor::OnLeft() {
    stopIndexRefresh_.store(true, std::memory_order_release);
    if (inst_) inst_->RequestIndexCancel();
    StopIndexRefresh();
    if (experienceBuildThread_.joinable()) experienceBuildThread_.join();
    experienceBuildState_.store(0, std::memory_order_release);
    if (mem_ && mem_->IsOpen()) mem_->Detach();
    if (inst_) {
        inst_->InvalidateIndex();
        inst_->ResetDataModelCache();
    }
    if (luau_) luau_->ResetSessionScripts();
    if (pid_) *pid_ = 0;
    waiting_ = true;
    missingPlayerSinceMs_ = 0;
    nextRetryMs_ = GetTickCount64();
    std::cout << "[session] process ended; waiting for next experience..." << std::endl;
    LustedLog("session process ended; waiting for next experience");
    LustedLogFlush();
}

void SessionMonitor::OnExperienceLeft() {
    stopIndexRefresh_.store(true, std::memory_order_release);
    if (inst_) inst_->RequestIndexCancel();
    StopIndexRefresh();
    if (experienceBuildThread_.joinable()) experienceBuildThread_.join();
    experienceBuildState_.store(0, std::memory_order_release);
    waiting_ = true;
    missingPlayerSinceMs_ = 0;
    if (inst_) {
        inst_->InvalidateIndex();
        inst_->ResetDataModelCache();
    }
    if (luau_) luau_->ResetSessionScripts();
    std::cout << "[session] experience ended; waiting for next experience..." << std::endl;
    LustedLog("session experience ended; waiting for next experience");
    LustedLogFlush();
}

void SessionMonitor::StartExperience() {
    const DWORD pid = pid_ ? *pid_ : 0;
    if (!pid || !inst_ || !inst_->GetLocalPlayerFast(pid)) return;
    int state = experienceBuildState_.load(std::memory_order_acquire);
    if (state == 1) return;
    if (state == 3) {
        const uint64_t now = GetTickCount64();
        if (nextBuildRetryMs_ == 0) {
            nextBuildRetryMs_ = now + kRetryIntervalMs;
            return;
        }
        if (now < nextBuildRetryMs_) return;
    }
    if (state != 2) {
        if (experienceBuildThread_.joinable()) experienceBuildThread_.join();
        inst_->InvalidateIndex();
        inst_->ResetDataModelCache();
        nextBuildRetryMs_ = 0;
        experienceBuildState_.store(1, std::memory_order_release);
        experienceBuildThread_ = std::thread([this, pid]() {
            const bool ok = inst_ && inst_->BuildIndex(pid, true);
            experienceBuildState_.store(ok ? 2 : 3, std::memory_order_release);
        });
        return;
    }
    if (experienceBuildThread_.joinable()) experienceBuildThread_.join();
    if (luau_) {
        luau_->SetTargetPid(pid);
        const size_t n = RunAutoFiles(*luau_, autoDir_);
        LustedLogf("session experience ready pid=%lu scripts=%zu",
                   (unsigned long)pid, n);
    }
    waiting_ = false;
    missingPlayerSinceMs_ = 0;
    if (inst_) inst_->ResetCachedDataModel();
    const uintptr_t dm2 = inst_ ? inst_->GetDataModel(pid) : 0;
    std::string id2 = inst_ ? inst_->GetExperienceIdentity(pid) : std::string();
    if (id2.empty()) id2 = "dm:" + std::to_string(dm2);
    else id2 += "|dm:" + std::to_string(dm2);
    experienceIdentity_ = id2;
    candidateIdentity_.clear();
    candidateIdentityChecks_ = 0;
    experienceBuildState_.store(0, std::memory_order_release);
    StartIndexRefresh(pid);
    std::cout << "[session] experience ready pid " << pid << "; index rebuilt" << std::endl;
    LustedLogFlush();
}

void SessionMonitor::StartIndexRefresh(DWORD pid) {
    StopIndexRefresh();
    if (!inst_ || !mem_ || !pid) return;
    stopIndexRefresh_.store(false, std::memory_order_release);
    indexRefreshThread_ = std::thread([this, pid]() {
        uint64_t elapsed = 0;
        while (elapsed < kPlayerIndexRefreshMs &&
               !stopIndexRefresh_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            elapsed += 250;
        }
        while (!stopIndexRefresh_.load(std::memory_order_acquire)) {
            if (mem_ && mem_->IsOpen() && !ProcExited())
                inst_->RefreshPlayers(pid);
            elapsed = 0;
            while (elapsed < kPlayerIndexRefreshMs &&
                   !stopIndexRefresh_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                elapsed += 250;
            }
        }
    });
}

void SessionMonitor::StopIndexRefresh() {
    stopIndexRefresh_.store(true, std::memory_order_release);
    if (indexRefreshThread_.joinable()) indexRefreshThread_.join();
}

void SessionMonitor::TryRejoin() {
    const DWORD pid = FindRobloxPid();
    if (!pid || !mem_ || !mem_->Attach(pid)) return;
    if (pid_) *pid_ = pid;
    if (luau_) luau_->SetTargetPid(pid);
    if (inst_) {
        inst_->ResetDataModelCache();
        inst_->InvalidateIndex();
    }
    missingPlayerSinceMs_ = 0;
    nextCheckMs_ = GetTickCount64();
    std::cout << "[session] attached pid " << pid << "; waiting for experience..." << std::endl;
    LustedLogf("session attached pid=%lu; waiting for active player", (unsigned long)pid);
    LustedLogFlush();
}
