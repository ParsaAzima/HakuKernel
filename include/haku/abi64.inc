; Haku x86-64 user ABI constants for NASM sources.
;
; The 64-bit kernel dispatches the x86-64 Linux numbers from two entry
; points: the SYSCALL instruction (fast path, returns with SYSRET) and the
; DPL-3 INT 0x80 gate (fallback). Both use the same convention: RAX number,
; arguments in RDI/RSI/RDX/R10/R8/R9, result in RAX or a negative errno.
;
; include/haku/abi64.h is the C counterpart, and build.py checks that every
; constant the header defines is present here with the same value. Keep the
; numbers here in step with K64_SYS_* in
; kernel/arch/x86_64/kernel/haku64.h.
;
; Because the C header is the reference for the check, the two files share
; names: HAKU64_SYS_*, HAKU64_E*, HAKU64_O_*, HAKU64_AT_*, and so on. The
; extra %defines at the bottom (clusters, futex ops) are NASM-only.

%define HAKU64_SYSCALL_VECTOR 0x80

%define HAKU64_SYS_READ          0
%define HAKU64_SYS_WRITE         1
%define HAKU64_SYS_OPEN          2
%define HAKU64_SYS_CLOSE         3
%define HAKU64_SYS_STAT          4
%define HAKU64_SYS_FSTAT         5
%define HAKU64_SYS_LSTAT         6
%define HAKU64_SYS_POLL          7
%define HAKU64_SYS_LSEEK         8
%define HAKU64_SYS_MMAP          9
%define HAKU64_SYS_MPROTECT      10
%define HAKU64_SYS_MUNMAP        11
%define HAKU64_SYS_BRK           12
%define HAKU64_SYS_IOCTL         16
%define HAKU64_SYS_PREAD64       17
%define HAKU64_SYS_PWRITE64      18
%define HAKU64_SYS_WRITEV        20
%define HAKU64_SYS_ACCESS        21
%define HAKU64_SYS_DUP           32
%define HAKU64_SYS_DUP2          33
%define HAKU64_SYS_NANOSLEEP     35
%define HAKU64_SYS_GETPID        39
%define HAKU64_SYS_FORK          57
%define HAKU64_SYS_EXECVE        59
%define HAKU64_SYS_EXIT          60
%define HAKU64_SYS_WAIT4         61
%define HAKU64_SYS_UNAME         63
%define HAKU64_SYS_FCNTL         72
%define HAKU64_SYS_FSYNC         74
%define HAKU64_SYS_FDATASYNC     75
%define HAKU64_SYS_TRUNCATE      76
%define HAKU64_SYS_FTRUNCATE     77
%define HAKU64_SYS_GETCWD        79
%define HAKU64_SYS_CHDIR         80
%define HAKU64_SYS_FCHDIR        81
%define HAKU64_SYS_RENAME        82
%define HAKU64_SYS_MKDIR         83
%define HAKU64_SYS_RMDIR         84
%define HAKU64_SYS_UNLINK        87
%define HAKU64_SYS_GETPPID       110
%define HAKU64_SYS_ARCH_PRCTL    158
%define HAKU64_SYS_GETTID        186
%define HAKU64_SYS_GETDENTS64    217
%define HAKU64_SYS_CLOCK_GETTIME 228
%define HAKU64_SYS_CLOCK_GETRES  229
%define HAKU64_SYS_EXIT_GROUP    231
%define HAKU64_SYS_OPENAT        257
%define HAKU64_SYS_MKDIRAT       258
%define HAKU64_SYS_NEWFSTATAT    262
%define HAKU64_SYS_UNLINKAT      263
%define HAKU64_SYS_RENAMEAT      264
%define HAKU64_SYS_DUP3          292
%define HAKU64_SYS_GETRANDOM     318
%define HAKU64_SYS_STATX         332

; arch_prctl(2) subcommands and the auxv keys the startup stub reads.
%define HAKU64_ARCH_SET_GS 0x1001
%define HAKU64_ARCH_SET_FS 0x1002
%define HAKU64_ARCH_GET_FS 0x1003
%define HAKU64_ARCH_GET_GS 0x1004

%define HAKU64_AT_NULL   0
%define HAKU64_AT_PHDR   3
%define HAKU64_AT_PHENT  4
%define HAKU64_AT_PHNUM  5
%define HAKU64_AT_PAGESZ 6
%define HAKU64_AT_BASE   7
%define HAKU64_AT_FLAGS  8
%define HAKU64_AT_ENTRY  9
%define HAKU64_AT_UID    11
%define HAKU64_AT_EUID   12
%define HAKU64_AT_GID    13
%define HAKU64_AT_EGID   14
%define HAKU64_AT_HWCAP  16
%define HAKU64_AT_SECURE 23
%define HAKU64_AT_RANDOM 25
%define HAKU64_AT_EXECFN 31

%define HAKU64_PT_TLS 7

; errno values the C library and the 64-bit tests compare against.
%define HAKU64_EPERM   1
%define HAKU64_ENOENT  2
%define HAKU64_EIO     5
%define HAKU64_ENOEXEC 8
%define HAKU64_EBADF   9
%define HAKU64_EAGAIN  11
%define HAKU64_ENOMEM  12
%define HAKU64_EACCES  13
%define HAKU64_EFAULT  14
%define HAKU64_EBUSY   16
%define HAKU64_EEXIST  17
%define HAKU64_ENODEV  19
%define HAKU64_ENOTDIR 20
%define HAKU64_EISDIR  21
%define HAKU64_EINVAL  22
%define HAKU64_ENFILE  23
%define HAKU64_EMFILE  24
%define HAKU64_ENOSPC  28
%define HAKU64_ESPIPE  29
%define HAKU64_ERANGE  34
%define HAKU64_ENAMETOOLONG 36
%define HAKU64_ENOSYS  38
%define HAKU64_ENOTEMPTY 39

; *at flags and access modes.
%define HAKU64_AT_FDCWD          -100
%define HAKU64_AT_SYMLINK_NOFOLLOW 0x100
%define HAKU64_AT_REMOVEDIR      0x200
%define HAKU64_AT_EMPTY_PATH     0x1000
%define HAKU64_F_OK 0
%define HAKU64_X_OK 1
%define HAKU64_W_OK 2
%define HAKU64_R_OK 4

; open() flags.
%define HAKU64_O_RDONLY 0
%define HAKU64_O_WRONLY 1
%define HAKU64_O_RDWR   2
%define HAKU64_O_CREAT  0x40
%define HAKU64_O_EXCL   0x80
%define HAKU64_O_TRUNC  0x200
%define HAKU64_O_APPEND 0x400
%define HAKU64_O_NONBLOCK 0x800
%define HAKU64_O_DIRECTORY 0x10000
%define HAKU64_O_CLOEXEC 0x80000

; fcntl(2) commands.
%define HAKU64_F_DUPFD         0
%define HAKU64_F_GETFD         1
%define HAKU64_F_SETFD         2
%define HAKU64_F_GETFL         3
%define HAKU64_F_SETFL         4
%define HAKU64_F_DUPFD_CLOEXEC 1030

%define HAKU64_SEEK_SET 0
%define HAKU64_SEEK_CUR 1
%define HAKU64_SEEK_END 2

%define HAKU64_PROT_NONE  0
%define HAKU64_PROT_READ  1
%define HAKU64_PROT_WRITE 2
%define HAKU64_PROT_EXEC  4

%define HAKU64_MAP_SHARED    0x01
%define HAKU64_MAP_PRIVATE   0x02
%define HAKU64_MAP_FIXED     0x10
%define HAKU64_MAP_ANONYMOUS 0x20

%define HAKU64_CLOCK_REALTIME  0
%define HAKU64_CLOCK_MONOTONIC 1

%define HAKU64_WNOHANG 1

; File type and directory-entry types as the kernel reports them.
%define HAKU64_S_IFMT  0xF000
%define HAKU64_S_IFREG 0x8000
%define HAKU64_S_IFDIR 0x4000
%define HAKU64_S_IFCHR 0x2000
%define HAKU64_DT_UNKNOWN 0
%define HAKU64_DT_CHR 2
%define HAKU64_DT_DIR 4
%define HAKU64_DT_REG 8

; NASM-only: clone() flags and futex operations used by the ring-3 test.
%define HAKU64_SYS_CLONE          56
%define HAKU64_SYS_FUTEX          202
%define HAKU64_SYS_GETRLIMIT      97
%define HAKU64_SYS_GETTIMEOFDAY   96
%define HAKU64_SYS_PRCTL          157
%define HAKU64_SYS_SCHED_YIELD    24
%define HAKU64_SYS_SELECT         23
%define HAKU64_SYS_TIMES          100

%define HAKU64_PR_SET_NAME 15
%define HAKU64_PR_GET_NAME 16
%define HAKU64_POLLIN  1
%define HAKU64_POLLOUT 4

%define HAKU64_CLONE_VM            0x00000100
%define HAKU64_CLONE_FS            0x00000200
%define HAKU64_CLONE_FILES         0x00000400
%define HAKU64_CLONE_SIGHAND       0x00000800
%define HAKU64_CLONE_THREAD        0x00010000
%define HAKU64_CLONE_SYSVSEM       0x00040000
%define HAKU64_CLONE_PARENT_SETTID 0x00100000

%define HAKU64_FUTEX_WAIT         0
%define HAKU64_FUTEX_WAKE         1
%define HAKU64_FUTEX_PRIVATE_FLAG 128
