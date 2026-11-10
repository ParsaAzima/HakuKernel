; Minimal static ELF64 executable used by the x86-64 ELF loader self-test.
;
; tools/mkelf64.py wraps this flat binary in an ELF image at USER64_BASE and
; the kernel embeds the result in .rdata. The program writes one message
; through the INT 0x80 gate, stores a magic word in its zero-filled BSS page,
; and exits. The kernel reads the word back after the task is done, which
; proves the image ran in ring 3 instead of merely being mapped.
;
; The code is position independent (RIP-relative operands only), so the flat
; binary does not need a linker to resolve USER64_BASE: mkelf64.py chooses the
; load address, and USER64_BASE is only needed for the BSS store.
[bits 64]
default rel

%include "haku/abi64.inc"

%ifndef USER64_BASE
%define USER64_BASE 0x0000008000400000
%endif

; mkelf64.py puts the read-write PT_LOAD segment one page above the code.
%define USER64_BSS (USER64_BASE + 0x1000)

    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [message]
    mov edx, message_end - message
    int HAKU64_SYSCALL_VECTOR
    cmp rax, rdx
    jne .fail

    mov rax, USER64_BSS
    mov dword [rax], 0x48414B55       ; "HAKU", read back by the kernel
    mov eax, HAKU64_SYS_EXIT
    xor edi, edi
    int HAKU64_SYSCALL_VECTOR

.hang:
    jmp .hang

.fail:
    mov eax, HAKU64_SYS_EXIT
    mov edi, 1
    int HAKU64_SYSCALL_VECTOR
    jmp .hang

message:
    db 'ELF64'
message_end:
