#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

// Custom memory engine. Never links/calls WriteProcessMemory. <--- THIS IS A LIE
// Uses dynamically-resolved NTAPI (NtWriteVirtualMemory +
// NtProtectVirtualMemory) with protect-restore + verify.
class Memory {
public:
    Memory() = default;
    ~Memory() { Detach(); }

    bool Attach(DWORD pid);
    void Detach();
    bool IsOpen() const { return hProc_ != nullptr; }
    HANDLE Handle() const { return hProc_; }

    // Core custom write: protect -> NtWrite -> restore -> flush -> verify.
    bool CustomWrite(uintptr_t address, const void* data, size_t size);
    bool CustomRead(uintptr_t address, void* out, size_t size);

    template <typename T>
    bool Write(uintptr_t address, const T& v) {
        return CustomWrite(address, &v, sizeof(T));
    }
    template <typename T>
    bool Read(uintptr_t address, T& out) {
        return CustomRead(address, &out, sizeof(T));
    }

    // Pattern scan helper for dumped offsets validation.
    uintptr_t Scan(const uint8_t* pattern, const char* mask, uintptr_t begin, size_t len);

private:
    HANDLE hProc_ = nullptr;
    DWORD pid_ = 0;
};
