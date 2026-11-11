; The program the ring-3 test runs with execve() from the FAT12 volume.
;
; build.py wraps this flat binary in a static ELF64 image, stores it as
; USER.ELF on the seed disk, and the test forks: the child execs the file
; with two arguments and one environment variable, this program checks the
; stack image execve built, prints one line per fact and exits with 42.
;
; Three facts come out of that: the image came off the disk (the kernel
; image does not contain it), execve really replaced the child's address
; space, and argc/argv/envp landed where a Linux static startup stub expects
; them ([rsp] = argc, then argv[], NULL, envp[], NULL, auxv).
[bits 64]
default rel

%include "haku/abi64.inc"

_start:
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_exec]
    mov edx, msg_exec_end - msg_exec
    syscall

    ; argc must be 3: the program name, "alpha" and "beta".
    mov rbx, rsp
    cmp qword [rbx], 3
    jne bad_argc

    ; argv[0] is the program path at [rsp + 8], argv[1] = "alpha" at
    ; [rsp + 16], argv[2] = "beta" at [rsp + 24].
    mov rsi, [rbx + 8]
    lea rdi, [arg_path]
    call streq
    test eax, eax
    jz bad_arg0

    mov rsi, [rbx + 16]
    lea rdi, [arg_alpha]
    call streq
    test eax, eax
    jz bad_arg1

    ; argv[2] must be "beta".
    mov rsi, [rbx + 24]
    lea rdi, [arg_beta]
    call streq
    test eax, eax
    jz bad_arg2

    ; envp starts after argv's NULL: rsp + 8 * (argc + 2).
    mov rax, [rbx]
    add rax, 2
    shl rax, 3
    add rax, rbx
    mov rsi, [rax]
    lea rdi, [env_one]
    call streq
    test eax, eax
    jz bad_env

    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_args]
    mov edx, msg_args_end - msg_args
    syscall

    mov eax, HAKU64_SYS_EXIT
    mov edi, 42
    syscall
    jmp $

bad_argc:
    lea rsi, [msg_bad_argc]
    mov edx, msg_bad_argc_end - msg_bad_argc
    mov r9d, 61
    jmp bad
bad_arg0:
    lea rsi, [msg_bad_arg0]
    mov edx, msg_bad_arg0_end - msg_bad_arg0
    mov r9d, 60
    jmp bad
bad_arg1:
    lea rsi, [msg_bad_arg1]
    mov edx, msg_bad_arg1_end - msg_bad_arg1
    mov r9d, 62
    jmp bad
bad_arg2:
    lea rsi, [msg_bad_arg2]
    mov edx, msg_bad_arg2_end - msg_bad_arg2
    mov r9d, 63
    jmp bad
bad_env:
    lea rsi, [msg_bad_env]
    mov edx, msg_bad_env_end - msg_bad_env
    mov r9d, 64
bad:
    ; Each check has its own status and line, so a stack-layout regression
    ; says which part of the image execve built was wrong.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    syscall
    mov eax, HAKU64_SYS_EXIT
    mov edi, r9d
    syscall
    jmp $

; streq(rsi = candidate, rdi = expected) -> eax = 1 when equal.
streq:
    xor ecx, ecx
.loop:
    mov al, [rsi + rcx]
    cmp al, [rdi + rcx]
    jne .no
    test al, al
    jz .yes
    inc rcx
    jmp .loop
.no:
    xor eax, eax
    ret
.yes:
    mov eax, 1
    ret

msg_exec:
    db 'HAKU64-EXEC-PROG', 13, 10
msg_exec_end:

msg_args:
    db 'HAKU64-EXEC-ARGS', 13, 10
msg_args_end:

msg_bad_argc:
    db 'HAKU64-EXEC-BAD-ARGC', 13, 10
msg_bad_argc_end:

msg_bad_arg0:
    db 'HAKU64-EXEC-BAD-ARG0', 13, 10
msg_bad_arg0_end:

msg_bad_arg1:
    db 'HAKU64-EXEC-BAD-ARG1', 13, 10
msg_bad_arg1_end:

msg_bad_arg2:
    db 'HAKU64-EXEC-BAD-ARG2', 13, 10
msg_bad_arg2_end:

msg_bad_env:
    db 'HAKU64-EXEC-BAD-ENV', 13, 10
msg_bad_env_end:

arg_path:
    db '/USER.ELF', 0

arg_alpha:
    db 'alpha', 0

arg_beta:
    db 'beta', 0

env_one:
    db 'A=1', 0
