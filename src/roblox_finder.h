#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <windows.h>

struct RobloxInstall {
    std::string exePath;     // full path to RobloxPlayerBeta.exe
    std::string versionHash; // e.g. "02c37bc51a384b8f" (folder "version-xxxx")
    std::string fileVersion; // e.g. "0.681.0.12345" from VERSION resource
    DWORD pid = 0;           // 0 if not running
};

// Finds RobloxPlayerBeta.exe on disk, extracts version hash, file version,
// and live PID when the player is running.
std::optional<RobloxInstall> FindRobloxPlayer();
std::string ExtractVersionHash(const std::string& exePath);
std::string GetExeFileVersion(const std::string& exePath);
DWORD FindRobloxPid();
