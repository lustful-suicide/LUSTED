#pragma once
// Native process/memory introspection behind direct syscalls.
//
// Small interface, everything hookable stays out: no Toolhelp snapshot, no
// OpenProcess/CloseHandle/VirtualQueryEx/QueryFullProcessImageName wrappers,
// no GetSystemInfo, no GetModuleHandle. String conversion and device-path parsing
// are done by hand so this module traps to the kernel and nothing else.
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct ProcEntry {
    DWORD pid = 0;
    std::string imageNt; // device path, e.g. \Device\HarddiskVolume3\...; empty for Idle/System
};

// One system-process snapshot call plus parsing.
// Replaces CreateToolhelp32Snapshot/Process32First/Next for PID listing.
bool ProcListAll(std::vector<ProcEntry>& out);

// Process image-file query plus \??\ drive-letter
// mapping via the object manager. Replaces QueryFullProcessImageNameA.
bool ProcImagePath(HANDLE hProc, std::string& win32Path);

// Remote PEB_LDR walk through MemRead. Replaces the Toolhelp
// module snapshot (TH32CS_SNAPMODULE) for base/size lookup.
bool ModuleLocate(HANDLE hProc, const char* moduleName, uintptr_t& base, size_t& size);

// Virtual-memory region query. Replaces VirtualQueryEx.
bool MemDescribe(HANDLE hProc, uintptr_t addr, MEMORY_BASIC_INFORMATION& mbi);

// x64 user address range as constants (minimum allocation .. top of user
// space, clamped to the canonical half). No call at all; replaces the
// GetSystemInfo queries in the heap sweep.
void AddrRange(uintptr_t& lo, uintptr_t& hi);

// ImageBaseAddress straight from the PEB. No call; replaces
// GetModuleHandleA(nullptr).
HMODULE CurrentModule();
