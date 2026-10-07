#include "memory.h"
#include "log.h"
#include <iostream>

typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
typedef NTSTATUS(NTAPI* NtWriteVirtualMemory_t)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
typedef NTSTATUS(NTAPI* NtReadVirtualMemory_t)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
typedef NTSTATUS(NTAPI* NtProtectVirtualMemory_t)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);

static NtWriteVirtualMemory_t pNtWrite = nullptr;
static NtReadVirtualMemory_t pNtRead = nullptr;
static NtProtectVirtualMemory_t pNtProtect = nullptr;

static bool ResolveNt() {
    if (pNtWrite && pNtRead && pNtProtect) return true;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;
    pNtWrite   = (NtWriteVirtualMemory_t)GetProcAddress(ntdll, "NtWriteVirtualMemory");
    pNtRead    = (NtReadVirtualMemory_t)GetProcAddress(ntdll, "NtReadVirtualMemory");
    pNtProtect = (NtProtectVirtualMemory_t)GetProcAddress(ntdll, "NtProtectVirtualMemory");
    return pNtWrite && pNtRead && pNtProtect;
}

bool Memory::Attach(DWORD pid) {
    Detach();
    if (!ResolveNt()) return false;
    // Read-only rights for the handle we hold for the process lifetime.
    // Write rights are taken and dropped inside CustomWrite instead, so the
    // process never sits on a VM_WRITE handle while idle.
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    hProc_ = h; pid_ = pid;
    LustedLogf("attach pid=%lu rights=READ|QUERY", (unsigned long)pid);
    return true;
}

void Memory::Detach() {
    if (hProc_) { CloseHandle(hProc_); hProc_ = nullptr; pid_ = 0; LustedLog("detach"); LustedLogFlush(); }
}

bool Memory::CustomWrite(uintptr_t address, const void* data, size_t size) {
    if (!IsOpen() || !address || !data || !size) return false;
    if (!ResolveNt()) return false;

    // Phase timers: anything over a millisecond is logged with its breakdown so
    // the slow phase is named instead of guessed at.
    LARGE_INTEGER fr, ta, tb, tc, td, te;
    QueryPerformanceFrequency(&fr);
    const auto us = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)fr.QuadPart;
    };
    QueryPerformanceCounter(&ta);

    // Write rights are scoped to this call: open, use, drop.
    HANDLE hw = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid_);
    if (!hw) {
        LustedLogf("write OPENFAIL addr=0x%llx size=%zu", (unsigned long long)address, size);
        return false;
    }

    // No VirtualQueryEx here: against a live target it measured 1.3-26 ms per
    // call, which dwarfs the write itself. NtWriteVirtualMemory is tried first
    // and the protect/restore dance only runs when it comes up short - that is
    // also exactly when the page is a non-writable (code) page, so the cache
    // flush below lands where it belongs.
    auto writeOnce = [&](size_t from) -> SIZE_T {
        const uint8_t* src = (const uint8_t*)data;
        SIZE_T total = 0;
        while (from + total < size) {
            SIZE_T w = 0;
            NTSTATUS st = pNtWrite(hw, (PVOID)(address + from + total),
                                   (PVOID)(src + from + total), size - from - total, &w);
            if (!NT_SUCCESS(st) || w == 0) break;
            total += w;
        }
        return total;
    };

    SIZE_T done = writeOnce(0);
    bool didProtect = false;
    if (done != size) {
        PVOID base = (PVOID)address;
        SIZE_T region = size;
        ULONG oldProt = 0;
        if (NT_SUCCESS(pNtProtect(hw, &base, &region, PAGE_EXECUTE_READWRITE, &oldProt))) {
            const SIZE_T retry = writeOnce(done);
            done += retry;
            PVOID rb = (PVOID)address; SIZE_T rs = size; ULONG tmp = 0;
            pNtProtect(hw, &rb, &rs, oldProt ? oldProt : PAGE_EXECUTE_READ, &tmp);
            FlushInstructionCache(hw, (LPCVOID)address, size);
            didProtect = true;
        }
    }
    CloseHandle(hw);
    QueryPerformanceCounter(&tb);

    if (done != size) {
        LustedLogf("write SHORT addr=0x%llx want=%zu got=%zu",
                   (unsigned long long)address, size, done);
        return false;
    }
    // Verify by reading back through the persistent read handle.
    std::vector<uint8_t> back(size);
    const bool verified = CustomRead(address, back.data(), size) &&
                          memcmp(back.data(), data, size) == 0;
    QueryPerformanceCounter(&tc);

    char line[256];
    snprintf(line, sizeof(line), "write addr=0x%llx size=%zu verify=%s mode=%s",
             (unsigned long long)address, size, verified ? "ok" : "MISMATCH",
             didProtect ? "protect" : "direct");
    LustedLog(line);
    QueryPerformanceCounter(&td);
    const double wallUs = us(ta, td);
    if (wallUs > 1000.0)
        LustedLogf("SLOW write addr=0x%llx core=%.0fus (write=%.0f verify=%.0f) log=%.0f",
                   (unsigned long long)address, us(ta, tb), us(tb, tc), us(tc, td));
    return verified;
}

bool Memory::CustomRead(uintptr_t address, void* out, size_t size) {
    if (!IsOpen() || !address || !out || !size) return false;
    if (!ResolveNt()) return false;
    SIZE_T r = 0;
    NTSTATUS st = pNtRead(hProc_, (PVOID)address, out, size, &r);
    return NT_SUCCESS(st) && r == size;
}

uintptr_t Memory::Scan(const uint8_t* pattern, const char* mask, uintptr_t begin, size_t len) {
    std::vector<uint8_t> buf(len);
    if (!CustomRead(begin, buf.data(), len)) return 0;
    size_t mlen = strlen(mask);
    for (size_t i = 0; i + mlen <= len; ++i) {
        bool hit = true;
        for (size_t j = 0; j < mlen; ++j)
            if (mask[j] == 'x' && buf[i + j] != pattern[j]) { hit = false; break; }
        if (hit) return begin + i;
    }
    return 0;
}
