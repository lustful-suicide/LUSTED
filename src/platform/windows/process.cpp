#include "platform/windows/process.h"
#include "platform/windows/syscalls.h"
#include <intrin.h> // __readgsqword
#include <cwchar>
#include <mutex>

namespace {

// Single-syscall divers, kept out of the interface.
constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessImageFileName = 27;
constexpr ULONG kMemoryBasicInformation = 0;
constexpr ULONG kSymbolicLinkQuery = 0x0001;
constexpr SysStatus kInfoLengthMismatch = (SysStatus)0xC0000004;

// SYSTEM_PROCESS_INFORMATION, x64 field offsets (stable since Vista).
constexpr size_t kNextEntryOffset = 0x00;
constexpr size_t kImageNameOffset = 0x38; // UNICODE_STRING: Length@+0, Buffer@+8
constexpr size_t kUniquePidOffset = 0x50; // HANDLE (ULONG_PTR)

// PROCESS_BASIC_INFORMATION: PEB address at +0x08.
constexpr size_t kPebOffset = 0x08;

// Remote _LDR_DATA_TABLE_ENTRY, x64 (same shape the local PEB walk uses).
// Walked via InLoadOrderModuleList: head at Ldr+0x10, node at entry+0x00.
constexpr size_t kLdrDllBase = 0x30;
constexpr size_t kLdrSizeOfImage = 0x40;
constexpr size_t kLdrBaseName = 0x58; // UNICODE_STRING
constexpr size_t kLdrLoadOrderHead = 0x10;
constexpr size_t kPebLdrOffset = 0x18;
constexpr size_t kPebImageBaseOffset = 0x10;

// Device paths are ASCII in practice; narrow by hand, no conversion wrapper.
std::string Narrow(const wchar_t* w, size_t chars) {
    std::string s;
    s.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        const wchar_t c = w[i];
        if (!c) break;
        s.push_back(c < 0x80 ? (char)c : '?');
    }
    return s;
}

bool EqualsInsensitive(const std::string& a, const char* b) {
    size_t n = 0;
    for (; b[n]; ++n) {
    }
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

const char* FilePart(const char* path) {
    const char* slash = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    return slash;
}

bool ReadRemote(HANDLE h, uintptr_t addr, void* out, size_t size) {
    SIZE_T n = 0;
    SysCount(kScRead);
    return SYS_OK(
               MemRead(h, (PVOID)addr, out, size, &n)) &&
           n == size;
}

} // namespace

bool ProcListAll(std::vector<ProcEntry>& out) {
    out.clear();
    if (!SysEnsureResolved()) return false;
    std::vector<uint8_t> buf(1 << 20);
    ULONG retLen = 0;
    for (;;) {
        SysCount(kScSysQuery);
        const SysStatus st = SysQuery(kSystemProcessInformation, buf.data(),
                                                           (ULONG)buf.size(), &retLen);
        if (SYS_OK(st)) break;
        if (st != kInfoLengthMismatch || buf.size() >= (1 << 26)) return false;
        buf.resize(buf.size() * 2);
    }
    for (size_t off = 0;;) {
        const uint8_t* e = buf.data() + off;
        const ULONG next = *(const ULONG*)(e + kNextEntryOffset);
        const DWORD pid = (DWORD)(uintptr_t) * (HANDLE*)(e + kUniquePidOffset);
        ProcEntry entry;
        entry.pid = pid;
        const USHORT nameLen = *(const USHORT*)(e + kImageNameOffset);
        const wchar_t* nameBuf = *(wchar_t* const*)(e + kImageNameOffset + 8);
        if (nameLen && nameBuf)
            entry.imageNt = Narrow(nameBuf, nameLen / sizeof(wchar_t));
        out.push_back(entry);
        if (!next) break;
        off += next;
    }
    return true;
}

namespace {

// \??\C: -> \Device\HarddiskVolumeN map, resolved once via the object manager.
std::once_flag g_drivesOnce;
char g_driveLetter[26] = {};
std::string g_driveTarget[26];

void ResolveDrives() {
    for (int i = 0; i < 26; ++i) {
        wchar_t link[] = L"\\??\\X:";
        link[4] = (wchar_t)('A' + i);
        SysUnicodeString name{};
        name.Length = (USHORT)(wcslen(link) * sizeof(wchar_t));
        name.MaximumLength = (USHORT)sizeof(link);
        name.Buffer = link;
        SysObjectAttributes oa{};
        oa.Length = sizeof(oa);
        oa.ObjectName = &name;
        HANDLE h = nullptr;
        SysCount(kScLinkOpen);
        if (!SYS_OK(LinkOpen(&h, kSymbolicLinkQuery, &oa)) || !h)
            continue;
        wchar_t target[256] = {};
        SysUnicodeString out{};
        out.Length = 0;
        out.MaximumLength = sizeof(target);
        out.Buffer = target;
        ULONG retLen = 0;
        SysCount(kScLinkQuery);
        const bool ok = SYS_OK(LinkQuery(h, &out, &retLen));
        SysCount(kScClose);
        HandleClose(h);
        if (ok && out.Length) {
            g_driveLetter[i] = (char)('A' + i);
            g_driveTarget[i] = Narrow(target, out.Length / sizeof(wchar_t));
        }
    }
}

} // namespace

bool ProcImagePath(HANDLE hProc, std::string& win32Path) {
    win32Path.clear();
    if (!hProc || !SysEnsureResolved()) return false;
    std::call_once(g_drivesOnce, ResolveDrives);
    uint8_t buf[sizeof(SysUnicodeString) + 512 * sizeof(wchar_t)] = {};
    ULONG retLen = 0;
    SysCount(kScProcQuery);
    if (!SYS_OK(ProcQuery(hProc, kProcessImageFileName, buf,
                                                     (ULONG)sizeof(buf), &retLen)))
        return false;
    const SysUnicodeString* name = (const SysUnicodeString*)buf;
    if (!name->Length || !name->Buffer) return false;
    const std::string nt = Narrow(name->Buffer, name->Length / sizeof(wchar_t));
    // Longest-prefix wins so nested mounts resolve correctly.
    size_t best = 0;
    int bestDrive = -1;
    for (int i = 0; i < 26; ++i) {
        if (!g_driveLetter[i] || g_driveTarget[i].empty()) continue;
        const std::string& t = g_driveTarget[i];
        if (nt.size() > t.size() && nt.compare(0, t.size(), t) == 0 && nt[t.size()] == '\\' &&
            t.size() > best) {
            best = t.size();
            bestDrive = i;
        }
    }
    if (bestDrive < 0) {
        win32Path = nt; // no mapping; device path is still identifying
        return true;
    }
    win32Path.reserve(3 + nt.size() - best);
    win32Path.push_back(g_driveLetter[bestDrive]);
    win32Path.push_back(':');
    win32Path.append(nt.substr(best));
    return true;
}

bool ModuleLocate(HANDLE hProc, const char* moduleName, uintptr_t& base, size_t& size) {
    base = 0;
    size = 0;
    if (!hProc || !moduleName || !SysEnsureResolved()) return false;
    uint8_t pbi[6 * 8] = {};
    ULONG retLen = 0;
    SysCount(kScProcQuery);
    if (!SYS_OK(ProcQuery(hProc, kProcessBasicInformation, pbi,
                                                     (ULONG)sizeof(pbi), &retLen)))
        return false;
    const uintptr_t peb = (uintptr_t) * (void**)(pbi + kPebOffset);
    if (!peb) return false;
    uintptr_t ldr = 0;
    if (!ReadRemote(hProc, peb + kPebLdrOffset, &ldr, sizeof(ldr)) || !ldr) return false;
    const uintptr_t head = ldr + kLdrLoadOrderHead;
    uintptr_t cur = 0;
    if (!ReadRemote(hProc, head, &cur, sizeof(cur))) return false;
    for (int hops = 0; cur && cur != head && hops < 1024; ++hops) {
        const uintptr_t rec = cur;
        uintptr_t dllBase = 0;
        ULONG imageSize = 0;
        SysUnicodeString name{};
        if (!ReadRemote(hProc, rec + kLdrDllBase, &dllBase, sizeof(dllBase)) ||
            !ReadRemote(hProc, rec + kLdrSizeOfImage, &imageSize, sizeof(imageSize)) ||
            !ReadRemote(hProc, rec + kLdrBaseName, &name, sizeof(name)))
            return false;
        uintptr_t next = 0;
        if (!ReadRemote(hProc, cur, &next, sizeof(next))) return false;
        if (name.Length && name.Buffer) {
            const size_t chars = name.Length / sizeof(wchar_t);
            if (chars < 128) {
                wchar_t wname[128] = {};
                SIZE_T n = 0;
                SysCount(kScRead);
                if (SYS_OK(MemRead(hProc, name.Buffer, wname,
                                                          chars * sizeof(wchar_t), &n)) &&
                    n == chars * sizeof(wchar_t)) {
                    if (EqualsInsensitive(Narrow(wname, chars), moduleName)) {
                        base = dllBase;
                        size = imageSize;
                        return true;
                    }
                }
            }
        }
        cur = next;
    }
    return false;
}

bool MemDescribe(HANDLE hProc, uintptr_t addr, MEMORY_BASIC_INFORMATION& mbi) {
    if (!hProc || !SysEnsureResolved()) return false;
    SIZE_T retLen = 0;
    SysCount(kScMemQuery);
    return SYS_OK(MemQuery(hProc, (PVOID)addr, kMemoryBasicInformation,
                                                  &mbi, sizeof(mbi), &retLen)) &&
           retLen == sizeof(mbi);
}

void AddrRange(uintptr_t& lo, uintptr_t& hi) {
    lo = 0x10000; // minimum user allocation, x64
    hi = 0x00007FFFFFFEFFFFULL; // documented top of user space, x64
    if (hi > 0x0000800000000000ULL) hi = 0x0000800000000000ULL; // canonical half
}

HMODULE CurrentModule() {
#if defined(_M_X64) || defined(__x86_64__)
    const uint64_t peb = __readgsqword(0x60);
    if (peb) {
        HMODULE base = *(HMODULE*)(peb + kPebImageBaseOffset);
        if (base) return base;
    }
#endif
    return GetModuleHandleA(nullptr);
}
