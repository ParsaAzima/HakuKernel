; Haku x86-64 raw system call stubs.
;
; The C prototypes take (number, a1..a6). The Windows x64 C compiler passes
; those in RCX/RDX/R8/R9 and on the stack, while the Haku SYSCALL entry wants
; the number in RAX and the arguments in RDI/RSI/RDX/R10/R8/R9. Each stub
; rearranges its incoming registers and executes SYSCALL. RCX and R11 are
; clobbered by the instruction, which the C caller already treats as volatile.
;
; RDI and RSI are nonvolatile under the Microsoft x64 ABI, and the stubs use
; both to build the kernel's argument registers, so they are saved and
; restored around the call. Clang in particular keeps live values in RSI
; across a call to one of these stubs; without the save/restore the caller
; silently loses them (a syscall wrapper returning its own buffer pointer
; instead of the kernel's result is how that shows up).
;
; The 7th argument of haku64_syscall6 (a6) is the first stack argument. The
; Microsoft x64 caller reserves 32 bytes of shadow space above the return
; address, so a4..a6 sit at [rsp+40], [rsp+48] and [rsp+56] on entry; the two
; pushes below add 16 to each.
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

%macro SAVE_NONVOLATILE 0
    push rdi
    push rsi
%endmacro

%macro RESTORE_NONVOLATILE 0
    pop rsi
    pop rdi
%endmacro

; Stack arguments in the caller's frame: 32 bytes of shadow space, the
; return address, then the two saves above it.
%define STACK_ARG(n) [rsp + 56 + 8 * (n)]

haku64_syscall0:
    SAVE_NONVOLATILE
    mov rax, rcx
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall1:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall2:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall3:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall4:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, STACK_ARG(0)
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall5:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, STACK_ARG(0)
    mov r8, STACK_ARG(1)
    syscall
    RESTORE_NONVOLATILE
    ret

haku64_syscall6:
    SAVE_NONVOLATILE
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rdx, r9
    mov r10, STACK_ARG(0)
    mov r8, STACK_ARG(1)
    mov r9, STACK_ARG(2)
    syscall
    RESTORE_NONVOLATILE
    ret
