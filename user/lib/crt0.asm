[bits 32]
section .text

%include "haku/abi.inc"

global _start
extern _main
extern _haku_tls_init

; The initial stack the kernel built: argc, argv[], NULL, envp[], NULL,
; then the auxiliary vector. A libc reads it before main; this runtime
; only keeps the pointer so the self-test can walk the same words.
section .data
global _haku_initial_sp
_haku_initial_sp: dd 0

section .text
_start:
    mov [_haku_initial_sp], esp
    ; errno and every thread-local access go through the block installed
    ; here, so no library call may happen before it. A failure exits with a
    ; status instead of faulting on the first fs-relative access.
    call _haku_tls_init
    test eax, eax
    jz .tls_ready
    mov ebx, 1
    mov eax, HAKU_SYS_EXIT
    int 0x80
    hlt
.tls_ready:
    call _main
    mov ebx, eax
    mov eax, HAKU_SYS_EXIT
    int 0x80
    hlt
