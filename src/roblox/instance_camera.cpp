#include "roblox/instance.h"
#include "platform/windows/memory.h"
#include "platform/windows/process.h"
#include <cstring>
#include <cmath>

// Camera basis for ESP/projection: DataModel -> Workspace -> CurrentCamera.
// Rotation is the 3x3 camera basis, Position the eye point, plus vertical FOV
// and viewport size. Everything is validated (a stale CurrentCamera during
// teleports/streaming reads as garbage, never as a camera).
bool InstanceStore::GetCamera(DWORD pid, uintptr_t& cam, float pos[3], float rot[9],
                              float& fov, float& vpW, float& vpH) {
    cam = 0;
    if (!mem_ || !mem_->IsOpen()) return false;
    const uintptr_t dm = GetDataModel(pid);
    if (!dm) return false;
    const uintptr_t ws = ReadPtr(dm + (uintptr_t)oWorkspace_);
    if (!FastReadable(ws)) return false;
    const uintptr_t c = ReadPtr(ws + (uintptr_t)oWsCamera_);
    if (!FastReadable(c) || !IsAlive(c)) return false;
    float p[3] = {}, r[9] = {}, f = 0, vp[2] = {};
    if (!mem_->CustomRead(c + (uintptr_t)oCamPos_, p, sizeof(p))) return false;
    if (!mem_->CustomRead(c + (uintptr_t)oCamRot_, r, sizeof(r))) return false;
    if (!mem_->CustomRead(c + (uintptr_t)oCamFov_, &f, sizeof(f))) return false;
    if (!mem_->CustomRead(c + (uintptr_t)oCamViewport_, vp, sizeof(vp))) return false;
    if (!(f > 0.0f && f < 180.0f)) return false;
    if (!(vp[0] > 0.0f && vp[1] > 0.0f && vp[0] < 16384.0f && vp[1] < 16384.0f)) return false;
    // Basis must be ~orthonormal: rows of a rotation matrix have unit length.
    for (int i = 0; i < 3; ++i) {
        const float n = r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] + r[i * 3 + 2] * r[i * 3 + 2];
        if (!(n > 0.9f && n < 1.1f)) return false;
    }
    memcpy(pos, p, sizeof(p));
    memcpy(rot, r, sizeof(r));
    fov = f;
    vpW = vp[0];
    vpH = vp[1];
    cam = c;
    return true;
}

bool InstanceStore::GetViewMatrix(DWORD pid, float m[16]) {
    if (!mem_ || !mem_->IsOpen()) return false;
    constexpr uint64_t kModuleRefreshMs = 30000;
    constexpr uint64_t kRetryMs = 1000;
    const uint64_t now = GetTickCount64();
    uintptr_t base = 0;
    {
        std::lock_guard<std::mutex> lock(viewMatrixMutex_);
        if (viewMatrixPid_ != pid) {
            viewMatrixPid_ = pid;
            viewMatrixModuleBase_ = 0;
            viewMatrixResolvedAt_ = 0;
        }
        const bool refresh = viewMatrixResolvedAt_ == 0 ||
            (viewMatrixModuleBase_ != 0 && now - viewMatrixResolvedAt_ >= kModuleRefreshMs) ||
            (viewMatrixModuleBase_ == 0 && now - viewMatrixResolvedAt_ >= kRetryMs);
        if (refresh) {
            uintptr_t resolvedBase = 0;
            size_t resolvedSize = 0;
            ModuleInfo(pid, "RobloxPlayerBeta.exe", resolvedBase, resolvedSize);
            viewMatrixModuleBase_ = resolvedBase;
            viewMatrixResolvedAt_ = now;
        }
        base = viewMatrixModuleBase_;
    }
    if (!base) return false;
    const int64_t veRva = OffsetOr("VisualEngine", "Pointer", 140865088);
    const int64_t vmOff = OffsetOr("VisualEngine", "ViewMatrix", 432);
    const uintptr_t ve = ReadPtr(base + (uintptr_t)veRva);
    if (!FastReadable(ve)) {
        std::lock_guard<std::mutex> lock(viewMatrixMutex_);
        if (viewMatrixPid_ == pid && viewMatrixModuleBase_ == base) {
            viewMatrixModuleBase_ = 0;
            viewMatrixResolvedAt_ = now;
        }
        return false;
    }
    float v[16] = {};
    if (!mem_->CustomRead(ve + (uintptr_t)vmOff, v, sizeof(v))) {
        std::lock_guard<std::mutex> lock(viewMatrixMutex_);
        if (viewMatrixPid_ == pid && viewMatrixModuleBase_ == base) {
            viewMatrixModuleBase_ = 0;
            viewMatrixResolvedAt_ = now;
        }
        return false;
    }
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(v[i]) || v[i] > 1e9f || v[i] < -1e9f) return false;
    memcpy(m, v, sizeof(v));
    return true;
}
