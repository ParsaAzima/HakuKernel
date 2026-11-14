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
#define HAKU64_SYS_IOCTL         16
#define HAKU64_SYS_PREAD64       17
#define HAKU64_SYS_PWRITE64      18
#define HAKU64_SYS_WRITEV        20
#define HAKU64_SYS_ACCESS        21
#define HAKU64_SYS_DUP           32
#define HAKU64_SYS_DUP2          33
#define HAKU64_SYS_NANOSLEEP     35
#define HAKU64_SYS_GETPID        39
#define HAKU64_SYS_FORK          57
#define HAKU64_SYS_EXECVE        59
#define HAKU64_SYS_EXIT          60
#define HAKU64_SYS_WAIT4         61
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
#define HAKU64_SYS_GETPPID       110
#define HAKU64_SYS_ARCH_PRCTL    158
#define HAKU64_SYS_GETTID        186
#define HAKU64_SYS_GETDENTS64    217
#define HAKU64_SYS_CLOCK_GETTIME 228
#define HAKU64_SYS_CLOCK_GETRES  229
#define HAKU64_SYS_EXIT_GROUP    231
#define HAKU64_SYS_OPENAT        257
#define HAKU64_SYS_MKDIRAT       258
#define HAKU64_SYS_NEWFSTATAT    262
#define HAKU64_SYS_UNLINKAT      263
#define HAKU64_SYS_RENAMEAT      264
#define HAKU64_SYS_DUP3          292
#define HAKU64_SYS_GETRANDOM     318
#define HAKU64_SYS_STATX         332

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
#define HAKU64_ENOSPC  28
#define HAKU64_ESPIPE  29
#define HAKU64_ERANGE  34
#define HAKU64_ENAMETOOLONG 36
#define HAKU64_ENOSYS  38
#define HAKU64_ENOTEMPTY 39

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

extern int errno;
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
/* Set or read the user FS/GS base. `address` is a pointer for GET_* and the
 * new base for SET_*; returns 0 or -1 with errno. */
int arch_prctl(int code, unsigned long long address);
/* crt64.asm calls this before main(); exported so a custom entry point can
 * do the same. Returns 0 on success (or when the image has no TLS). */
int haku64_tls_init(const unsigned long long *auxv);
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
