#ifndef HAKU_ABI_H
#define HAKU_ABI_H

/* User/kernel system-call ABI for 32-bit x86.
 *
 * The syscall numbers are the i386 Linux ones (arch/x86/entry/syscalls/
 * syscall_32.tbl), so a Linux-targeted binary compiled for i386 can call the
 * kernel unchanged. Numbers are declared for the calls Haku implements today
 * and for the ones the next milestones add; the ones that are not
 * implemented yet return -ENOSYS.
 */
#define HAKU_SYSCALL_VECTOR 0x80u

/* i386 Linux syscall numbers implemented by Haku. */
#define HAKU_SYS_EXIT    1u
#define HAKU_SYS_FORK    2u
#define HAKU_SYS_READ    3u
#define HAKU_SYS_WRITE   4u
#define HAKU_SYS_OPEN    5u
#define HAKU_SYS_CLOSE   6u
#define HAKU_SYS_WAITPID 7u
#define HAKU_SYS_UNLINK  10u
#define HAKU_SYS_EXECVE  11u
#define HAKU_SYS_LSEEK   19u
/* i386 libc builds (musl among them) seek through _llseek, which takes and
 * returns a 64-bit offset through a user pointer. */
#define HAKU_SYS_LLSEEK  140u
/* Scatter/gather I/O. A libc's stdio writes through writev, so this is part
 * of the surface a foreign libc needs, not an extra. */
#define HAKU_SYS_READV   145u
#define HAKU_SYS_WRITEV  146u
#define HAKU_IOV_MAX     1024u
#define HAKU_SYS_GETPID  20u
/* i386 set*id/get*id: Haku has a single user, so every process runs as
 * uid/gid 0 and the setters are not implemented. */
#define HAKU_SYS_GETUID  24u
#define HAKU_SYS_RENAME  38u
#define HAKU_SYS_MKDIR   39u
#define HAKU_SYS_RMDIR   40u
#define HAKU_SYS_DUP     41u
#define HAKU_SYS_TIMES   43u
#define HAKU_SYS_BRK     45u
#define HAKU_SYS_GETGID  47u
#define HAKU_SYS_GETEUID 49u
#define HAKU_SYS_GETEGID 50u
#define HAKU_SYS_DUP2    63u
#define HAKU_SYS_GETPPID 64u
#define HAKU_SYS_GETRUSAGE 77u
#define HAKU_SYS_GETTIMEOFDAY 78u
/* i386 old_mmap: a single pointer to haku_mmap_arg_t in ebx. */
#define HAKU_SYS_MMAP    90u
#define HAKU_SYS_MUNMAP  91u
/* i386 "newstat" family: the 64-byte struct stat layout. */
#define HAKU_SYS_STAT    106u
#define HAKU_SYS_LSTAT   107u
#define HAKU_SYS_FSTAT   108u
#define HAKU_SYS_WAIT4   114u
#define HAKU_SYS_CLONE   120u
#define HAKU_SYS_UNAME   122u
#define HAKU_SYS_NANOSLEEP    162u
#define HAKU_SYS_VFORK   190u
/* i386 "stat64" family: the 96-byte struct stat64 layout that glibc and
 * musl use on i386 (musl's stat() goes through fstatat64). */
#define HAKU_SYS_STAT64  195u
#define HAKU_SYS_LSTAT64 196u
#define HAKU_SYS_FSTAT64 197u
#define HAKU_SYS_GETTID  224u
#define HAKU_SYS_EXIT_GROUP 252u
#define HAKU_SYS_FSTATAT64 300u
/* The call musl's stat() makes first on i386. */
#define HAKU_SYS_STATX   383u
/* Signals: the rt_* forms are what a modern libc uses on i386. */
#define HAKU_SYS_KILL    37u
#define HAKU_SYS_RT_SIGRETURN 173u
#define HAKU_SYS_RT_SIGACTION 174u
#define HAKU_SYS_RT_SIGPROCMASK 175u
#define HAKU_SYS_TKILL   238u
#define HAKU_SYS_TGKILL  270u

/* i386 Linux syscall numbers reserved for the upcoming milestones: they
 * are listed so the ABI stays a single, Linux-shaped table, but the kernel
 * answers -ENOSYS until the matching work lands. */
#define HAKU_SYS_LINK      9u
#define HAKU_SYS_CHDIR     12u
#define HAKU_SYS_MKNOD     14u
#define HAKU_SYS_CHMOD     15u
#define HAKU_SYS_LCHOWN    16u
#define HAKU_SYS_MOUNT     21u
#define HAKU_SYS_UMOUNT2   52u
#define HAKU_SYS_IOCTL     54u
#define HAKU_SYS_FCNTL     55u
#define HAKU_SYS_SETUID    23u
#define HAKU_SYS_ACCESS    33u
#define HAKU_SYS_PIPE      42u
#define HAKU_SYS_SETGID    46u
#define HAKU_SYS_SETRLIMIT 75u
#define HAKU_SYS_SYMLINK   83u
#define HAKU_SYS_READLINK  85u
#define HAKU_SYS_REBOOT    88u
#define HAKU_SYS_TRUNCATE  92u
#define HAKU_SYS_FTRUNCATE 93u
#define HAKU_SYS_FCHMOD    94u
#define HAKU_SYS_FCHOWN    95u
#define HAKU_SYS_MPROTECT  125u
#define HAKU_SYS_FCHDIR    133u
#define HAKU_SYS_PERSONALITY 136u
#define HAKU_SYS_GETDENTS  141u
#define HAKU_SYS_SELECT    142u
/* i386 names 142 "_newselect" and keeps 82 as the original select. musl's
 * select() uses 82, so both numbers route to the same implementation. */
#define HAKU_SYS_OLDSELECT 82u
#define HAKU_SYS_SCHED_YIELD 158u
#define HAKU_SYS_MREMAP    163u
#define HAKU_SYS_POLL      168u
#define HAKU_SYS_PRCTL     172u
#define HAKU_SYS_CHOWN     182u
#define HAKU_SYS_GETCWD    183u
#define HAKU_SYS_GETRLIMIT 191u
#define HAKU_SYS_MMAP2     192u
#define HAKU_SYS_GETDENTS64 220u
#define HAKU_SYS_FCNTL64   221u
#define HAKU_SYS_MADVISE   219u
#define HAKU_SYS_FUTEX     240u
#define HAKU_SYS_FUTEX_TIME64 422u
#define HAKU_SYS_SET_THREAD_AREA 243u
#define HAKU_SYS_GET_THREAD_AREA 244u
#define HAKU_SYS_EPOLL_CREATE 254u
#define HAKU_SYS_EPOLL_CTL 255u
#define HAKU_SYS_EPOLL_WAIT 256u
#define HAKU_SYS_SET_TID_ADDRESS 258u
#define HAKU_SYS_CLOCK_GETTIME 265u
#define HAKU_SYS_WAITID    284u
#define HAKU_SYS_OPENAT    295u
#define HAKU_SYS_MKDIRAT   296u
#define HAKU_SYS_FCHOWNAT  298u
#define HAKU_SYS_UNLINKAT  301u
#define HAKU_SYS_RENAMEAT  302u
#define HAKU_SYS_READLINKAT 305u
#define HAKU_SYS_FCHMODAT  306u
#define HAKU_SYS_FACCESSAT 307u
#define HAKU_SYS_SET_ROBUST_LIST 311u
#define HAKU_SYS_UTIMENSAT 320u
#define HAKU_SYS_DUP3      330u
#define HAKU_SYS_PIPE2     331u
#define HAKU_SYS_PRLIMIT64 340u
#define HAKU_SYS_GETRANDOM 355u
#define HAKU_SYS_RSEQ      386u

/* Process-creation flags (Linux). Haku implements the fork-like case of
 * clone: a new process with its own copy of the address space and the
 * parent's descriptors. The sharing flags (CLONE_VM and friends) are
 * rejected with EINVAL. */
#define HAKU_SIGCHLD       17u
#define HAKU_CLONE_VM      0x100u
#define HAKU_CLONE_FS      0x200u
#define HAKU_CLONE_FILES   0x400u
#define HAKU_CLONE_SIGHAND 0x800u
#define HAKU_CLONE_VFORK   0x4000u
#define HAKU_CLONE_THREAD  0x10000u
/* wait4(2) options. */
#define HAKU_WNOHANG       0x1u

/* Signals (Linux i386 numbering). Haku implements the standard range
 * 1..31: real-time signals (32..64) are rejected with EINVAL, and the
 * *at variants of the older calls are not provided, because a modern libc
 * uses the rt_* forms below. */
#define HAKU_SIGHUP     1u
#define HAKU_SIGINT     2u
#define HAKU_SIGQUIT    3u
#define HAKU_SIGILL     4u
#define HAKU_SIGTRAP    5u
#define HAKU_SIGABRT    6u
#define HAKU_SIGBUS     7u
#define HAKU_SIGFPE     8u
#define HAKU_SIGKILL    9u
#define HAKU_SIGUSR1    10u
#define HAKU_SIGSEGV    11u
#define HAKU_SIGUSR2    12u
#define HAKU_SIGPIPE    13u
#define HAKU_SIGALRM    14u
#define HAKU_SIGTERM    15u
#define HAKU_SIGSTKFLT  16u
#define HAKU_SIGCONT    18u
#define HAKU_SIGSTOP    19u
#define HAKU_SIGTSTP    20u
#define HAKU_SIGTTIN    21u
#define HAKU_SIGTTOU    22u
#define HAKU_SIGURG     23u
#define HAKU_SIGXCPU    24u
#define HAKU_SIGXFSZ    25u
#define HAKU_SIGVTALRM  26u
#define HAKU_SIGPROF    27u
#define HAKU_SIGWINCH   28u
#define HAKU_SIGIO      29u
#define HAKU_SIGPWR     30u
#define HAKU_SIGSYS     31u
/* rt_sigaction(2) handler values and rt_sigprocmask(2) operations. */
#define HAKU_SIG_DFL    0u
#define HAKU_SIG_IGN    1u
#define HAKU_SIG_BLOCK  0u
#define HAKU_SIG_UNBLOCK 1u
#define HAKU_SIG_SETMASK 2u
/* sigaction flags. Haku honours SIGINFO, NODEFER and RESTORER; the others
 * are stored unchanged and ignored. */
#define HAKU_SA_NOCLDSTOP 0x00000001u
#define HAKU_SA_SIGINFO 0x00000004u
#define HAKU_SA_RESTORER 0x04000000u
#define HAKU_SA_RESTART 0x10000000u
#define HAKU_SA_NODEFER 0x40000000u

/* Haku-specific syscalls for starting a child ELF from a pathname and for
 * reading the calling process's memory/resource usage. They live outside
 * the Linux range so a Linux binary never collides with them. */
#define HAKU_SYS_SPAWN   0x1000u
#define HAKU_SYS_GETUSAGE 0x1001u


/* Raw syscall errors are returned as -errno. */
#define HAKU_EPERM         1u
#define HAKU_E2BIG         7u
#define HAKU_ENOENT        2u
#define HAKU_ESRCH         3u
#define HAKU_EINTR         4u
#define HAKU_EIO           5u
#define HAKU_ENXIO         6u
#define HAKU_ENOEXEC       8u
#define HAKU_EBADF          9u
#define HAKU_ECHILD        10u
#define HAKU_EAGAIN        11u
#define HAKU_ENOMEM        12u
#define HAKU_EACCES        13u
#define HAKU_EFAULT        14u
#define HAKU_EBUSY         16u
#define HAKU_EEXIST        17u
#define HAKU_EXDEV         18u
#define HAKU_ENODEV        19u
#define HAKU_ENOTDIR       20u
#define HAKU_EISDIR        21u
#define HAKU_EINVAL        22u
#define HAKU_ENFILE        23u
#define HAKU_EMFILE        24u
#define HAKU_ENOTTY        25u
#define HAKU_EFBIG         27u
#define HAKU_ENOSPC        28u
#define HAKU_ESPIPE        29u
#define HAKU_EROFS         30u
#define HAKU_EPIPE         32u
#define HAKU_ERANGE        34u
#define HAKU_ENAMETOOLONG  36u
#define HAKU_ENOLCK        37u
#define HAKU_ENOSYS        38u
#define HAKU_ENOTEMPTY     39u
#define HAKU_ELOOP         40u
#define HAKU_EOVERFLOW     75u
#define HAKU_EOPNOTSUPP    95u
#define HAKU_ENOTSUP       HAKU_EOPNOTSUPP
#define HAKU_ETIMEDOUT     110u

/* futex(2) operations (Linux). The command is the operation without the
 * private and clock flags. */
#define HAKU_FUTEX_WAIT           0u
#define HAKU_FUTEX_WAKE           1u
#define HAKU_FUTEX_REQUEUE        3u
#define HAKU_FUTEX_CMP_REQUEUE    4u
#define HAKU_FUTEX_WAKE_OP        5u
#define HAKU_FUTEX_LOCK_PI        6u
#define HAKU_FUTEX_UNLOCK_PI      7u
#define HAKU_FUTEX_TRYLOCK_PI     8u
#define HAKU_FUTEX_WAIT_BITSET    9u
#define HAKU_FUTEX_WAKE_BITSET    10u
#define HAKU_FUTEX_WAIT_REQUEUE_PI 11u
#define HAKU_FUTEX_CMP_REQUEUE_PI 12u
/* The futex belongs to one process instead of a shared mapping. */
#define HAKU_FUTEX_PRIVATE_FLAG   128u
/* The timeout is an absolute CLOCK_REALTIME deadline, not a relative
 * interval. */
#define HAKU_FUTEX_CLOCK_REALTIME 256u
#define HAKU_FUTEX_CMD_MASK \
    (~(uint32_t)(HAKU_FUTEX_PRIVATE_FLAG | HAKU_FUTEX_CLOCK_REALTIME))

/* open(2)/openat(2) flags accepted by the current VFS implementation. The
 * remaining Linux flags are accepted where Haku can honour them: O_CLOEXEC
 * and O_NOCTTY have no effect (one thread per process, no controlling
 * terminal), O_DIRECTORY opens a directory for getdents64, and O_NONBLOCK
 * is remembered by fcntl but does not change console behaviour. */
#define HAKU_O_RDONLY  0x0000u
#define HAKU_O_WRONLY  0x0001u
#define HAKU_O_RDWR    0x0002u
#define HAKU_O_ACCMODE 0x0003u
#define HAKU_O_CREAT   0x0040u
#define HAKU_O_EXCL    0x0080u
#define HAKU_O_NOCTTY  0x0100u
#define HAKU_O_TRUNC   0x0200u
#define HAKU_O_APPEND  0x0400u
#define HAKU_O_NONBLOCK 0x0800u
/* Set by a 32-bit libc on every open(2) call to ask for a 64-bit file
 * offset. Haku's offsets are already 32-bit and a file cannot be larger
 * than the disk image, so the flag is accepted and ignored; rejecting it
 * made musl's opendir(), which passes it, fail with EINVAL. */
#define HAKU_O_LARGEFILE 0x8000u
#define HAKU_O_DIRECTORY 0x10000u
#define HAKU_O_NOFOLLOW 0x20000u
#define HAKU_O_CLOEXEC  0x80000u
/* The flags the VFS itself understands (the rest are handled in the
 * syscall layer). */
#define HAKU_O_VFS_MASK (HAKU_O_ACCMODE | HAKU_O_CREAT | HAKU_O_EXCL | \
                         HAKU_O_TRUNC | HAKU_O_APPEND | HAKU_O_DIRECTORY)

/* access(2) modes and the *at flags. */
#define HAKU_F_OK 0u
#define HAKU_X_OK 1u
#define HAKU_W_OK 2u
#define HAKU_R_OK 4u
#define HAKU_AT_REMOVEDIR 0x200u

/* fcntl(2) commands and the descriptor flags. */
#define HAKU_F_DUPFD          0u
#define HAKU_F_GETFD          1u
#define HAKU_F_SETFD          2u
#define HAKU_F_GETFL          3u
#define HAKU_F_SETFL          4u
#define HAKU_F_GETLK          5u
#define HAKU_F_SETLK          6u
#define HAKU_F_SETLKW         7u
#define HAKU_F_DUPFD_CLOEXEC  1030u
#define HAKU_FD_CLOEXEC       1u
#define HAKU_F_RDLCK          0u
#define HAKU_F_WRLCK          1u
#define HAKU_F_UNLCK          2u

/* getrlimit(2)/setrlimit(2). Haku reports fixed limits: the descriptor
 * table is 8 entries, the process table 4 tasks, and the address space is
 * bounded only by physical memory. */
#define HAKU_RLIMIT_CPU     0u
#define HAKU_RLIMIT_FSIZE   1u
#define HAKU_RLIMIT_DATA    2u
#define HAKU_RLIMIT_STACK   3u
#define HAKU_RLIMIT_CORE    4u
#define HAKU_RLIMIT_RSS     5u
#define HAKU_RLIMIT_NPROC   6u
#define HAKU_RLIMIT_NOFILE  7u
#define HAKU_RLIMIT_MEMLOCK 8u
#define HAKU_RLIMIT_AS      9u
#define HAKU_RLIMIT_LOCKS   10u
#define HAKU_RLIMIT_SIGPENDING 11u
#define HAKU_RLIMIT_MSGQUEUE 12u
#define HAKU_RLIMIT_NICE    13u
#define HAKU_RLIMIT_RTPRIO  14u
#define HAKU_RLIMIT_RTTIME  15u
#define HAKU_RLIMIT_NLIMITS 16u
#define HAKU_RLIM_INFINITY  0xFFFFFFFFu
#define HAKU_RLIMIT_SIZE    8u

/* getrandom(2) flags. */
#define HAKU_GRND_NONBLOCK 0x1u
#define HAKU_GRND_RANDOM   0x2u

/* poll(2) events. */
#define HAKU_POLLIN     0x001u
#define HAKU_POLLOUT    0x004u
#define HAKU_POLLERR    0x008u
#define HAKU_POLLHUP    0x010u
#define HAKU_POLLNVAL   0x020u
#define HAKU_POLLFD_SIZE 8u
#define HAKU_FD_SETSIZE 64u

/* prctl(2) options Haku accepts. Process names are stored per task; the
 * remaining options are accepted as no-ops because their Linux effects
 * (dumpability, parent-death signals, privilege escalation guards) do not
 * exist in a single-user kernel. */
#define HAKU_PR_SET_PDEATHSIG   1u
#define HAKU_PR_GET_PDEATHSIG   2u
#define HAKU_PR_GET_DUMPABLE    3u
#define HAKU_PR_SET_DUMPABLE    4u
#define HAKU_PR_SET_NAME        15u
#define HAKU_PR_GET_NAME        16u
#define HAKU_PR_SET_SECCOMP     22u
#define HAKU_PR_CAPBSET_READ    23u
#define HAKU_PR_SET_NO_NEW_PRIVS 38u
#define HAKU_PR_GET_NO_NEW_PRIVS 39u
#define HAKU_PR_SET_CHILD_SUBREAPER 36u
#define HAKU_PR_GET_CHILD_SUBREAPER 37u
#define HAKU_PR_SET_VMA          0x53564d41u

/* personality(2). Haku is always PER_LINUX. */
#define HAKU_PER_LINUX        0x0000000u
#define HAKU_PER_LINUX_32BIT  0x0008000u
#define HAKU_PER_MASK         0x00000FFu
#define HAKU_PER_QUERY        0xFFFFFFFFu
#define HAKU_ADDR_NO_RANDOMIZE 0x0040000u

#define HAKU_STDIN_FILENO  0u
#define HAKU_STDOUT_FILENO 1u
#define HAKU_STDERR_FILENO 2u

#define HAKU_S_IFREG       0x8000u
#define HAKU_S_IFDIR       0x4000u
#define HAKU_S_IFCHR       0x2000u
#define HAKU_S_IFBLK       0x6000u
#define HAKU_S_IFIFO       0x1000u
#define HAKU_S_IFLNK       0xA000u
#define HAKU_S_IFSOCK      0xC000u
#define HAKU_S_IFMT        0xF000u
#define HAKU_S_ISREG(m)    (((m) & HAKU_S_IFMT) == HAKU_S_IFREG)
#define HAKU_S_ISDIR(m)    (((m) & HAKU_S_IFMT) == HAKU_S_IFDIR)
/* Default permissions Haku reports for FAT12 entries. */
#define HAKU_STAT_MODE_REG 0x81A4u /* S_IFREG | 0644 */
#define HAKU_STAT_MODE_DIR 0x41EDu /* S_IFDIR | 0755 */
/* S_IFREG | 0755, for a filesystem that stores no permission bits at all.
 * A shell will not run a program it finds on $PATH unless the file has an
 * execute bit, so a FAT volume has to report one: the format cannot express
 * permissions, and a vfat mount with mode=0755 behaves the same way. */
#define HAKU_STAT_MODE_FILE_EXEC 0x81EDu
#define HAKU_STAT_SIZE     64u
#define HAKU_STAT64_SIZE   96u
#define HAKU_STAT_BLKSIZE  512u
#define HAKU_RUSAGE_SIZE   72u
#define HAKU_TMS_SIZE      16u
#define HAKU_UTSNAME_SIZE  390u
/* AT_FDCWD is the "relative to the current directory" dirfd value. */
#define HAKU_AT_FDCWD      (-100)
#define HAKU_AT_SYMLINK_NOFOLLOW 0x100u
#define HAKU_AT_EMPTY_PATH 0x1000u
#define HAKU_AT_NO_AUTOMOUNT 0x800u
/* Auxiliary vector types (the i386 Linux numbers). The kernel places these
 * as (type, value) pairs after the environment on the initial stack, so a
 * libc can read the page size, the canary seed, and the identity words
 * before main runs. */
#define HAKU_AT_NULL    0u
#define HAKU_AT_PHDR    3u
#define HAKU_AT_PHENT   4u
#define HAKU_AT_PHNUM   5u
#define HAKU_AT_PAGESZ  6u
#define HAKU_AT_BASE    7u
#define HAKU_AT_FLAGS   8u
#define HAKU_AT_ENTRY   9u
#define HAKU_AT_UID     11u
#define HAKU_AT_EUID    12u
#define HAKU_AT_GID     13u
#define HAKU_AT_EGID    14u
#define HAKU_AT_HWCAP   16u
#define HAKU_AT_CLKTCK  17u
#define HAKU_AT_SECURE  23u
#define HAKU_AT_RANDOM  25u
#define HAKU_AT_RANDOM_BYTES 16u
/* statx(2) mask bits. */
#define HAKU_STATX_TYPE     0x001u
#define HAKU_STATX_MODE     0x002u
#define HAKU_STATX_NLINK    0x004u
#define HAKU_STATX_UID      0x008u
#define HAKU_STATX_GID      0x010u
#define HAKU_STATX_ATIME    0x020u
#define HAKU_STATX_MTIME    0x040u
#define HAKU_STATX_CTIME    0x080u
#define HAKU_STATX_INO      0x100u
#define HAKU_STATX_SIZE     0x200u
#define HAKU_STATX_BLOCKS   0x400u
#define HAKU_STATX_BASIC    0x7FFu

/* Time conversion factors and the fixed-width user structs used by
 * gettimeofday(2) and nanosleep(2). Values are 32-bit on this ABI. */
#define HAKU_USEC_PER_SEC  1000000u
#define HAKU_NSEC_PER_SEC  1000000000u
#define HAKU_TIMEVAL_SIZE  8u
#define HAKU_TIMESPEC_SIZE 8u

/* mmap(2) protection and mapping flags. Only anonymous private mappings
 * are supported today; the values match Linux. PROT_WRITE controls the
 * PTE R/W bit, so a PROT_READ mapping is enforced read-only. PROT_NONE
 * reservations are rejected with EINVAL. */
#define HAKU_PROT_NONE  0x0u
#define HAKU_PROT_READ  0x1u
#define HAKU_PROT_WRITE 0x2u
#define HAKU_PROT_EXEC  0x4u
#define HAKU_MAP_SHARED    0x01u
#define HAKU_MAP_PRIVATE   0x02u
#define HAKU_MAP_FIXED     0x10u
#define HAKU_MAP_ANONYMOUS 0x20u
/* mremap(2) flags. */
#define HAKU_MREMAP_MAYMOVE 0x1u
#define HAKU_MREMAP_FIXED   0x2u
/* madvise(2) advice values. */
#define HAKU_MADV_NORMAL     0u
#define HAKU_MADV_RANDOM     1u
#define HAKU_MADV_SEQUENTIAL 2u
#define HAKU_MADV_WILLNEED   3u
#define HAKU_MADV_DONTNEED   4u
#define HAKU_MADV_FREE       5u
#define HAKU_MMAP_ARG_SIZE 24u

typedef unsigned int haku_u32;
typedef unsigned short haku_u16;
typedef unsigned char haku_u8;
typedef signed int haku_i32;
typedef unsigned long long haku_u64;
typedef long long haku_i64;

/* The thread area descriptor of set_thread_area(243): the i386
 * "struct user_desc" (16 bytes). base_addr is the thread pointer; the
 * kernel writes the descriptor entry it used back into entry_number and
 * returns the same number, so a libc can load entry*8+3 into %gs. */
typedef struct {
    haku_u32 entry_number;
    haku_u32 base_addr;
    haku_u32 limit;
    haku_u32 seg_32bit : 1;
    haku_u32 contents : 2;
    haku_u32 read_exec_only : 1;
    haku_u32 limit_in_pages : 1;
    haku_u32 seg_not_present : 1;
    haku_u32 useable : 1;
    haku_u32 reserved : 25;
} haku_thread_area_t;

/* i386 Linux "struct stat" (arch/x86/include/uapi/asm/stat.h): the layout
 * returned by stat(106), lstat(107), and fstat(108). 64 bytes. */
typedef struct {
    haku_u32 st_dev;
    haku_u32 st_ino;
    haku_u16 st_mode;
    haku_u16 st_nlink;
    haku_u16 st_uid;
    haku_u16 st_gid;
    haku_u32 st_rdev;
    haku_u32 st_size;
    haku_u32 st_blksize;
    haku_u32 st_blocks;
    haku_u32 st_atime;
    haku_u32 st_atime_nsec;
    haku_u32 st_mtime;
    haku_u32 st_mtime_nsec;
    haku_u32 st_ctime;
    haku_u32 st_ctime_nsec;
    haku_u32 __unused4;
    haku_u32 __unused5;
} haku_stat_t;

/* i386 Linux "struct stat64" (uapi/asm/stat.h): the glibc2.1-compatible
 * layout returned by stat64(195), lstat64(196), fstat64(197), and
 * fstatat64(300). This is what glibc and musl expect on i386, so it is the
 * layout the user library uses. The 4-byte packing reproduces the i386
 * System V ABI, where long long members are only 4-byte aligned. */
#pragma pack(push, 4)
typedef struct {
    haku_u64 st_dev;
    haku_u8  __pad0[4];
    haku_u32 __st_ino;
    haku_u32 st_mode;
    haku_u32 st_nlink;
    haku_u32 st_uid;
    haku_u32 st_gid;
    haku_u64 st_rdev;
    haku_u8  __pad3[4];
    haku_i64 st_size;
    haku_u32 st_blksize;
    haku_u64 st_blocks;
    haku_u32 st_atime;
    haku_u32 st_atime_nsec;
    haku_u32 st_mtime;
    haku_u32 st_mtime_nsec;
    haku_u32 st_ctime;
    haku_u32 st_ctime_nsec;
    haku_u64 st_ino;
} haku_stat64_t;
#pragma pack(pop)

/* gettimeofday(2): seconds + microseconds since boot. */
typedef struct {
    haku_u32 tv_sec;
    haku_u32 tv_usec;
} haku_timeval_t;

/* nanosleep(2): requested/completed duration in seconds + nanoseconds. */
typedef struct {
    haku_u32 tv_sec;
    haku_u32 tv_nsec;
} haku_timespec_t;

/* rt_sigaction(2) argument layout on i386: handler, flags, restorer, then
 * the signal set to block while the handler runs. The kernel sigset_t is
 * 64 bits there; Haku uses the low 32 bits (signals 1..31) and ignores the
 * rest. A restorer is only used when SA_RESTORER is set: otherwise the
 * kernel plants its own stub in the signal frame. */
typedef struct {
    haku_u32 handler;
    haku_u32 flags;
    haku_u32 restorer;
    haku_u32 mask;
    haku_u32 mask_high;
} haku_sigaction_t;

/* mmap(2) argument block (i386 old_mmap ABI). */
typedef struct {
    haku_u32 addr;
    haku_u32 length;
    haku_u32 prot;
    haku_u32 flags;
    haku_u32 fd;
    haku_u32 offset;
} haku_mmap_arg_t;

/* getusage(0x1001) snapshot of the calling process. Ticks are 100 Hz timer
 * ticks; pages are 4 KiB user pages, page tables, and kernel-stack pages.
 * resident_bytes covers mapped user pages only, kernel_bytes covers the
 * user page tables plus the directory and kernel stack, and total_bytes is
 * their sum. peak_* are process-lifetime high-water marks. */
#define HAKU_USAGE_SIZE 64u

typedef struct {
    haku_u32 pid;                 /*  0 */
    haku_u32 user_ticks;          /*  4 */
    haku_u32 kernel_ticks;        /*  8 */
    haku_u32 syscalls;            /* 12 */
    haku_u32 voluntary_switches;  /* 16 */
    haku_u32 involuntary_switches; /* 20 */
    haku_u32 mapped_pages;        /* 24 */
    haku_u32 peak_pages;          /* 28 */
    haku_u32 page_tables;         /* 32 */
    haku_u32 kernel_stack_pages;  /* 36 */
    haku_u32 heap_bytes;          /* 40 */
    haku_u32 mmap_bytes;          /* 44 */
    haku_u32 resident_bytes;      /* 48 */
    haku_u32 kernel_bytes;        /* 52 */
    haku_u32 total_bytes;         /* 56 */
    haku_u32 peak_bytes;          /* 60 */
} haku_usage_t;

/* i386 Linux "struct statx" (include/uapi/linux/stat.h): the modern
 * metadata call. musl uses statx(383) for stat() on i386 and falls back
 * to the stat64 family when the kernel does not provide it. 256 bytes. */
typedef struct {
    haku_i64 tv_sec;
    haku_u32 tv_nsec;
    haku_i32 __reserved;
} haku_statx_timestamp_t;

#pragma pack(push, 4)
typedef struct {
    haku_u32 stx_mask;
    haku_u32 stx_blksize;
    haku_u64 stx_attributes;
    haku_u32 stx_nlink;
    haku_u32 stx_uid;
    haku_u32 stx_gid;
    haku_u16 stx_mode;
    haku_u16 __pad0[1];
    haku_u64 stx_ino;
    haku_u64 stx_size;
    haku_u64 stx_blocks;
    haku_u64 stx_attributes_mask;
    haku_statx_timestamp_t stx_atime;
    haku_statx_timestamp_t stx_btime;
    haku_statx_timestamp_t stx_ctime;
    haku_statx_timestamp_t stx_mtime;
    haku_u32 stx_rdev_major;
    haku_u32 stx_rdev_minor;
    haku_u32 stx_dev_major;
    haku_u32 stx_dev_minor;
    haku_u64 stx_mnt_id;
    haku_u32 stx_dio_mem_align;
    haku_u32 stx_dio_offset_align;
    haku_u64 stx_subvol;
    haku_u32 stx_atomic_write_unit_min;
    haku_u32 stx_atomic_write_unit_max;
    haku_u32 stx_atomic_write_segments_max;
    haku_u32 __pad1[1];
    haku_u64 __pad2[9];
} haku_statx_t;
#pragma pack(pop)

/* i386 Linux "struct rusage" (80 bytes) and "struct tms" (16 bytes):
 * the accounting blocks wait4(2) and getrusage(2) fill in. */
typedef struct {
    haku_u32 ru_utime_sec;
    haku_u32 ru_utime_usec;
    haku_u32 ru_stime_sec;
    haku_u32 ru_stime_usec;
    haku_i32 ru_maxrss;
    haku_i32 ru_ixrss;
    haku_i32 ru_idrss;
    haku_i32 ru_isrss;
    haku_i32 ru_minflt;
    haku_i32 ru_majflt;
    haku_i32 ru_nswap;
    haku_i32 ru_inblock;
    haku_i32 ru_oublock;
    haku_i32 ru_msgsnd;
    haku_i32 ru_msgrcv;
    haku_i32 ru_nsignals;
    haku_i32 ru_nvcsw;
    haku_i32 ru_nivcsw;
} haku_rusage_t;

typedef struct {
    haku_u32 tms_utime;
    haku_u32 tms_stime;
    haku_u32 tms_cutime;
    haku_u32 tms_cstime;
} haku_tms_t;

/* i386 Linux "struct utsname": six 65-byte fields (390 bytes). */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} haku_utsname_t;

/* getrlimit(2)/setrlimit(2): two 32-bit limits. prlimit64(340) uses the
 * 64-bit form below. */
typedef struct {
    haku_u32 rlim_cur;
    haku_u32 rlim_max;
} haku_rlimit_t;

typedef struct {
    haku_u64 rlim_cur;
    haku_u64 rlim_max;
} haku_rlimit64_t;

/* poll(2): one entry per descriptor of interest. */
typedef struct {
    haku_i32 fd;
    haku_u16 events;
    haku_u16 revents;
} haku_pollfd_t;

/* Linux i386 "struct termios" (36 bytes) and "struct winsize" (8 bytes).
 * The console answers TCGETS/TCSETS* and TIOCGWINSZ. */
typedef struct {
    haku_u32 c_iflag;
    haku_u32 c_oflag;
    haku_u32 c_cflag;
    haku_u32 c_lflag;
    haku_u8  c_line;
    haku_u8  c_cc[19];
} haku_termios_t;

typedef struct {
    haku_u16 ws_row;
    haku_u16 ws_col;
    haku_u16 ws_xpixel;
    haku_u16 ws_ypixel;
} haku_winsize_t;

#define HAKU_TERMIOS_SIZE 36u
#define HAKU_WINSIZE_SIZE 8u

/* ioctl(2) requests for the console. */
#define HAKU_TCGETS     0x5401u
#define HAKU_TCSETS     0x5402u
#define HAKU_TCSETSW    0x5403u
#define HAKU_TCSETSF    0x5404u
#define HAKU_TCSBRK     0x5409u
#define HAKU_TCFLSH     0x540Bu
#define HAKU_TIOCGWINSZ 0x5413u
#define HAKU_FIONREAD   0x541Bu

/* termios flag bits Haku honours (the rest are stored and reported back
 * unchanged). */
#define HAKU_IGNBRK  0x0001u
#define HAKU_BRKINT  0x0002u
#define HAKU_IGNPAR  0x0004u
#define HAKU_PARMRK  0x0008u
#define HAKU_INPCK   0x0010u
#define HAKU_ISTRIP  0x0020u
#define HAKU_INLCR   0x0040u
#define HAKU_IGNCR   0x0080u
#define HAKU_ICRNL   0x0100u
#define HAKU_IXON    0x0400u
#define HAKU_OPOST   0x0001u
#define HAKU_ONLCR   0x0004u
#define HAKU_ISIG    0x0001u
#define HAKU_ICANON  0x0002u
#define HAKU_ECHO    0x0008u
#define HAKU_ECHOE   0x0010u
#define HAKU_ECHOK   0x0020u
#define HAKU_ECHONL  0x0040u

/* c_cc[] indexes. */
#define HAKU_VINTR   0u
#define HAKU_VQUIT   1u
#define HAKU_VERASE  2u
#define HAKU_VKILL   3u
#define HAKU_VEOF    4u
#define HAKU_VTIME   5u
#define HAKU_VMIN    6u
#define HAKU_VSTART  7u
#define HAKU_VSTOP   8u
#define HAKU_VSUSP   9u
#define HAKU_VEOL    10u
#define HAKU_VREPRINT 12u
#define HAKU_VDISCARD 13u
#define HAKU_VWERASE  14u
#define HAKU_VLNEXT   15u

/* getdents64(2) record: the Linux i386 layout, where the inode and offset
 * are 64-bit but the whole record is 8-byte aligned. */
#define HAKU_DIRENT64_HEADER 19u
#define HAKU_DT_UNKNOWN 0u
#define HAKU_DT_FIFO 1u
#define HAKU_DT_CHR  2u
#define HAKU_DT_DIR  4u
#define HAKU_DT_BLK  6u
#define HAKU_DT_REG  8u
#define HAKU_DT_LNK  10u
#define HAKU_DT_SOCK 12u

typedef char haku_u32_must_be_32_bits[(sizeof(haku_u32) == 4u) ? 1 : -1];
typedef char haku_u64_must_be_64_bits[(sizeof(haku_u64) == 8u) ? 1 : -1];
typedef char haku_stat_must_be_64_bytes[
    (sizeof(haku_stat_t) == 64u) ? 1 : -1];
typedef char haku_stat64_must_be_96_bytes[
    (sizeof(haku_stat64_t) == 96u) ? 1 : -1];
typedef char haku_statx_must_be_256_bytes[
    (sizeof(haku_statx_t) == 256u) ? 1 : -1];
typedef char haku_rusage_must_be_72_bytes[
    (sizeof(haku_rusage_t) == 72u) ? 1 : -1];
typedef char haku_tms_must_be_16_bytes[
    (sizeof(haku_tms_t) == 16u) ? 1 : -1];
typedef char haku_utsname_must_be_390_bytes[
    (sizeof(haku_utsname_t) == 390u) ? 1 : -1];
typedef char haku_stat_mode_must_be_16_bits[
    (sizeof(((haku_stat_t *)0)->st_mode) == 2u) ? 1 : -1];
typedef char haku_timeval_must_be_8_bytes[
    (sizeof(haku_timeval_t) == HAKU_TIMEVAL_SIZE) ? 1 : -1];
typedef char haku_timespec_must_be_8_bytes[
    (sizeof(haku_timespec_t) == HAKU_TIMESPEC_SIZE) ? 1 : -1];
typedef char haku_mmap_arg_must_be_24_bytes[
    (sizeof(haku_mmap_arg_t) == HAKU_MMAP_ARG_SIZE) ? 1 : -1];
typedef char haku_usage_must_be_64_bytes[
    (sizeof(haku_usage_t) == HAKU_USAGE_SIZE) ? 1 : -1];
typedef char haku_rlimit_must_be_8_bytes[
    (sizeof(haku_rlimit_t) == HAKU_RLIMIT_SIZE) ? 1 : -1];
typedef char haku_termios_must_be_36_bytes[
    (sizeof(haku_termios_t) == HAKU_TERMIOS_SIZE) ? 1 : -1];
typedef char haku_winsize_must_be_8_bytes[
    (sizeof(haku_winsize_t) == HAKU_WINSIZE_SIZE) ? 1 : -1];
typedef char haku_pollfd_must_be_8_bytes[
    (sizeof(haku_pollfd_t) == HAKU_POLLFD_SIZE) ? 1 : -1];

#endif
