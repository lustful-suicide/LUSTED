#include "roblox/roblox_finder.h"
#include "platform/windows/process.h"
#include "platform/windows/syscalls.h"
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

std::string ExtractVersionHash(const std::string& exePath) {
    // Folder layout: ...\Versions\version-<hash>\RobloxPlayerBeta.exe
    fs::path p(exePath);
    std::string parent = p.parent_path().filename().string();
    const std::string tag = "version-";
    auto pos = parent.find(tag);
    if (pos != std::string::npos)
        return parent.substr(pos + tag.size());
    return "";
}

std::string GetExeFileVersion(const std::string& exePath) {
    DWORD h = 0;
    DWORD sz = GetFileVersionInfoSizeA(exePath.c_str(), &h);
    if (!sz) return "";
    std::string buf(sz, '\0');
    if (!GetFileVersionInfoA(exePath.c_str(), 0, sz, buf.data())) return "";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(buf.data(), "\\", (void**)&fi, &len) || !fi) return "";
    char v[64];
    snprintf(v, sizeof(v), "%u.%u.%u.%u",
        HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
        HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}

DWORD FindRobloxPid() {
    // One system-process snapshot; the image name rides along per
    // process, so no per-process open is needed to match the exe name.
    std::vector<ProcEntry> procs;
    if (!ProcListAll(procs)) return 0;
    for (const auto& p : procs) {
        const char* exe = p.imageNt.c_str();
        const char* slash = exe;
        for (const char* q = exe; *q; ++q)
            if (*q == '\\' || *q == '/') slash = q + 1;
        if (_stricmp(slash, "RobloxPlayerBeta.exe") == 0) return p.pid;
    }
    return 0;
}

std::optional<RobloxInstall> FindRobloxPlayer() {
    // 1) Live process image path (most accurate when running).
    DWORD pid = FindRobloxPid();
    if (pid) {
        SysObjectAttributes oa{};
        oa.Length = sizeof(oa);
        SysClientId cid{};
        cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid;
        HANDLE h = nullptr;
        SysCount(kScOpen);
        if (SYS_OK(ProcOpen(&h, PROCESS_QUERY_LIMITED_INFORMATION, &oa,
                                            &cid)) &&
            h) {
            std::string path;
            if (ProcImagePath(h, path) && !path.empty()) {
                SysCount(kScClose);
                HandleClose(h);
                RobloxInstall r;
                r.exePath = path;
                r.versionHash = ExtractVersionHash(r.exePath);
                r.fileVersion = GetExeFileVersion(r.exePath);
                r.pid = pid;
                return r;
            }
            SysCount(kScClose);
            HandleClose(h);
        }
    }
    // 2) Scan %LOCALAPPDATA%\Roblox\Versions\version-*\RobloxPlayerBeta.exe
    const char* local = std::getenv("LOCALAPPDATA");
    if (local) {
        fs::path versions = fs::path(local) / "Roblox" / "Versions";
        std::error_code ec;
        fs::path best;
        for (auto& e : fs::directory_iterator(versions, ec)) {
            if (ec) break;
            fs::path cand = e.path() / "RobloxPlayerBeta.exe";
            if (fs::exists(cand, ec)) {
                // newest write time wins
                if (best.empty() || fs::last_write_time(cand, ec) > fs::last_write_time(best, ec))
                    best = cand;
            }
        }
        if (!best.empty()) {
            RobloxInstall r;
            r.exePath = best.string();
            r.versionHash = ExtractVersionHash(r.exePath);
            r.fileVersion = GetExeFileVersion(r.exePath);
            r.pid = 0;
            return r;
        }
    }
    // 3) PATH / CWD fallback
    char exp[MAX_PATH] = {};
    if (SearchPathA(nullptr, "RobloxPlayerBeta.exe", nullptr, sizeof(exp), exp, nullptr)) {
        RobloxInstall r;
        r.exePath = exp;
        r.versionHash = ExtractVersionHash(r.exePath);
        r.fileVersion = GetExeFileVersion(r.exePath);
        r.pid = FindRobloxPid();
        return r;
    }
    return std::nullopt;
}
