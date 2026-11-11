; The program the ring-3 test runs with execve() from the FAT12 volume.
;
; build.py wraps this flat binary in a static ELF64 image, stores it as
; USER.ELF on the seed disk, and the test forks: the child execs the file,
; prints one line and exits with 42. Two facts come out of that: the image
; came off the disk (the kernel image does not contain it), and execve
; really replaced the child's address space, because the printed line and
; the status both belong to this program.
[bits 64]
default rel

%include "haku/abi64.inc"

_start:
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_exec]
    mov edx, msg_exec_end - msg_exec
    syscall

    ; Exit with a status the parent can recognise: 42 is not a number any
    ; other test in this image uses.
    mov eax, HAKU64_SYS_EXIT
    mov edi, 42
    syscall
    jmp $

msg_exec:
    db 'HAKU64-EXEC-PROG', 13, 10
msg_exec_end:
