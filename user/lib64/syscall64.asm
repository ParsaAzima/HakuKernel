; Haku x86-64 raw system call stubs.
;
; The C prototypes take (number, a1..a6). The Windows x64 C compiler passes
; those in RCX/RDX/R8/R9 and on the stack, while the Haku SYSCALL entry wants
; the number in RAX and the arguments in RDI/RSI/RDX/R10/R8/R9. Each stub
; rearranges its incoming registers and executes SYSCALL. RCX and R11 are
; clobbered by the instruction, which the C caller already treats as volatile.
;
; The 7th argument of haku64_syscall6 (a6) is the first stack argument, at
; [rsp+8] on entry because the return address occupies [rsp].
[bits 64]
default rel

section .text

global haku64_syscall0
global haku64_syscall1
global haku64_syscall2
global haku64_syscall3
global haku64_syscall4
global haku64_syscall5
global haku64_syscall6

haku64_syscall0:
    mov rax, rcx
    syscall
    ret

haku64_syscall1:
    mov rax, rcx
    mov rdi, rdx
    syscall
    ret

haku64_syscall2:
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    syscall
    ret

haku64_syscall3:
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    syscall
    ret

haku64_syscall4:
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, [rsp + 40]
    syscall
    ret

haku64_syscall5:
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, [rsp + 40]
    mov r8, [rsp + 48]
    syscall
    ret

haku64_syscall6:
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, [rsp + 40]
    mov r8, [rsp + 48]
    mov r9, [rsp + 56]
    syscall
    ret
