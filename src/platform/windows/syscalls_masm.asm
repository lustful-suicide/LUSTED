; Raw-syscall stubs for ML64 (MSVC).
; Each stub moves the first arg from RCX to R10 (syscall convention),
; loads its SSN from the C++-owned slot, and traps to the kernel.
; The `syscall` instruction lives in our own image: no ntdll code runs.

.code

EXTERN g_SysProcOpen:DWORD
EXTERN g_SysHandleClose:DWORD
EXTERN g_SysMemRead:DWORD
EXTERN g_SysMemWrite:DWORD
EXTERN g_SysMemProtect:DWORD
EXTERN g_SysCacheFlush:DWORD
EXTERN g_SysQuery:DWORD
EXTERN g_SysProcQuery:DWORD
EXTERN g_SysMemQuery:DWORD
EXTERN g_SysLinkOpen:DWORD
EXTERN g_SysLinkQuery:DWORD

ProcOpen PROC
    mov r10, rcx
    mov eax, g_SysProcOpen
    syscall
    ret
ProcOpen ENDP

HandleClose PROC
    mov r10, rcx
    mov eax, g_SysHandleClose
    syscall
    ret
HandleClose ENDP

MemRead PROC
    mov r10, rcx
    mov eax, g_SysMemRead
    syscall
    ret
MemRead ENDP

MemWrite PROC
    mov r10, rcx
    mov eax, g_SysMemWrite
    syscall
    ret
MemWrite ENDP

MemProtect PROC
    mov r10, rcx
    mov eax, g_SysMemProtect
    syscall
    ret
MemProtect ENDP

CacheFlush PROC
    mov r10, rcx
    mov eax, g_SysCacheFlush
    syscall
    ret
CacheFlush ENDP

SysQuery PROC
    mov r10, rcx
    mov eax, g_SysQuery
    syscall
    ret
SysQuery ENDP

ProcQuery PROC
    mov r10, rcx
    mov eax, g_SysProcQuery
    syscall
    ret
ProcQuery ENDP

MemQuery PROC
    mov r10, rcx
    mov eax, g_SysMemQuery
    syscall
    ret
MemQuery ENDP

LinkOpen PROC
    mov r10, rcx
    mov eax, g_SysLinkOpen
    syscall
    ret
LinkOpen ENDP

LinkQuery PROC
    mov r10, rcx
    mov eax, g_SysLinkQuery
    syscall
    ret
LinkQuery ENDP

; Custom full transfers. Args: RCX=h, RDX=addr, R8=buf, R9=len, [rsp+28h]=done*.
; Frame (sub rsp,60h): [rsp+20h]=Written temp, [rsp+28h]=trap arg5 slot
; (&Written), [rsp+30h..58h]=h,addr,buf,len,done*,total. Only volatile regs
; used (R11 holds totals between traps only - syscall clobbers it, so total
; lives on the stack).
MemWriteFull PROC
    mov rax, QWORD PTR [rsp + 28h]
    sub rsp, 60h
    mov QWORD PTR [rsp + 30h], rcx
    mov QWORD PTR [rsp + 38h], rdx
    mov QWORD PTR [rsp + 40h], r8
    mov QWORD PTR [rsp + 48h], r9
    mov QWORD PTR [rsp + 50h], rax
    mov QWORD PTR [rsp + 58h], 0
    lea rax, [rsp + 20h]
    mov QWORD PTR [rsp + 28h], rax
    xor eax, eax
WrLoop:
    mov r11, QWORD PTR [rsp + 58h]
    mov r9, QWORD PTR [rsp + 48h]
    sub r9, r11
    jz WrDone
    mov rcx, QWORD PTR [rsp + 30h]
    mov rdx, QWORD PTR [rsp + 38h]
    add rdx, r11
    mov r8, QWORD PTR [rsp + 40h]
    add r8, r11
    mov r10, rcx
    mov eax, g_SysMemWrite
    syscall
    test eax, eax
    js WrFinish
    mov r11, QWORD PTR [rsp + 20h]
    test r11, r11
    jz WrFinish
    add QWORD PTR [rsp + 58h], r11
    jmp WrLoop
WrDone:
    xor eax, eax
WrFinish:
    mov r11, QWORD PTR [rsp + 58h]
    mov rcx, QWORD PTR [rsp + 50h]
    test rcx, rcx
    jz WrRet
    mov QWORD PTR [rcx], r11
WrRet:
    add rsp, 60h
    ret
MemWriteFull ENDP

MemReadFull PROC
    mov rax, QWORD PTR [rsp + 28h]
    sub rsp, 60h
    mov QWORD PTR [rsp + 30h], rcx
    mov QWORD PTR [rsp + 38h], rdx
    mov QWORD PTR [rsp + 40h], r8
    mov QWORD PTR [rsp + 48h], r9
    mov QWORD PTR [rsp + 50h], rax
    mov QWORD PTR [rsp + 58h], 0
    lea rax, [rsp + 20h]
    mov QWORD PTR [rsp + 28h], rax
    xor eax, eax
RdLoop:
    mov r11, QWORD PTR [rsp + 58h]
    mov r9, QWORD PTR [rsp + 48h]
    sub r9, r11
    jz RdDone
    mov rcx, QWORD PTR [rsp + 30h]
    mov rdx, QWORD PTR [rsp + 38h]
    add rdx, r11
    mov r8, QWORD PTR [rsp + 40h]
    add r8, r11
    mov r10, rcx
    mov eax, g_SysMemRead
    syscall
    test eax, eax
    js RdFinish
    mov r11, QWORD PTR [rsp + 20h]
    test r11, r11
    jz RdFinish
    add QWORD PTR [rsp + 58h], r11
    jmp RdLoop
RdDone:
    xor eax, eax
RdFinish:
    mov r11, QWORD PTR [rsp + 58h]
    mov rcx, QWORD PTR [rsp + 50h]
    test rcx, rcx
    jz RdRet
    mov QWORD PTR [rcx], r11
RdRet:
    add rsp, 60h
    ret
MemReadFull ENDP

END
