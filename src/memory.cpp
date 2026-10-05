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
    if (hProc_) { CloseHandle(hProc_); hProc_ = nullptr; pid_ = 0; LustedLog("detach"); }
}

bool Memory::CustomWrite(uintptr_t address, const void* data, size_t size) {
    if (!IsOpen() || !address || !data || !size) return false;
    if (!ResolveNt()) return false;

    // Write rights are scoped to this call: open, use, drop.
    HANDLE hw = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid_);
    if (!hw) {
        LustedLogf("write OPENFAIL addr=0x%llx size=%zu", (unsigned long long)address, size);
        return false;
    }

    bool ok = false;
    PVOID base = (PVOID)address;
    SIZE_T region = size;
    ULONG oldProt = 0;
    // 1) Make writable (no WriteProcessMemory anywhere in this binary path).
    NTSTATUS st = pNtProtect(hw, &base, &region, PAGE_EXECUTE_READWRITE, &oldProt);
    if (!NT_SUCCESS(st)) {
        // Try without protect change (already writable pages).
        oldProt = 0; region = 0;
    }
    // 2) Raw NT write in small chunks so partial-write failures are visible.
    SIZE_T done = 0;
    const uint8_t* src = (const uint8_t*)data;
    while (done < size) {
        SIZE_T w = 0;
        st = pNtWrite(hw, (PVOID)(address + done), (PVOID)(src + done), size - done, &w);
        if (!NT_SUCCESS(st) || w == 0) break;
        done += w;
        if (w != size - (done - w) && w == 0) break;
    }
    // 3) Restore protection.
    if (region) {
        PVOID rb = (PVOID)address; SIZE_T rs = size; ULONG tmp = 0;
        pNtProtect(hw, &rb, &rs, oldProt ? oldProt : PAGE_EXECUTE_READ, &tmp);
    }
    FlushInstructionCache(hw, (LPCVOID)address, size);
    CloseHandle(hw);

    if (done != size) {
        LustedLogf("write SHORT addr=0x%llx want=%zu got=%zu",
                   (unsigned long long)address, size, done);
        return false;
    }
    // 4) Verify by reading back through the persistent read handle.
    std::vector<uint8_t> back(size);
    if (!CustomRead(address, back.data(), size)) return false;
    ok = memcmp(back.data(), data, size) == 0;
    LustedLogf("write addr=0x%llx size=%zu verify=%s",
               (unsigned long long)address, size, ok ? "ok" : "MISMATCH");
    return ok;
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
