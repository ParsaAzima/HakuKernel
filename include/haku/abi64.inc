; Haku x86-64 system call numbers used by the 64-bit test images.
;
; The 64-bit kernel dispatches the x86-64 Linux numbers from two entry
; points: the SYSCALL instruction (fast path, returns with SYSRET) and the
; DPL-3 INT 0x80 gate (fallback). Both use the same convention: RAX number,
; arguments in RDI/RSI/RDX/R10/R8/R9, result in RAX or a negative errno.
; Keep these in step with K64_SYS_* in kernel/arch/x86_64/kernel/haku64.h.

%define HAKU64_SYSCALL_VECTOR 0x80

%define HAKU64_SYS_READ          0
%define HAKU64_SYS_WRITE         1
%define HAKU64_SYS_OPEN          2
%define HAKU64_SYS_CLOSE         3
%define HAKU64_SYS_LSEEK         8
%define HAKU64_SYS_MMAP          9
%define HAKU64_SYS_MUNMAP        11
%define HAKU64_SYS_WRITEV        20
%define HAKU64_SYS_DUP           32
%define HAKU64_SYS_DUP2          33
%define HAKU64_SYS_NANOSLEEP     35
%define HAKU64_SYS_GETPID        39
%define HAKU64_SYS_CLONE         56
%define HAKU64_SYS_FORK          57
%define HAKU64_SYS_EXECVE        59
%define HAKU64_SYS_EXIT          60
%define HAKU64_SYS_WAIT4         61
%define HAKU64_SYS_KILL          62
%define HAKU64_SYS_MKDIR         83
%define HAKU64_SYS_GETPPID       110
%define HAKU64_SYS_GETTID        186
%define HAKU64_SYS_FUTEX         202
%define HAKU64_SYS_CLOCK_GETTIME 228
%define HAKU64_SYS_EXIT_GROUP    231

; clone() flags; the kernel honours exactly this subset.
%define HAKU64_CLONE_VM            0x00000100
%define HAKU64_CLONE_FS            0x00000200
%define HAKU64_CLONE_FILES         0x00000400
%define HAKU64_CLONE_SIGHAND       0x00000800
%define HAKU64_CLONE_THREAD        0x00010000
%define HAKU64_CLONE_SYSVSEM       0x00040000
%define HAKU64_CLONE_PARENT_SETTID 0x00100000

; futex() operations; FUTEX_PRIVATE_FLAG is accepted and ignored.
%define HAKU64_FUTEX_WAIT         0
%define HAKU64_FUTEX_WAKE         1
%define HAKU64_FUTEX_PRIVATE_FLAG 128

%define HAKU64_O_RDONLY 0
%define HAKU64_O_WRONLY 1
%define HAKU64_O_RDWR   2
%define HAKU64_O_CREAT  0x40
%define HAKU64_O_EXCL   0x80
%define HAKU64_O_TRUNC  0x200
%define HAKU64_O_APPEND 0x400

%define HAKU64_SEEK_SET 0
%define HAKU64_SEEK_CUR 1
%define HAKU64_SEEK_END 2

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
%define HAKU64_ENOENT 2
%define HAKU64_ENOEXEC 8
%define HAKU64_EBADF  9
%define HAKU64_EAGAIN 11
%define HAKU64_ENOMEM 12
%define HAKU64_EFAULT 14
%define HAKU64_EEXIST 17
%define HAKU64_EINVAL 22
%define HAKU64_ENOSYS 38
