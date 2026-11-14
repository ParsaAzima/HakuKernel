; Haku x86-64 C runtime entry.
;
; execve() and the kernel's /INIT.ELF launch leave the x86-64 Linux stack
; image at RSP:
;
;     [rsp]      argc
;     [rsp+8]    argv[0..argc-1], NULL
;                envp[0..], NULL
;                auxiliary vector (AT_PHDR, AT_PHNUM, AT_PAGESZ, AT_ENTRY,
;                                  AT_RANDOM, AT_EXECFN, ... AT_NULL)
;
; main() is compiled for the Microsoft x64 ABI, which passes its three
; arguments in RCX/RDX/R8, so this stub reads the stack image and moves the
; values there. It then aligns RSP to the 16-byte boundary a CALL leaves and
; jumps into main(). When main() returns, its value becomes the exit status.
;
; The stack image is read before anything can write to it, and RSP then
; moves below it: a C function is allowed to spill its register arguments
; into the 32-byte shadow space at [rsp+8], which would otherwise overlap
; argc/argv/envp on the initial stack. argc/argv/envp are kept in the
; callee-saved registers across the TLS setup call.
;
; TLS is not optional for a program that uses __declspec(thread), so the
; auxiliary vector is walked here and haku64_tls_init() builds the TEB and
; the per-module block before main() can touch one. A failure exits with a
; status instead of faulting later. There is still no constructor array and
; no atexit list: a static program that only uses Haku's libc needs neither.
[bits 64]
default rel

section .text

global _start
extern main
extern exit
extern haku64_tls_init

_start:
    xor ebp, ebp                ; the outermost frame, so debuggers stop here
    mov r12, [rsp]              ; argc
    lea r13, [rsp + 8]          ; argv
    lea r14, [r13 + r12 * 8 + 8] ; envp follows argv's NULL terminator

    ; envp is a NULL-terminated vector, and the auxiliary vector starts at
    ; the word after that NULL.
    mov r15, r14
.find_auxv:
    cmp qword [r15], 0
    je .auxv_here
    add r15, 8
    jmp .find_auxv
.auxv_here:
    add r15, 8
    cmp qword [r15], 0          ; AT_NULL as the first word means no auxv
    je .no_tls

    sub rsp, 32                 ; shadow space for the C call
    mov rcx, r15
    call haku64_tls_init
    add rsp, 32
    test eax, eax
    jz .tls_ready
    mov ecx, 70                 ; EX_SOFTWARE: the thread state is missing
    call exit
.no_tls:
.tls_ready:
    ; main()'s shadow space is [rsp .. rsp+0x20) once the return address is
    ; pushed, which would land on top of the argc/argv image just read. Put
    ; RSP well below it so those words stay intact for the whole program.
    sub rsp, 128
    and rsp, -16                ; main() may assume a 16-byte aligned stack
    mov rcx, r12
    mov rdx, r13
    mov r8, r14
    call main
    mov ecx, eax                ; MSVC x64: the first argument is RCX
    call exit
    hlt                         ; exit() does not return
