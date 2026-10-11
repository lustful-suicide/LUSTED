#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

// Reads and writes the target process through Windows NT memory APIs. Write
// access is requested for the duration of each write and released immediately.
class Memory {
public:
    Memory() = default;
    ~Memory() { Detach(); }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;

    bool Attach(DWORD pid);
    void Detach();
    bool IsOpen() const { return hProc_ != nullptr; }
    HANDLE Handle() const { return hProc_; }

    // Write bytes to the target and verify the result.
    bool CustomWrite(uintptr_t address, const void* data, size_t size);
    // Write bytes and confirm the full transfer without a read-back. Intended
    // for latency-sensitive transform updates where a second read races the
    // target's own simulation and success logging would dominate the write.
    bool CustomWriteFast(uintptr_t address, const void* data, size_t size);
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
    bool CustomWriteImpl(uintptr_t address, const void* data, size_t size,
                         bool verify, bool logSuccess);
};
