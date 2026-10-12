// Raw-syscall stubs for GNU as (MinGW GCC / Clang).
// Each stub moves the first arg from RCX to R10 (syscall convention),
// loads its SSN from the C++-owned slot, and traps to the kernel.
// The `syscall` instruction lives in our own image: no ntdll code runs.
// R10/R11 volatility matches the Win64 ABI (both volatile), and 5th+
// arguments stay on the stack where the kernel expects them.
.intel_syntax noprefix
.text

.globl ProcOpen
ProcOpen:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysProcOpen]
    syscall
    ret

.globl HandleClose
HandleClose:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysHandleClose]
    syscall
    ret

.globl MemRead
MemRead:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemRead]
    syscall
    ret

.globl MemWrite
MemWrite:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemWrite]
    syscall
    ret

.globl MemProtect
MemProtect:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemProtect]
    syscall
    ret

.globl CacheFlush
CacheFlush:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysCacheFlush]
    syscall
    ret

.globl SysQuery
SysQuery:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysQuery]
    syscall
    ret

.globl ProcQuery
ProcQuery:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysProcQuery]
    syscall
    ret

.globl MemQuery
MemQuery:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemQuery]
    syscall
    ret

.globl LinkOpen
LinkOpen:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysLinkOpen]
    syscall
    ret

.globl LinkQuery
LinkQuery:
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysLinkQuery]
    syscall
    ret

// Custom full transfers. Args: RCX=h, RDX=addr, R8=buf, R9=len, [rsp+0x28]=done*.
// Frame (sub rsp,0x60): [rsp+0x20]=Written temp, [rsp+0x28]=trap arg5 slot
// (&Written), [rsp+0x30..0x58]=h,addr,buf,len,done*,total. Only volatile regs
// used (R11 holds totals between traps only - syscall clobbers it, so total
// lives on the stack).
.globl MemWriteFull
MemWriteFull:
    mov rax, QWORD PTR [rsp + 0x28]
    sub rsp, 0x60
    mov QWORD PTR [rsp + 0x30], rcx
    mov QWORD PTR [rsp + 0x38], rdx
    mov QWORD PTR [rsp + 0x40], r8
    mov QWORD PTR [rsp + 0x48], r9
    mov QWORD PTR [rsp + 0x50], rax
    mov QWORD PTR [rsp + 0x58], 0
    lea rax, [rsp + 0x20]
    mov QWORD PTR [rsp + 0x28], rax
    xor eax, eax
.LwLoop:
    mov r11, QWORD PTR [rsp + 0x58]
    mov r9, QWORD PTR [rsp + 0x48]
    sub r9, r11
    jz .LwDone
    mov rcx, QWORD PTR [rsp + 0x30]
    mov rdx, QWORD PTR [rsp + 0x38]
    add rdx, r11
    mov r8, QWORD PTR [rsp + 0x40]
    add r8, r11
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemWrite]
    syscall
    test eax, eax
    js .LwFinish
    mov r11, QWORD PTR [rsp + 0x20]
    test r11, r11
    jz .LwFinish
    add QWORD PTR [rsp + 0x58], r11
    jmp .LwLoop
.LwDone:
    xor eax, eax
.LwFinish:
    mov r11, QWORD PTR [rsp + 0x58]
    mov rcx, QWORD PTR [rsp + 0x50]
    test rcx, rcx
    jz .LwRet
    mov QWORD PTR [rcx], r11
.LwRet:
    add rsp, 0x60
    ret

.globl MemReadFull
MemReadFull:
    mov rax, QWORD PTR [rsp + 0x28]
    sub rsp, 0x60
    mov QWORD PTR [rsp + 0x30], rcx
    mov QWORD PTR [rsp + 0x38], rdx
    mov QWORD PTR [rsp + 0x40], r8
    mov QWORD PTR [rsp + 0x48], r9
    mov QWORD PTR [rsp + 0x50], rax
    mov QWORD PTR [rsp + 0x58], 0
    lea rax, [rsp + 0x20]
    mov QWORD PTR [rsp + 0x28], rax
    xor eax, eax
.LrLoop:
    mov r11, QWORD PTR [rsp + 0x58]
    mov r9, QWORD PTR [rsp + 0x48]
    sub r9, r11
    jz .LrDone
    mov rcx, QWORD PTR [rsp + 0x30]
    mov rdx, QWORD PTR [rsp + 0x38]
    add rdx, r11
    mov r8, QWORD PTR [rsp + 0x40]
    add r8, r11
    mov r10, rcx
    mov eax, DWORD PTR [rip + g_SysMemRead]
    syscall
    test eax, eax
    js .LrFinish
    mov r11, QWORD PTR [rsp + 0x20]
    test r11, r11
    jz .LrFinish
    add QWORD PTR [rsp + 0x58], r11
    jmp .LrLoop
.LrDone:
    xor eax, eax
.LrFinish:
    mov r11, QWORD PTR [rsp + 0x58]
    mov rcx, QWORD PTR [rsp + 0x50]
    test rcx, rcx
    jz .LrRet
    mov QWORD PTR [rcx], r11
.LrRet:
    add rsp, 0x60
    ret
