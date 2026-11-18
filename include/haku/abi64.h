/* Haku x86-64 user ABI: types, syscall numbers, layouts and wrappers.
 *
 * The 32-bit userland includes <haku/abi.h>, which carries i386 layouts
 * (a 64-byte struct stat, 8-byte iovec, 32-bit pointers). This header is the
 * x86-64 counterpart: a pointer and a long are 64 bits, `struct stat` is the
 * 144-byte x86-64 Linux layout syscall64.c fills, and every constant is the
 * number the 64-bit kernel dispatches (the Linux x86-64 table).
 *
 * Keep the HAKU64_* numbers here and in include/haku/abi64.inc in step:
 * build.py checks every value this header defines against the NASM file.
 * Userspace C uses this header; NASM uses the .inc.
 */
#ifndef HAKU_ABI64_H
#define HAKU_ABI64_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned long long size_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long long ssize_t;
#endif

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long long off_t;
#endif

#ifndef HAKU64_PID_T_DEFINED
#define HAKU64_PID_T_DEFINED
typedef int pid_t;
#endif

#ifndef HAKU64_MODE_T_DEFINED
#define HAKU64_MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

#ifndef HAKU64_TIME_T_DEFINED
#define HAKU64_TIME_T_DEFINED
typedef long long time_t;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ------------------------------------------------------------- syscalls
 * x86-64 Linux numbers. The number goes in RAX, arguments in
 * RDI/RSI/RDX/R10/R8/R9, and the result is RAX or a negative errno. */
#define HAKU64_SYS_READ          0
#define HAKU64_SYS_WRITE         1
#define HAKU64_SYS_OPEN          2
#define HAKU64_SYS_CLOSE         3
#define HAKU64_SYS_STAT          4
#define HAKU64_SYS_FSTAT         5
#define HAKU64_SYS_LSTAT         6
#define HAKU64_SYS_POLL          7
#define HAKU64_SYS_LSEEK         8
#define HAKU64_SYS_MMAP          9
#define HAKU64_SYS_MPROTECT      10
#define HAKU64_SYS_MUNMAP        11
#define HAKU64_SYS_BRK           12
#define HAKU64_SYS_RT_SIGACTION  13
#define HAKU64_SYS_RT_SIGPROCMASK 14
#define HAKU64_SYS_RT_SIGRETURN  15
#define HAKU64_SYS_IOCTL         16
#define HAKU64_SYS_PREAD64       17
#define HAKU64_SYS_PWRITE64      18
#define HAKU64_SYS_WRITEV        20
#define HAKU64_SYS_ACCESS        21
#define HAKU64_SYS_PIPE          22
#define HAKU64_SYS_DUP           32
#define HAKU64_SYS_DUP2          33
#define HAKU64_SYS_NANOSLEEP     35
#define HAKU64_SYS_GETPID        39
#define HAKU64_SYS_CLONE         56
#define HAKU64_SYS_FORK          57
#define HAKU64_SYS_EXECVE        59
#define HAKU64_SYS_EXIT          60
#define HAKU64_SYS_WAIT4         61
#define HAKU64_SYS_KILL          62
#define HAKU64_SYS_UNAME         63
#define HAKU64_SYS_FCNTL         72
#define HAKU64_SYS_FSYNC         74
#define HAKU64_SYS_FDATASYNC     75
#define HAKU64_SYS_TRUNCATE      76
#define HAKU64_SYS_FTRUNCATE     77
#define HAKU64_SYS_GETCWD        79
#define HAKU64_SYS_CHDIR         80
#define HAKU64_SYS_FCHDIR        81
#define HAKU64_SYS_RENAME        82
#define HAKU64_SYS_MKDIR         83
#define HAKU64_SYS_RMDIR         84
#define HAKU64_SYS_UNLINK        87
#define HAKU64_SYS_SETPGID       109
#define HAKU64_SYS_GETPPID       110
#define HAKU64_SYS_GETPGRP       111
#define HAKU64_SYS_SETSID        112
#define HAKU64_SYS_GETPGID       121
#define HAKU64_SYS_GETSID        124
#define HAKU64_SYS_SIGALTSTACK   131
#define HAKU64_SYS_ARCH_PRCTL    158
#define HAKU64_SYS_GETTID        186
#define HAKU64_SYS_FUTEX         202
#define HAKU64_SYS_GETDENTS64    217
#define HAKU64_SYS_CLOCK_GETTIME 228
#define HAKU64_SYS_CLOCK_GETRES  229
#define HAKU64_SYS_EXIT_GROUP    231
#define HAKU64_SYS_TGKILL        234
#define HAKU64_SYS_OPENAT        257
#define HAKU64_SYS_MKDIRAT       258
#define HAKU64_SYS_NEWFSTATAT    262
#define HAKU64_SYS_UNLINKAT      263
#define HAKU64_SYS_RENAMEAT      264
#define HAKU64_SYS_SET_ROBUST_LIST 273
#define HAKU64_SYS_GET_ROBUST_LIST 274
#define HAKU64_SYS_DUP3          292
#define HAKU64_SYS_PIPE2         293
#define HAKU64_SYS_GETRANDOM     318
#define HAKU64_SYS_STATX         332

/* Offsets inside the thread control block (the TEB) this library builds. The
 * +0x30 self pointer and +0x58 TlsSlots pointer are the Windows definitions
 * the compiler's TLS sequence reads; +0x20 is ours and holds errno, and
 * +0x50 holds this thread's pthread handle. */
#define HAKU64_TEB_ERRNO_OFFSET 32
#define HAKU64_TEB_SELF_OFFSET  48
#define HAKU64_TEB_THREAD_OFFSET 80
#define HAKU64_TEB_SLOTS_OFFSET 88

/* clone(2) flags and futex(2) operations the thread API uses. Only the bits
 * below are meaningful here: the kernel refuses the rest rather than
 * pretending to honour them. CHILD_SETTID publishes the new tid in the
 * child's space and CHILD_CLEARTID zeroes that word (and wakes a futex
 * waiter) when the child ends; together they are what a join sleeps on.
 * DETACH says nobody will wait for the task, so the kernel releases its slot
 * at the next switch instead of leaving a zombie behind. */
#define HAKU64_CLONE_VM             0x00000100
#define HAKU64_CLONE_FS             0x00000200
#define HAKU64_CLONE_FILES          0x00000400
#define HAKU64_CLONE_SIGHAND        0x00000800
#define HAKU64_CLONE_THREAD         0x00010000
#define HAKU64_CLONE_SYSVSEM        0x00040000
#define HAKU64_CLONE_SETTLS         0x00080000
#define HAKU64_CLONE_CHILD_CLEARTID 0x00200000
#define HAKU64_CLONE_CHILD_SETTID   0x01000000
#define HAKU64_CLONE_DETACH         0x40000000
#define HAKU64_FUTEX_WAIT           0
#define HAKU64_FUTEX_WAKE           1
#define HAKU64_FUTEX_PRIVATE_FLAG   128
/* A pthread mutex keeps the owner tid in the low 30 bits of its futex word.
 * OWNER_DIED is what the kernel's robust-list walk writes when the holder
 * disappears, and WAITERS marks a word with a sleeper on it so an unlock
 * knows to wake one. */
#define HAKU64_FUTEX_TID_MASK       0x3FFFFFFF
#define HAKU64_FUTEX_OWNER_DIED     0x40000000
#define HAKU64_FUTEX_WAITERS        0x80000000
/* sizeof(struct robust_list_head), the size set_robust_list(273) is given. */
#define HAKU64_ROBUST_LIST_SIZE     24

/* arch_prctl(2) subcommands and the auxv keys crt64.asm and the TLS setup
 * read. The numbers are the Linux x86-64 ones. */
#define HAKU64_ARCH_SET_GS 0x1001
#define HAKU64_ARCH_SET_FS 0x1002
#define HAKU64_ARCH_GET_FS 0x1003
#define HAKU64_ARCH_GET_GS 0x1004

#define HAKU64_AT_NULL   0
#define HAKU64_AT_PHDR   3
#define HAKU64_AT_PHENT  4
#define HAKU64_AT_PHNUM  5
#define HAKU64_AT_PAGESZ 6
#define HAKU64_AT_BASE   7
#define HAKU64_AT_FLAGS  8
#define HAKU64_AT_ENTRY  9
#define HAKU64_AT_UID    11
#define HAKU64_AT_EUID   12
#define HAKU64_AT_GID    13
#define HAKU64_AT_EGID   14
#define HAKU64_AT_HWCAP  16
#define HAKU64_AT_SECURE 23
#define HAKU64_AT_RANDOM 25
#define HAKU64_AT_EXECFN 31

#define HAKU64_PT_TLS 7

/* --------------------------------------------------------------- errno.
 * Only the values this library and its tests compare against. */
#define HAKU64_EPERM   1
#define HAKU64_ENOENT  2
#define HAKU64_ESRCH   3
#define HAKU64_EINTR   4
#define HAKU64_EIO     5
#define HAKU64_ENOEXEC 8
#define HAKU64_EBADF   9
#define HAKU64_EAGAIN  11
#define HAKU64_ENOMEM  12
#define HAKU64_EACCES  13
#define HAKU64_EFAULT  14
#define HAKU64_EBUSY   16
#define HAKU64_EEXIST  17
#define HAKU64_ENODEV  19
#define HAKU64_ENOTDIR 20
#define HAKU64_EISDIR  21
#define HAKU64_EINVAL  22
#define HAKU64_ENFILE  23
#define HAKU64_EMFILE  24
#define HAKU64_ENOTTY  25
#define HAKU64_ENOSPC  28
#define HAKU64_ESPIPE  29
#define HAKU64_EROFS   30
#define HAKU64_EPIPE   32
#define HAKU64_ERANGE  34
#define HAKU64_EDEADLK 35
#define HAKU64_ENAMETOOLONG 36
#define HAKU64_ENOSYS  38
#define HAKU64_ENOTEMPTY 39
#define HAKU64_EOWNERDEAD 130
#define HAKU64_ENOTRECOVERABLE 131

/* Signals (Linux x86-64 numbering). Haku implements the standard range
 * 1..31: real-time signals are rejected with EINVAL. A handler is a ring-3
 * function and needs a restorer, because the kernel cannot place an
 * executable stub on the user stack (it is mapped NX); sigaction() below
 * fills one in, so only code that touches the raw structure has to care.
 *
 * Handler arguments follow the Microsoft x64 C ABI this toolchain compiles
 * for, not Linux's System V one: the signal number arrives in RCX, and with
 * SA_SIGINFO the siginfo pointer is in RDX and the ucontext pointer in R8. */
#define HAKU64_SIGHUP   1
#define HAKU64_SIGINT   2
#define HAKU64_SIGQUIT  3
#define HAKU64_SIGILL   4
#define HAKU64_SIGTRAP  5
#define HAKU64_SIGABRT  6
#define HAKU64_SIGBUS   7
#define HAKU64_SIGFPE   8
#define HAKU64_SIGKILL  9
#define HAKU64_SIGUSR1  10
#define HAKU64_SIGSEGV  11
#define HAKU64_SIGUSR2  12
#define HAKU64_SIGPIPE  13
#define HAKU64_SIGALRM  14
#define HAKU64_SIGTERM  15
#define HAKU64_SIGSTKFLT 16
#define HAKU64_SIGCONT  18
#define HAKU64_SIGSTOP  19
#define HAKU64_SIGTSTP  20
#define HAKU64_SIGTTIN  21
#define HAKU64_SIGTTOU  22
#define HAKU64_SIGURG   23
#define HAKU64_SIGXCPU  24
#define HAKU64_SIGXFSZ  25
#define HAKU64_SIGVTALRM 26
#define HAKU64_SIGPROF  27
#define HAKU64_SIGWINCH 28
#define HAKU64_SIGIO    29
#define HAKU64_SIGPWR   30
#define HAKU64_SIGSYS   31
#define HAKU64_NSIG     31

#define HAKU64_SIG_DFL    0
#define HAKU64_SIG_IGN    1
#define HAKU64_SIG_BLOCK  0
#define HAKU64_SIG_UNBLOCK 1
#define HAKU64_SIG_SETMASK 2

/* sigaction flags. SIGINFO, ONSTACK, RESTORER and NODEFER are honoured; the
 * others are stored unchanged and ignored (SA_RESETHAND does not reset the
 * handler after it runs). */
#define HAKU64_SA_NOCLDSTOP 0x00000001
#define HAKU64_SA_SIGINFO   0x00000004
#define HAKU64_SA_RESTORER  0x04000000
#define HAKU64_SA_ONSTACK   0x08000000
#define HAKU64_SA_RESTART   0x10000000
#define HAKU64_SA_NODEFER   0x40000000
#define HAKU64_SA_RESETHAND 0x80000000

/* sigaltstack(2): only DISABLE is acted on; ONSTACK is reported when the
 * running handler owns the alternate stack. */
#define HAKU64_SS_ONSTACK    1
#define HAKU64_SS_DISABLE    2
#define HAKU64_SS_AUTODISARM 0x80000000

/* --------------------------------------------------------- path and flags */
#define HAKU64_AT_FDCWD -100
#define HAKU64_AT_SYMLINK_NOFOLLOW 0x100
#define HAKU64_AT_REMOVEDIR 0x200
#define HAKU64_AT_EMPTY_PATH 0x1000

#define HAKU64_F_OK 0
#define HAKU64_X_OK 1
#define HAKU64_W_OK 2
#define HAKU64_R_OK 4

#define HAKU64_O_RDONLY 0
#define HAKU64_O_WRONLY 1
#define HAKU64_O_RDWR   2
#define HAKU64_O_CREAT  0x40
#define HAKU64_O_EXCL   0x80
#define HAKU64_O_TRUNC  0x200
#define HAKU64_O_APPEND 0x400
#define HAKU64_O_NONBLOCK 0x800
#define HAKU64_O_DIRECTORY 0x10000
#define HAKU64_O_CLOEXEC 0x80000

#define HAKU64_F_DUPFD         0
#define HAKU64_F_GETFD         1
#define HAKU64_F_SETFD         2
#define HAKU64_F_GETFL         3
#define HAKU64_F_SETFL         4
#define HAKU64_F_DUPFD_CLOEXEC 1030

#define HAKU64_SEEK_SET 0
#define HAKU64_SEEK_CUR 1
#define HAKU64_SEEK_END 2

#define HAKU64_PROT_NONE  0
#define HAKU64_PROT_READ  1
#define HAKU64_PROT_WRITE 2
#define HAKU64_PROT_EXEC  4

#define HAKU64_MAP_SHARED    0x01
#define HAKU64_MAP_PRIVATE   0x02
#define HAKU64_MAP_FIXED     0x10
#define HAKU64_MAP_ANONYMOUS 0x20

#define HAKU64_CLOCK_REALTIME  0
#define HAKU64_CLOCK_MONOTONIC 1

#define HAKU64_WNOHANG 1

/* wait4 options and the status encodings its results use. A stopped child
 * reports (signal << 8) | 0x7F; a resumed one reports 0xFFFF. */
#define HAKU64_WUNTRACED  2
#define HAKU64_WCONTINUED 8
#define HAKU64_WIFEXITED(status)    (((status) & 0x7F) == 0)
#define HAKU64_WEXITSTATUS(status)  (((status) >> 8) & 0xFF)
#define HAKU64_WIFSIGNALED(status)  (((status) & 0x7F) != 0 && \
                                     ((status) & 0x7F) != 0x7F)
#define HAKU64_WTERMSIG(status)     ((status) & 0x7F)
#define HAKU64_WIFSTOPPED(status)   (((status) & 0xFF) == 0x7F)
#define HAKU64_WSTOPSIG(status)     (((status) >> 8) & 0xFF)
#define HAKU64_WIFCONTINUED(status) ((status) == 0xFFFF)

/* The console's termios and winsize records: the Linux i386 layouts, which
 * are also what the x86-64 kernel writes. TCGETS/TCSETS*, TIOCGWINSZ and the
 * foreground-group requests are answered on the console only; every other
 * descriptor fails with ENOTTY. */
typedef struct {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
} haku64_termios_t;

typedef struct {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
} haku64_winsize_t;

#define HAKU64_TCGETS     0x5401
#define HAKU64_TCSETS     0x5402
#define HAKU64_TCSETSW    0x5403
#define HAKU64_TCSETSF    0x5404
#define HAKU64_TCSBRK     0x5409
#define HAKU64_TCFLSH     0x540B
#define HAKU64_TIOCGPGRP  0x540F
#define HAKU64_TIOCSPGRP  0x5410
#define HAKU64_TIOCGWINSZ 0x5413
#define HAKU64_FIONREAD   0x541B
#define HAKU64_TIOCNOTTY  0x5422

/* termios flag bits the console reports. TOSTOP is deliberately absent: a
 * background write is never stopped, so SIGTTOU cannot arise here. */
#define HAKU64_ISIG   0x0001
#define HAKU64_ICANON 0x0002
#define HAKU64_ECHO   0x0008
#define HAKU64_ECHOE  0x0010
#define HAKU64_ECHOK  0x0020
#define HAKU64_TOSTOP 0x0100

#define HAKU64_S_IFMT  0xF000
#define HAKU64_S_IFREG 0x8000
#define HAKU64_S_IFDIR 0x4000
#define HAKU64_S_IFCHR 0x2000
#define HAKU64_S_ISREG(mode) (((mode) & HAKU64_S_IFMT) == HAKU64_S_IFREG)
#define HAKU64_S_ISDIR(mode) (((mode) & HAKU64_S_IFMT) == HAKU64_S_IFDIR)

#define HAKU64_DT_UNKNOWN 0
#define HAKU64_DT_CHR 2
#define HAKU64_DT_DIR 4
#define HAKU64_DT_REG 8

/* ------------------------------------------------------------- layouts */

typedef struct {
    long long tv_sec;
    long long tv_nsec;
} haku64_timespec_t;

typedef struct {
    long long tv_sec;
    long long tv_usec;
} haku64_timeval_t;

typedef struct {
    void *iov_base;
    unsigned long long iov_len;
} haku64_iovec_t;

/* x86-64 "struct stat": 144 bytes, the layout sys64_fill_stat writes. */
typedef struct {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned long long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    unsigned long long st_rdev;
    long long st_size;
    long long st_blksize;
    long long st_blocks;
    long long st_atime_sec;
    long long st_atime_nsec;
    long long st_mtime_sec;
    long long st_mtime_nsec;
    long long st_ctime_sec;
    long long st_ctime_nsec;
    long long __unused[3];
} haku64_stat_t;

typedef struct {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
} haku64_dirent64_t;

typedef struct {
    int fd;
    short events;
    short revents;
} haku64_pollfd_t;

/* rt_sigaction(2) record: handler, flags, restorer, and the signals to block
 * while the handler runs (one 32-bit word, signals 1..31). The kernel copies
 * 32 bytes and rejects a sigset size other than 8, like Linux x86-64. */
typedef struct {
    unsigned long long handler;
    unsigned long long flags;
    unsigned long long restorer;
    unsigned long long mask;
} haku64_sigaction_t;

typedef struct {
    unsigned long long bits;
} haku64_sigset_t;

/* sigaltstack(2) record: stack pointer, flags, size. */
typedef struct {
    void *ss_sp;
    unsigned int ss_flags;
    unsigned int __pad;
    unsigned long long ss_size;
} haku64_stack_t;

/* The saved context in a signal frame. uc_mcontext holds the interrupted
 * register set in the kernel's order: r15..r8, rbp, rdi, rsi, rdx, rcx, rbx,
 * rax, rip, cs, rflags, rsp, ss. */
typedef struct {
    unsigned long long uc_flags;
    unsigned long long uc_link;
    haku64_stack_t uc_stack;
    unsigned long long uc_mcontext[20];
    unsigned long long uc_sigmask;
} haku64_ucontext_t;

/* siginfo_t prefix. The kernel writes a 128-byte block: si_signo, si_errno
 * and si_code in the first 12 bytes, the rest zero. This structure names the
 * head of it, which is what a handler reads. */
typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad;
    long long si_value[2];
} haku64_siginfo_t;

#define HAKU64_POLLIN  0x0001
#define HAKU64_POLLOUT 0x0004
#define HAKU64_POLLERR 0x0008
#define HAKU64_POLLHUP 0x0010
#define HAKU64_POLLNVAL 0x0020

/* Linux x86-64 utsname: six 65-byte fields. */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} haku64_utsname_t;

/* --------------------------------------------------------- raw syscalls */
long long haku64_syscall0(long long number);
long long haku64_syscall1(long long number, long long a);
long long haku64_syscall2(long long number, long long a, long long b);
long long haku64_syscall3(long long number, long long a, long long b,
                          long long c);
long long haku64_syscall4(long long number, long long a, long long b,
                          long long c, long long d);
long long haku64_syscall5(long long number, long long a, long long b,
                          long long c, long long d, long long e);
long long haku64_syscall6(long long number, long long a, long long b,
                          long long c, long long d, long long e, long long f);

/* errno is one word per thread. It lives in the thread's control block (the
 * TEB whose GS base this library installs), so two threads never overwrite
 * each other's failure code. haku64_errno_slot is a compiler-intrinsic GS
 * load plus an offset; the macro keeps ordinary `errno = ...` code reading
 * naturally. */
int *haku64_errno_slot(void);
#define errno (*haku64_errno_slot())
/* Return `result`, or -1 with errno set when it is a -errno value. */
long long haku64_errno_check(long long result);
long long haku64_syscall_check6(long long number, long long a, long long b,
                                long long c, long long d, long long e,
                                long long f);

/* ------------------------------------------------------------- string */
void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
void *memset(void *dst, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *block, int value, size_t count);
size_t strlen(const char *text);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t count);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strchr(const char *text, int ch);
char *strrchr(const char *text, int ch);
char *strstr(const char *text, const char *needle);

/* -------------------------------------------------------------- stdio */
int putchar(int ch);
int puts(const char *text);
int printf(const char *format, ...);
int snprintf(char *buffer, size_t capacity, const char *format, ...);

/* ------------------------------------------------------------- malloc */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
int atoi(const char *text);

/* -------------------------------------------------------------- unistd */
ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
int open(const char *path, int flags, ...);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int old_fd, int new_fd);
int dup3(int old_fd, int new_fd, int flags);
/* pipe()/pipe2(): fds[0] reads and fds[1] writes the same ring. pipe2 also
 * takes O_NONBLOCK and O_CLOEXEC. A read of an empty pipe blocks until data
 * or the last writer closes (then it returns 0); a write with no reader fails
 * with EPIPE and raises SIGPIPE, whose default action still ends the
 * process. */
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
/* poll(2): `count` entries of haku64_pollfd_t, `timeout` in milliseconds and
 * -1 to wait for a pipe to change. Returns the number of entries with events
 * or 0 on timeout. */
int poll(haku64_pollfd_t *fds, unsigned int count, int timeout);
/* ioctl(2) on the console: termios and window-size queries, and the
 * foreground process group (TCGETPGRP/TCSETPGRP in libc terms). A
 * descriptor that is not the console fails with ENOTTY. */
int ioctl(int fd, unsigned long long request, void *argument);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int mkdir(const char *path, ...);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
int access(const char *path, int mode);
int getpid(void);
int gettid(void);
int getppid(void);
/* Process groups and sessions. Ids are pids, as in Linux. `setpgid(0, 0)`
 * starts a new group led by the caller; `setsid()` starts a new session. */
int getpgrp(void);
int getpgid(int pid);
int setpgid(int pid, int pgid);
int getsid(int pid);
int setsid(void);
/* kill(2) with the Linux pid selectors: > 0 one process, 0 the caller's
 * process group, -1 every other process, < -1 the group whose id is -pid.
 * Signal 0 only checks that the target exists. The signal is queued and
 * acted on before the target returns to ring 3, so a handler runs with a
 * clean stack. */
int kill(int pid, int signal);

/* Signals. sigaction() fills in the restorer trampoline the kernel needs for
 * a real handler, keeps the previous disposition in `old` when it is not
 * NULL, and returns 0 or -1 with errno. `signal()` is the historical
 * one-argument form; raise() sends the caller's own signal. */
typedef void (*haku64_sighandler_t)(int);
int sigaction(int signal, const haku64_sigaction_t *action,
              haku64_sigaction_t *old);
int sigprocmask(int how, const haku64_sigset_t *set, haku64_sigset_t *old);
int sigaltstack(const haku64_stack_t *stack, haku64_stack_t *old);
haku64_sighandler_t signal(int signal, haku64_sighandler_t handler);
int raise(int signal);

/* The robust futex list a thread registers so its mutexes are handed on when
 * it exits. The layout matches Linux's struct robust_list_head: the first
 * node, the signed byte offset from a node to its futex word, and the node of
 * an operation in progress (unused here). A thread registers one list; the
 * kernel walks it at exit and marks every mutex the thread still owned as
 * owner-died. */
typedef struct haku64_robust_list {
    struct haku64_robust_list *next;
    long long futex_offset;
    struct haku64_robust_list *pending;
} haku64_robust_list_t;
int set_robust_list(haku64_robust_list_t *head, size_t length);
int get_robust_list(int pid, haku64_robust_list_t **head, size_t *length);
/* Set or read the user FS/GS base. `address` is a pointer for GET_* and the
 * new base for SET_*; returns 0 or -1 with errno. */
int arch_prctl(int code, unsigned long long address);
/* crt64.asm calls this before main(); exported so a custom entry point can
 * do the same. Returns 0 on success (or when the image has no TLS). */
int haku64_tls_init(const unsigned long long *auxv);
/* One more TEB and TLS block for a new thread, and their release.
 * haku64_tls_new returns the TEB address (a GS base) or NULL with errno
 * set. Freeing is only safe once no task can reach them, so only a caller
 * that knows the thread is gone should call haku64_tls_free. */
void *haku64_tls_new(void);
void haku64_tls_free(void *teb);
/* Run fn(arg) on a detached thread: same address space, files and cwd, its
 * own stack and its own copy of every __declspec(thread) variable. Returns
 * the new tid, or -1 with errno. Nobody joins it; when the function returns
 * the thread frees its TLS block and stack and the kernel reclaims its task
 * slot, so a long-running program can start detached workers without
 * accumulating mappings. */
int haku64_thread_create(int (*fn)(void *), void *arg);
/* A joinable thread. The handle lives in the thread's own stack mapping, so
 * joining costs no allocation and the handle points into memory that
 * haku64_thread_join() releases: never touch it after a successful join. */
typedef struct haku64_thread {
    int tid;                        /* also what waitpid() wants */
    int done;                       /* CLONE_CHILD_CLEARTID word, 0 once ended */
    void *teb;                      /* the thread's TLS control block */
    unsigned char *stack;           /* base of its stack mapping */
    unsigned long long stack_size;
} haku64_thread_t;
/* Spawn a thread that can be joined, or NULL with errno set. The child gets
 * CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID, so the tid lands in the handle
 * before it runs and is zeroed (with one futex wake) when it ends. */
haku64_thread_t *haku64_thread_spawn(int (*fn)(void *), void *arg);
/* Wait until the thread has ended, reap it, and release its stack and TLS
 * block. Returns its exit status, or -1 with errno; the handle is dangling
 * afterwards. */
int haku64_thread_join(haku64_thread_t *thread);
int fork(void);
int waitpid(int pid, int *status, int options);
int execve(const char *path, char *const argv[], char *const envp[]);
void _exit(int status);
void exit(int status);
void *sbrk(long long increment);
int brk(void *address);
int stat(const char *path, haku64_stat_t *status);
int lstat(const char *path, haku64_stat_t *status);
int fstat(int fd, haku64_stat_t *status);
int nanosleep(const haku64_timespec_t *request,
              haku64_timespec_t *remaining);
int clock_gettime(int clock_id, haku64_timespec_t *value);
unsigned int sleep(unsigned int seconds);
int uname(haku64_utsname_t *name);

/* ---------------------------------------------------------------- mman */
void *mmap(void *address, size_t length, int prot, int flags, int fd,
           long long offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int prot);

/* -------------------------------------------------------------- stdlib */
#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

#endif
