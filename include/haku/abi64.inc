; Haku x86-64 system call numbers used by the 64-bit test images.
;
; The 64-bit kernel dispatches the x86-64 Linux numbers from two entry
; points: the SYSCALL instruction (fast path, returns with SYSRET) and the
; DPL-3 INT 0x80 gate (fallback). Both use the same convention: RAX number,
; arguments in RDI/RSI/RDX/R10/R8/R9, result in RAX or a negative errno.
; Keep these in step with K64_SYS_* in kernel/arch/x86_64/kernel/haku64.h.

%define HAKU64_SYSCALL_VECTOR 0x80

%define HAKU64_SYS_WRITE         1
%define HAKU64_SYS_MMAP          9
%define HAKU64_SYS_MUNMAP        11
%define HAKU64_SYS_WRITEV        20
%define HAKU64_SYS_GETPID        39
%define HAKU64_SYS_EXIT          60
%define HAKU64_SYS_CLOCK_GETTIME 228
%define HAKU64_SYS_EXIT_GROUP    231

%define HAKU64_PROT_READ  1
%define HAKU64_PROT_WRITE 2
%define HAKU64_PROT_EXEC  4

%define HAKU64_MAP_SHARED    0x01
%define HAKU64_MAP_PRIVATE   0x02
%define HAKU64_MAP_FIXED     0x10
%define HAKU64_MAP_ANONYMOUS 0x20

%define HAKU64_CLOCK_REALTIME  0
%define HAKU64_CLOCK_MONOTONIC 1

; Linux errno values the test program compares against.
%define HAKU64_EBADF  9
%define HAKU64_ENOMEM 12
%define HAKU64_EFAULT 14
%define HAKU64_EINVAL 22
%define HAKU64_ENOSYS 38
