; Haku x86-64 C runtime entry.
;
; execve() (and the kernel's /INIT.ELF launch) leaves the x86-64 Linux stack
; image at RSP:
;
;     [rsp]      argc
;     [rsp+8]    argv[0..argc-1], NULL
;                envp[0..], NULL
;                auxiliary vector (AT_PAGESZ, AT_ENTRY, AT_NULL)
;
; main() is compiled for the Microsoft x64 ABI, which passes its three
; arguments in RCX/RDX/R8, so this stub reads the stack image and moves the
; values there. It then aligns RSP to the 16-byte boundary a CALL leaves and
; jumps into main(). When main() returns, its value becomes the exit status.
;
; The stack image is read before anything can write to it, and RSP then
; moves below it: a C function is allowed to spill its register arguments
; into the 32-byte shadow space at [rsp+8], which would otherwise overlap
; argc/argv/envp on the initial stack.
;
; There is no constructor array, no TLS and no atexit list: a static program
; that only uses Haku's libc needs none of them, and inventing them here
; would be a promise the library does not keep.
[bits 64]
default rel

section .text

global _start
extern main
extern exit

_start:
    xor ebp, ebp                ; the outermost frame, so debuggers stop here
    mov rcx, [rsp]              ; argc
    lea rdx, [rsp + 8]          ; argv
    lea r8, [rdx + rcx * 8 + 8] ; envp follows argv's NULL terminator
    ; main()'s shadow space is [rsp .. rsp+0x20) once the return address is
    ; pushed, which would land on top of the argc/argv image just read. Put
    ; RSP well below it so those words stay intact for the whole program.
    sub rsp, 128
    and rsp, -16                ; main() may assume a 16-byte aligned stack
    call main
    mov ecx, eax                ; MSVC x64: the first argument is RCX
    call exit
    hlt                         ; exit() does not return
