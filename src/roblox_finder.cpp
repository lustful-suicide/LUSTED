#include "roblox_finder.h"
#include <filesystem>
#include <cstdlib>
#include <tlhelp32.h>

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
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
#ifdef UNICODE
            if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
#else
            if (_stricmp(pe.szExeFile, "RobloxPlayerBeta.exe") == 0) {
#endif
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

std::optional<RobloxInstall> FindRobloxPlayer() {
    // 1) Live process image path (most accurate when running).
    DWORD pid = FindRobloxPid();
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            char buf[MAX_PATH] = {};
            DWORD n = sizeof(buf);
            if (QueryFullProcessImageNameA(h, 0, buf, &n) && n) {
                CloseHandle(h);
                RobloxInstall r;
                r.exePath = buf;
                r.versionHash = ExtractVersionHash(r.exePath);
                r.fileVersion = GetExeFileVersion(r.exePath);
                r.pid = pid;
                return r;
            }
            CloseHandle(h);
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
