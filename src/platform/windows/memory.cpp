#include "platform/windows/memory.h"
#include "platform/windows/syscalls.h"
#include "support/log.h"
#include <cstdio>
#include <cstring>
#include <vector>

// Every kernel transition here is a raw `syscall` from our own image (see
// syscalls_gas.S / syscalls_masm.asm). Nothing calls into ntdll, kernel32's
// OpenProcess/CloseHandle/FlushInstructionCache wrappers, or any OS export,
// so user-mode hooks on those entry points never execute. SSNs resolve once
// via SysEnsureResolved() because the numbers move between Windows builds.

bool Memory::Attach(DWORD pid) {
    Detach();
    if (!SysEnsureResolved()) return false;
    // Read-only rights for the handle we hold for the process lifetime.
    // Write rights are taken and dropped inside CustomWrite instead, so the
    // process never sits on a VM_WRITE handle while idle.
    SysObjectAttributes oa{};
    oa.Length = sizeof(oa);
    SysClientId cid{};
    cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid;
    HANDLE h = nullptr;
    SysCount(kScOpen);
    SysStatus st =
        ProcOpen(&h, PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, &oa, &cid);
    if (!SYS_OK(st) || !h) return false;
    hProc_ = h; pid_ = pid;
    LustedLogf("attach pid=%lu rights=READ|QUERY via=syscall", (unsigned long)pid);
    return true;
}

void Memory::Detach() {
    if (hProc_) {
        SysCount(kScClose);
        HandleClose(hProc_);
        hProc_ = nullptr; pid_ = 0;
        SysCallTotals totals{};
        SysCallTotalsSnapshot(totals);
        LustedLogf("detach traps open=%llu close=%llu read=%llu write=%llu readFull=%llu "
                   "writeFull=%llu protect=%llu flush=%llu sysq=%llu procq=%llu memq=%llu "
                   "linkopen=%llu linkq=%llu",
                   totals.n[kScOpen], totals.n[kScClose], totals.n[kScRead],
                   totals.n[kScWrite], totals.n[kScReadFull], totals.n[kScWriteFull],
                   totals.n[kScProtect], totals.n[kScFlush], totals.n[kScSysQuery],
                   totals.n[kScProcQuery], totals.n[kScMemQuery], totals.n[kScLinkOpen],
                   totals.n[kScLinkQuery]);
        LustedLogFlush();
    }
}

bool Memory::CustomWrite(uintptr_t address, const void* data, size_t size) {
    return CustomWriteImpl(address, data, size, true, true);
}

bool Memory::CustomWriteFast(uintptr_t address, const void* data, size_t size) {
    return CustomWriteImpl(address, data, size, false, false);
}

bool Memory::CustomWriteImpl(uintptr_t address, const void* data, size_t size,
                             bool verify, bool logSuccess) {
    if (!IsOpen() || !address || !data || !size) return false;
    if (!SysEnsureResolved()) return false;

    // Phase timers: anything over a millisecond is logged with its breakdown so
    // the slow phase is named instead of guessed at.
    const bool measure = verify || logSuccess;
    LARGE_INTEGER fr{}, ta{}, tb{}, tc{}, td{};
    if (measure) QueryPerformanceFrequency(&fr);
    const auto us = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)fr.QuadPart;
    };
    if (measure) QueryPerformanceCounter(&ta);

    // Write rights are scoped to this call: open, use, drop.
    SysObjectAttributes oa{};
    oa.Length = sizeof(oa);
    SysClientId cid{};
    cid.UniqueProcess = (HANDLE)(ULONG_PTR)pid_;
    HANDLE hw = nullptr;
    SysCount(kScOpen);
    if (!SYS_OK(ProcOpen(&hw, PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                                         &oa, &cid)) ||
        !hw) {
        LustedLogf("write OPENFAIL addr=0x%llx size=%zu", (unsigned long long)address, size);
        return false;
    }

    // No region query here: against a live target it measured 1.3-26 ms per
    // call, which dwarfs the write itself. The hand-written MemWriteFull loop
    // is tried first and the protect/restore dance only runs when it comes up
    // short - that is also exactly when the page is a non-writable (code)
    // page, so the cache flush below lands where it belongs.
    const uint8_t* src = (const uint8_t*)data;
    auto writeRange = [&](size_t from) -> SIZE_T {
        SIZE_T moved = 0;
        SysCount(kScWriteFull);
        MemWriteFull(hw, (PVOID)(address + from), (PVOID)(src + from),
                     size - from, &moved);
        return moved;
    };

    SIZE_T done = writeRange(0);
    bool didProtect = false;
    bool protectionRestored = true;
    if (done != size) {
        PVOID base = (PVOID)address;
        SIZE_T region = size;
        ULONG oldProt = 0;
        SysCount(kScProtect);
        if (SYS_OK(MemProtect(hw, &base, &region,
                                                     PAGE_EXECUTE_READWRITE, &oldProt))) {
            done += writeRange(done);
            PVOID rb = (PVOID)address; SIZE_T rs = size; ULONG tmp = 0;
            SysCount(kScProtect);
            protectionRestored = SYS_OK(MemProtect(
                hw, &rb, &rs, oldProt ? oldProt : PAGE_EXECUTE_READ, &tmp));
            SysCount(kScFlush);
            CacheFlush(hw, (PVOID)address, size);
            didProtect = true;
        }
    }
    SysCount(kScClose);
    HandleClose(hw);
    if (measure) QueryPerformanceCounter(&tb);

    if (!protectionRestored) {
        LustedLogf("write PROTECTRESTOREFAIL addr=0x%llx size=%zu",
                   (unsigned long long)address, size);
        return false;
    }
    if (done != size) {
        LustedLogf("write SHORT addr=0x%llx want=%zu got=%zu",
                   (unsigned long long)address, size, done);
        return false;
    }
    if (!verify) return true;
    // Verify by reading back through the persistent read handle.
    std::vector<uint8_t> back(size);
    const bool verified = CustomRead(address, back.data(), size) &&
                          memcmp(back.data(), data, size) == 0;
    QueryPerformanceCounter(&tc);

    char line[256];
    snprintf(line, sizeof(line), "write addr=0x%llx size=%zu verify=%s mode=%s",
             (unsigned long long)address, size, verified ? "ok" : "MISMATCH",
             didProtect ? "protect" : "direct");
    if (logSuccess || !verified) LustedLog(line);
    QueryPerformanceCounter(&td);
    const double wallUs = us(ta, td);
    if (wallUs > 1000.0)
        LustedLogf("SLOW write addr=0x%llx core=%.0fus (write=%.0f verify=%.0f) log=%.0f",
                   (unsigned long long)address, us(ta, tb), us(tb, tc), us(tc, td));
    return verified;
}

bool Memory::CustomRead(uintptr_t address, void* out, size_t size) {
    if (!IsOpen() || !address || !out || !size) return false;
    if (!SysEnsureResolved()) return false;
    SIZE_T r = 0;
    SysCount(kScReadFull);
    const SysStatus st = MemReadFull(hProc_, (PVOID)address, out, size, &r);
    return SYS_OK(st) && r == size;
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
