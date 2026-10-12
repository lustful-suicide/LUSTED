#include "platform/windows/syscalls.h"
#include <intrin.h> // __readgsqword
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
uint32_t g_SysProcOpen = 0;
uint32_t g_SysHandleClose = 0;
uint32_t g_SysMemRead = 0;
uint32_t g_SysMemWrite = 0;
uint32_t g_SysMemProtect = 0;
uint32_t g_SysCacheFlush = 0;
uint32_t g_SysQuery = 0;
uint32_t g_SysProcQuery = 0;
uint32_t g_SysMemQuery = 0;
uint32_t g_SysLinkOpen = 0;
uint32_t g_SysLinkQuery = 0;
}

namespace {

// ntdll base via PEB walk. gs:[0x60] -> PEB, PEB+0x18 -> Ldr,
// Ldr+0x10 -> InLoadOrderModuleList head; the node sits at record+0x00.
// DllBase is at record+0x30, BaseDllName (UNICODE_STRING) at record+0x58.
// Falls back to GetModuleHandleA
// (a hook-free PEB read inside kernel32, no syscall) if the walk fails.
uint8_t* FindNtdllBase() {
#if defined(_M_X64) || defined(__x86_64__)
    const uint64_t peb = __readgsqword(0x60);
    if (peb) {
        const uint64_t ldr = *(volatile const uint64_t*)(peb + 0x18);
        if (ldr) {
            const uint64_t head = ldr + 0x10;
            uint64_t cur = *(volatile const uint64_t*)head;
            for (int hops = 0; cur && cur != head && hops < 256; ++hops) {
                const uint64_t rec = cur;
                const uint16_t nameLen = *(volatile const uint16_t*)(rec + 0x58);
                const wchar_t* name = *(wchar_t* volatile const*)(rec + 0x58 + 8);
                void* dllBase = *(void* volatile const*)(rec + 0x30);
                const uint64_t next = *(volatile const uint64_t*)cur;
                if (nameLen == 18 && dllBase && name &&
                    _wcsnicmp(name, L"ntdll.dll", 9) == 0)
                    return (uint8_t*)dllBase;
                cur = next;
            }
        }
    }
#endif
    return (uint8_t*)GetModuleHandleA("ntdll.dll");
}

// Clean x64 ntdll stub starts: mov r10,rcx ; mov eax,SSN.
// Hooked stubs start with jmp/call/int3 instead, which is what Halo's Gate
// below routes around.
bool CleanStub(const uint8_t* p, uint32_t& ssn) {
    if (!p) return false;
    if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8 &&
        p[6] == 0x00 && p[7] == 0x00) {
        uint32_t v = 0;
        memcpy(&v, p + 4, sizeof(v));
        if (v < 0x1000) {
            ssn = v;
            return true;
        }
    }
    return false;
}

struct ZwExport {
    std::string name; // "Zw..." form; unique per stub
    uint8_t* func;
};

// Parses ntdll's export table by hand and returns every Zw* export sorted by
// function address. Only Zw names are kept: Nt* entries alias the same RVAs
// and would duplicate addresses, breaking the Halo's Gate index math.
bool CollectZwExports(uint8_t* base, std::vector<ZwExport>& out) {
    if (!base || base[0] != 'M' || base[1] != 'Z') return false;
    const uint32_t e_lfanew = *(const uint32_t*)(base + 0x3C);
    uint8_t* nt = base + e_lfanew;
    if (*(const uint32_t*)nt != 0x00004550) return false; // "PE\0\0"
    const uint16_t magic = *(const uint16_t*)(nt + 0x18);
    if (magic != 0x20B) return false; // PE32+ expected
    const uint32_t expRva = *(const uint32_t*)(nt + 0x18 + 0x70 + 0 * 8);
    const uint32_t expSize = *(const uint32_t*)(nt + 0x18 + 0x70 + 0 * 8 + 4);
    if (!expRva || !expSize) return false;
    uint8_t* exp = base + expRva;
    const uint32_t numNames = *(const uint32_t*)(exp + 0x18);
    const uint32_t addrNames = *(const uint32_t*)(exp + 0x20);
    const uint32_t addrOrdinals = *(const uint32_t*)(exp + 0x24);
    const uint32_t addrFuncs = *(const uint32_t*)(exp + 0x1C);
    if (!numNames || !addrNames || !addrOrdinals || !addrFuncs) return false;

    out.reserve(512);
    for (uint32_t i = 0; i < numNames; ++i) {
        const char* name = (const char*)(base + *(const uint32_t*)(base + addrNames + i * 4));
        if (name[0] != 'Z' || name[1] != 'w') continue;
        const uint16_t ord = *(const uint16_t*)(base + addrOrdinals + i * 2);
        const uint32_t rva = *(const uint32_t*)(base + addrFuncs + (uint32_t)ord * 4);
        if (!rva) continue;
        out.push_back({name, base + rva});
    }
    if (out.empty()) return false;
    std::sort(out.begin(), out.end(),
              [](const ZwExport& a, const ZwExport& b) { return a.func < b.func; });
    return true;
}

// SSNs are sequential with stub address order in ntdll, so a hooked stub's
// number is recovered from the nearest clean neighbour:
//   ssn(target) = ssn(neighbour) + (index(target) - index(neighbour)).
bool ResolveOne(const std::vector<ZwExport>& sorted, const char* ntName, uint32_t& ssn) {
    std::string zw = std::string("Zw") + (ntName + 2);
    size_t idx = sorted.size();
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i].name == zw) {
            idx = i;
            break;
        }
    }
    if (idx == sorted.size()) return false;
    if (CleanStub(sorted[idx].func, ssn)) return true;
    for (size_t dist = 1; dist <= 40; ++dist) {
        for (int dir = -1; dir <= 1; dir += 2) {
            const long j = (long)idx + dir * (long)dist;
            if (j < 0 || (size_t)j >= sorted.size()) continue;
            uint32_t n = 0;
            if (CleanStub(sorted[(size_t)j].func, n)) {
                ssn = n + (uint32_t)((long)idx - j);
                return true;
            }
        }
    }
    return false;
}

bool ResolveAll() {
    uint8_t* base = FindNtdllBase();
    if (!base) return false;
    std::vector<ZwExport> sorted;
    if (!CollectZwExports(base, sorted)) return false;
    // Wants table: left column holds ntdll's export names. Those strings are
    // lookup keys only - the numbers are read out of the export table and the
    // traps run from our own asm. No NT code ever executes.
    const struct Want {
        const char* nt;
        uint32_t* slot;
    } wants[] = {
        {"NtOpenProcess", &g_SysProcOpen},
        {"NtClose", &g_SysHandleClose},
        {"NtReadVirtualMemory", &g_SysMemRead},
        {"NtWriteVirtualMemory", &g_SysMemWrite},
        {"NtProtectVirtualMemory", &g_SysMemProtect},
        {"NtFlushInstructionCache", &g_SysCacheFlush},
        {"NtQuerySystemInformation", &g_SysQuery},
        {"NtQueryInformationProcess", &g_SysProcQuery},
        {"NtQueryVirtualMemory", &g_SysMemQuery},
        {"NtOpenSymbolicLinkObject", &g_SysLinkOpen},
        {"NtQuerySymbolicLinkObject", &g_SysLinkQuery},
    };
    for (const Want& w : wants) {
        uint32_t ssn = 0;
        if (!ResolveOne(sorted, w.nt, ssn) || !ssn) return false;
        *w.slot = ssn;
    }
    return true;
}

std::once_flag g_resolveOnce;
bool g_resolved = false;

std::atomic<uint64_t> g_counts[kScCount];

} // namespace

void SysCount(SysCallId id) {
    if ((int)id >= 0 && (int)id < kScCount)
        g_counts[(int)id].fetch_add(1, std::memory_order_relaxed);
}

void SysCallTotalsSnapshot(SysCallTotals& out) {
    for (int i = 0; i < kScCount; ++i)
        out.n[i] = g_counts[i].load(std::memory_order_relaxed);
}

bool SysEnsureResolved() {
    std::call_once(g_resolveOnce, [] { g_resolved = ResolveAll(); });
    return g_resolved;
}
