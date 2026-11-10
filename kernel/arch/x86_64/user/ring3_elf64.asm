; Minimal static ELF64 executable used by the x86-64 syscall/ELF self-test.
;
; tools/mkelf64.py wraps this flat binary in an ELF image at USER64_BASE and
; the kernel embeds the result in .rdata. The program exercises the six
; argument INT 0x80 ABI: the success paths for write, writev, getpid,
; clock_gettime, mmap and munmap, and the error paths for a bad descriptor,
; a bad user pointer, an unknown number, a W+X mapping, a bad mmap
; descriptor/offset and an unaligned munmap. It prints HAKU64-ABI-OK, stores
; a magic word in its zero-filled BSS page and exits with status 0; the
; kernel reads both back, which proves the image ran in ring 3 instead of
; merely being mapped.
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

; A free page inside the kernel's anonymous mmap window (K64_USER_MMAP_BASE
; is 0x8010000000), used to exercise MAP_FIXED.
%define USER64_FIXED 0x0000008010008000

_start:
    ; write(1, "ELF64\n", 6) through SYSCALL, then prove the SYSRET return
    ; with a second write that only ring 3 can reach after the first one.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_boot]
    mov edx, msg_boot_end - msg_boot
    syscall
    cmp rax, msg_boot_end - msg_boot
    jne fail

    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_sys64]
    mov edx, msg_sys64_end - msg_sys64
    syscall
    cmp rax, msg_sys64_end - msg_sys64
    jne fail

    ; getpid() must return a non-zero task id.
    mov eax, HAKU64_SYS_GETPID
    syscall
    test rax, rax
    jle fail

    ; clock_gettime(CLOCK_MONOTONIC, &timespec) writes a sane tv_nsec.
    sub rsp, 32
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov rsi, rsp
    syscall
    test rax, rax
    jne fail
    cmp qword [rsp + 8], 1000000000
    jae fail

    ; CLOCK_REALTIME is anchored on the CMOS RTC, so its seconds field must
    ; be at least the monotonic count and still carry a sane tv_nsec.
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_REALTIME
    lea rsi, [rsp + 16]
    syscall
    test rax, rax
    jne fail
    cmp qword [rsp + 24], 1000000000
    jae fail
    mov rax, [rsp + 16]
    cmp rax, [rsp]
    jb fail

    ; mmap(NULL, 0x2000, R+W, PRIVATE|ANONYMOUS, -1, 0) returns a user
    ; address that reads back what was written to both pages.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x2000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r12, rax
    mov rax, 0x1122334455667788
    mov [r12], rax
    mov [r12 + 0x1000], rax
    cmp [r12], rax
    jne fail
    cmp [r12 + 0x1000], rax
    jne fail

    ; munmap(base, 0x2000) releases both pages.
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r12
    mov esi, 0x2000
    syscall
    test rax, rax
    jne fail

    ; MAP_FIXED at a free page inside the anonymous window lands exactly
    ; at the requested address.
    mov eax, HAKU64_SYS_MMAP
    mov r14, USER64_FIXED
    mov rdi, r14
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, r14
    jne fail
    mov rax, 0x55AA55AA55AA55AA
    mov [r14], rax
    cmp [r14], rax
    jne fail

    ; Repeating it over the same range is refused: MAP_FIXED maps free
    ; ranges only, it does not replace an existing mapping.
    mov eax, HAKU64_SYS_MMAP
    mov rdi, USER64_FIXED
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; An unaligned MAP_FIXED address is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    lea rdi, [r14 + 1]
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; MAP_SHARED | MAP_ANONYMOUS is accepted and behaves like a private
    ; anonymous page until there is a fork to share it with.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_SHARED | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov rax, 0x1234567890ABCDEF
    mov [r13], rax
    cmp [r13], rax
    jne fail

    ; MAP_PRIVATE together with MAP_SHARED is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_SHARED | HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; Release both of those pages again.
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r14
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r13
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    ; The INT 0x80 gate stays available; the error paths below use it so
    ; both entry points run in the same program.
    ; write() to a descriptor other than 1 or 2 is -EBADF.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 99
    lea rsi, [msg_boot]
    mov edx, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EBADF
    jne fail

    ; A pointer outside the user half is -EFAULT, not a kernel fault.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    mov esi, 0x10
    mov edx, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EFAULT
    jne fail

    ; An unimplemented number is -ENOSYS.
    mov eax, 9999
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_ENOSYS
    jne fail

    ; mmap with W+X, a real descriptor, or a non-zero offset is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE | HAKU64_PROT_EXEC
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    xor r8d, r8d
    xor r9d, r9d
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    mov r9d, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; munmap of an unaligned address is -EINVAL.
    mov eax, HAKU64_SYS_MUNMAP
    lea rdi, [r12 + 1]
    mov esi, 0x1000
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; clock_gettime() to an unmapped pointer is -EFAULT.
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov esi, 0x10
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EFAULT
    jne fail

    ; writev(1, {"HAKU64-ABI-OK", "\n"}, 2) is the ABI success marker.
    lea rax, [msg_abi]
    mov [rsp], rax
    mov qword [rsp + 8], msg_abi_end - msg_abi
    lea rax, [msg_nl]
    mov [rsp + 16], rax
    mov qword [rsp + 24], 1
    mov eax, HAKU64_SYS_WRITEV
    mov edi, 1
    mov rsi, rsp
    mov edx, 2
    syscall
    cmp rax, (msg_abi_end - msg_abi) + (msg_nl_end - msg_nl)
    jne fail

    ; The BSS store is read back by the kernel after the task exits.
    mov rax, USER64_BSS
    mov dword [rax], 0x48414B55       ; "HAKU", read back by the kernel
    mov eax, HAKU64_SYS_EXIT
    xor edi, edi
    syscall

.hang:
    jmp .hang

fail:
    mov eax, HAKU64_SYS_EXIT
    mov edi, 1
    int HAKU64_SYSCALL_VECTOR
    jmp $

msg_boot:
    db 'ELF64', 10
msg_boot_end:

msg_sys64:
    db 'HAKU64-SYSCALL64-OK', 10
msg_sys64_end:

msg_abi:
    db 'HAKU64-ABI-OK'
msg_abi_end:

msg_nl:
    db 10
msg_nl_end:
