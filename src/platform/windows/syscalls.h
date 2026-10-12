#pragma once
// Direct-syscall layer.
//
// The `syscall` instructions live in our own image (syscalls_gas.S for
// GCC/Clang, syscalls_masm.asm for MSVC). No call into ntdll ever executes,
// so user-mode hooks/detours placed on Nt* exports are never touched. The
// syscall numbers (SSNs) change between Windows builds, so they are resolved
// at runtime from the in-memory ntdll export table (Halo's Gate tolerant)
// by SysEnsureResolved().
//
// x64 only. The `syscall` instruction does not exist in 32-bit mode and the
// SSN table parsed here is the 64-bit one.
#if !defined(_M_X64) && !defined(__x86_64__)
#error "Direct syscalls are implemented for x64 only."
#endif

#include <windows.h>
#include <cstdint>

typedef LONG SysStatus;
#define SYS_OK(s) ((SysStatus)(s) >= 0)

// Trap census: every layer entry point below is counted (relaxed atomics,
// ~1ns against a ~4us trap). One Detach log line reports the session totals,
// so a retry storm or a hot caller shows up as a number instead of a feeling.
enum SysCallId {
    kScOpen = 0,
    kScClose,
    kScRead,
    kScWrite,
    kScReadFull,
    kScWriteFull,
    kScProtect,
    kScFlush,
    kScSysQuery,
    kScProcQuery,
    kScMemQuery,
    kScLinkOpen,
    kScLinkQuery,
    kScCount
};
void SysCount(SysCallId id);

struct SysCallTotals {
    uint64_t n[kScCount];
};
void SysCallTotalsSnapshot(SysCallTotals& out);

// Minimal UNICODE_STRING for object-manager calls.
struct SysUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t* Buffer;
};
static_assert(sizeof(SysUnicodeString) == 16, "UNICODE_STRING layout");

// Minimal OBJECT_ATTRIBUTES for ProcOpen. We always pass a null object
// name, so the name field is an untyped pointer; layout matches the native shape
// (Length=48 on x64).
struct SysObjectAttributes {
    ULONG Length;
    uint8_t _pad0[4];
    HANDLE RootDirectory;
    SysUnicodeString* ObjectName;
    ULONG Attributes;
    uint8_t _pad1[4];
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};
static_assert(sizeof(SysObjectAttributes) == 48, "OBJECT_ATTRIBUTES layout");

// Minimal CLIENT_ID for ProcOpen.
struct SysClientId {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
};

extern "C" {
// SSN slots, filled by SysEnsureResolved(), read by the asm stubs.
extern uint32_t g_SysProcOpen;
extern uint32_t g_SysHandleClose;
extern uint32_t g_SysMemRead;
extern uint32_t g_SysMemWrite;
extern uint32_t g_SysMemProtect;
extern uint32_t g_SysCacheFlush;
extern uint32_t g_SysQuery;
extern uint32_t g_SysProcQuery;
extern uint32_t g_SysMemQuery;
extern uint32_t g_SysLinkOpen;
extern uint32_t g_SysLinkQuery;

// One raw-syscall stub each. Our own names and our own code: first args in
// RCX/RDX/R8/R9, remainder on the stack, status in EAX. (Resolving the
// numbers still keys off ntdll's export names - that table is the only place
// the numbers live - but no NT code ever executes.)
SysStatus ProcOpen(HANDLE* ProcessHandle, ACCESS_MASK DesiredAccess,
                             SysObjectAttributes* ObjectAttributes, SysClientId* ClientId);
SysStatus HandleClose(HANDLE Handle);
SysStatus MemRead(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                                   SIZE_T BytesToRead, SIZE_T* BytesRead);
SysStatus MemWrite(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                                    SIZE_T BytesToWrite, SIZE_T* BytesWritten);
SysStatus MemProtect(HANDLE ProcessHandle, PVOID* BaseAddress,
                                      SIZE_T* RegionSize, ULONG NewProtect, ULONG* OldProtect);
SysStatus CacheFlush(HANDLE ProcessHandle, PVOID BaseAddress, SIZE_T Length);
SysStatus SysQuery(ULONG SystemInformationClass, PVOID SystemInformation,
                                        ULONG SystemInformationLength, ULONG* ReturnLength);
SysStatus ProcQuery(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                         PVOID ProcessInformation, ULONG ProcessInformationLength,
                                         ULONG* ReturnLength);
SysStatus MemQuery(HANDLE ProcessHandle, PVOID BaseAddress,
                                    ULONG MemoryInformationClass, PVOID MemoryInformation,
                                    SIZE_T MemoryInformationLength, SIZE_T* ReturnLength);
SysStatus LinkOpen(HANDLE* LinkHandle, ACCESS_MASK DesiredAccess,
                                        SysObjectAttributes* ObjectAttributes);
SysStatus LinkQuery(HANDLE LinkHandle, SysUnicodeString* LinkTarget,
                                         ULONG* ReturnedLength);

// Hand-written full transfers. The retry loop lives in asm: trap, advance by
// what moved, repeat until Length is done, progress stops, or an error
// status returns. *Done always reports bytes moved. Returns SUCCESS (0) when
// Done == Length, else the failing status (or SUCCESS with a short Done when
// the kernel stopped without error).
SysStatus MemWriteFull(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                                            SIZE_T Length, SIZE_T* Done);
SysStatus MemReadFull(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer,
                                           SIZE_T Length, SIZE_T* Done);
} // extern "C"

// Resolves every SSN once (thread-safe). No WinAPI/Nt call is used for the
// lookup itself: ntdll's base comes from a PEB walk and its exports are
// parsed by hand. Returns false when ntdll cannot be parsed.
bool SysEnsureResolved();
