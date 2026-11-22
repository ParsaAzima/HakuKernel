/* Shared declarations for the Haku x86-64 kernel.
 *
 * The 64-bit side is built as a separate image (haku64.img) by
 * `python build.py x64`, so it has its own small set of types instead of
 * borrowing the i386 headers: a pointer is 64 bits here, and the 32-bit
 * code's uint32_t arithmetic would silently truncate physical addresses.
 *
 * Everything is freestanding. MSVC x64 has no inline assembly, so the few
 * instructions the kernel needs (port I/O, invlpg) live in entry64.asm and
 * are declared here.
 */
#ifndef HAKU64_H
#define HAKU64_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef signed long long s64;

#define K64_PAGE_SIZE 4096ull
#define K64_PAGE_MASK (~(K64_PAGE_SIZE - 1ull))
/* Physical address bits 12..51. Leaf entries may also carry the NX bit
 * (63), so every walk that turns an entry into a frame address masks with
 * this, never with K64_PAGE_MASK. */
#define K64_PHYS_MASK 0x000FFFFFFFFFF000ull
/* EFER.NXE makes bit 63 of a leaf a no-execute flag instead of a reserved
 * bit. paging64_map_space sets it on data, stack and non-executable mmap
 * pages; executable segments clear it. */
#define K64_PAGE_NX 0x8000000000000000ull

/* Where the supervisor map of all physical memory starts. One PML4 slot
 * covers 512 GiB, which is more than the frame allocator will ever track.
 * The map covers up to K64_PHYS_MAP_MAX of RAM; beyond that the frames are
 * left out of the pool rather than aliased. */
#define K64_PHYS_MAP_BASE 0xFFFF800000000000ull
#define K64_PHYS_MAP_MAX  (64ull * 1024ull * 1024ull * 1024ull)

/* User address space. PML4 slot 1 is kept out of the kernel maps on
 * purpose: a process owns that slot and nothing else, so creating a new
 * address space is copying the two kernel slots and zeroing this one.
 * 0x0000008000000000 is the start of PML4 entry 1 and sits in the lower
 * canonical half, which is the only half ring 3 can reach. */
#define K64_USER_BASE 0x0000008000000000ull
#define K64_USER_END  0x0000010000000000ull

/* User layout used by the ELF64 loader. Images load above
 * K64_USER_IMAGE_MIN (the first 64 KiB of the slot stay unmapped, so a NULL
 * dereference is a fault rather than a valid read). The stack grows down
 * from the top of the slot; the highest page is left unmapped, and the
 * space between the stack and the image is unmapped too, so a runaway stack
 * faults instead of overwriting code. */
#define K64_USER_IMAGE_MIN   (K64_USER_BASE + 0x10000ull)
#define K64_USER_STACK_TOP   (K64_USER_END - K64_PAGE_SIZE)
#define K64_USER_STACK_PAGES 4u
#define K64_USER_STACK_BOTTOM \
    (K64_USER_STACK_TOP - (u64)K64_USER_STACK_PAGES * K64_PAGE_SIZE)

/* Anonymous mappings grow up from just above the image window; the top of
 * the window is the stack guard, so a wild length cannot run into it. The
 * per-task bump pointer lives in task64.c, and the page cap is what keeps
 * one call from swallowing the whole address space. */
#define K64_USER_MMAP_BASE      (K64_USER_BASE + 0x10000000ull)
#define K64_USER_MMAP_END       K64_USER_STACK_BOTTOM
#define K64_USER_MMAP_PAGES_MAX 256u

/* The program break gets its own window between the image and the mmap
 * window, so growing the heap never collides with an anonymous mapping. It
 * starts page aligned at K64_USER_BRK_BASE and stops at the mmap base. */
#define K64_USER_BRK_BASE       (K64_USER_BASE + 0x08000000ull)
#define K64_USER_BRK_LIMIT      K64_USER_MMAP_BASE
#define K64_USER_BRK_PAGES_MAX  4096u

/* Physical layout this stage reserves (all identity mapped, so a plain
 * pointer works):
 *   0x000000-0x200000  firmware, stage1/stage2, E820 and VBE handover,
 *                      kernel image at 0x10000, boot page tables
 *   0x200000-0x1000000 boot allocations (page tables, frame bitmaps)
 *   0x1000000-0x1800000 kernel heap (8 MiB)
 * A machine therefore needs at least 32 MiB; the x64 image asks QEMU for
 * 512 MiB by default. */
#define K64_BOOT_ALLOC_START 0x00200000ull
#define K64_BOOT_ALLOC_END   0x01000000ull
#define K64_HEAP_START       0x01000000ull
#define K64_HEAP_SIZE        0x00800000ull
#define K64_RESERVED_END     (K64_HEAP_START + K64_HEAP_SIZE)

/* ------------------------------------------------------------ device windows
 * Three kernel virtual windows outside the physmap, each with its own PML4
 * slot so a 4 KiB mapping never touches the 2 MiB pages the physmap uses:
 *
 *   K64_LAPIC_VIRT (slot 448)  the local APIC MMIO page, uncached. The
 *                             mapping is copied into every per-process PML4,
 *                             because an interrupt handler or an IPI send
 *                             must reach the unit whatever CR3 is loaded.
 *   K64_ACPI_WINDOW (slot 384) two pages used to read ACPI tables at boot.
 *                             Never copied into a user space.
 *   K64_FB_VIRT (slot 496)    the VBE linear framebuffer, copied into every
 *                             PML4 for the same reason as the APIC: the
 *                             console keeps drawing while a task runs.
 *
 * All three sit in the upper canonical half, next to the physmap (slot 256). */
#define K64_LAPIC_PHYS   0xFEE00000ull
#define K64_LAPIC_VIRT   0xFFFFE00000000000ull
#define K64_LAPIC_SLOT   448u
#define K64_ACPI_WINDOW  0xFFFFC00000000000ull
#define K64_ACPI_WINDOW_PAGES 2u
#define K64_FB_VIRT      0xFFFFF80000000000ull
#define K64_FB_SLOT      496u
#define K64_FB_WINDOW_BYTES (4ull * 1024ull * 1024ull)

/* E820 handover from stage2 (24-byte entries, count at 0x6FFC). */
#define K64_E820_COUNT (*(volatile u32 *)0x6FFC)
#define K64_E820_MAP   0x7000ull
#define K64_E820_MAX   24u
#define K64_E820_RAM   1u

/* ---------------------------------------------------------------- segments
 * The GDT gdt64.c installs. Indices 1 and 2 stay kernel code and data with
 * the values entry64.asm's boot table used, because gdt64_load replaces the
 * table without reloading CS and the cached descriptor must keep matching.
 *
 * Indices 3 and 4 are user *data* then user *code*, which is the order
 * SYSRET needs: it returns to CS = STAR[63:48] + 16, SS = STAR[63:48] + 8.
 * With the STAR base at 0x10 that gives SS 0x18 (index 3) and CS 0x20
 * (index 4), while SYSCALL enters at CS = STAR[47:32] = 0x08 with SS 0x10. */
#define K64_KERNEL_CS 0x08u
#define K64_KERNEL_DS 0x10u
#define K64_USER_DS   0x18u
#define K64_USER_CS   0x20u
#define K64_TSS_SEL   0x28u
#define K64_STAR_BASE 0x10u

/* --------------------------------------------------------------- interrupts
 * Vectors 0..31 are CPU exceptions, 32..47 the remapped 8259 IRQs, 0x80 the
 * ring-3 system call gate, and everything else lands on isr64_default. Gates
 * are 64-bit interrupt gates (type 0xE), so IF is cleared on entry and
 * handlers never nest. */
#define K64_IDT_ENTRIES 256u
#define K64_GATE_INT    0x8Eu  /* present, DPL 0, 64-bit interrupt gate */
#define K64_GATE_USER   0xEEu  /* present, DPL 3, 64-bit interrupt gate */
#define K64_IRQ_BASE    0x20u
#define K64_IRQ_COUNT   16u
#define K64_SYSCALL_VECTOR 0x80u

/* The three local-APIC vectors the per-CPU scheduler uses. They sit above
 * the 8259's remapped 0x20..0x2F block so the PIC wiring is untouched. */
#define K64_VEC_LAPIC_TIMER 0x40u
#define K64_VEC_RESCHED     0x41u
#define K64_VEC_TLB         0x42u

/* Not a vector: a marker the SYSCALL stub looks for after signal delivery.
 * SYSRET takes the return RIP from RCX and the flags from R11, which would
 * destroy the handler's first argument (RCX, per the Microsoft x64 ABI) and
 * the restored R11 of an rt_sigreturn frame. A frame carrying this value goes
 * back through IRETQ, which restores every register from the frame itself.
 * Real vectors are 0..66 and 0xFF, so 0x1FF cannot be mistaken for one. */
#define K64_VEC_SIGRET      0x1FFu

/* Model-specific registers reached through read_msr64/write_msr64. GS.base
 * holds the per-CPU block while the kernel runs and the task's user GS base
 * in ring 3, so a SWAPGS on every privilege transition is what makes
 * gs:[offset] mean "this CPU" in the kernel and "this thread's TEB" in user
 * space (see percpu64.c and task64.c). FS has no kernel use: its base is
 * simply the task's user FS base and is reloaded on every switch. */
#define K64_MSR_FS_BASE        0xC0000100u
#define K64_MSR_GS_BASE        0xC0000101u
#define K64_MSR_KERNEL_GS_BASE 0xC0000102u

/* System calls enter through SYSCALL: the CPU switches to the kernel code
 * and stack selectors from STAR, saves RIP in RCX and RFLAGS in R11, and
 * FMASK clears TF/DF/IF/AC. isr64.asm builds the same regs64_t frame the
 * interrupt path uses and returns with SYSRET when it can; INT 0x80 at
 * vector 0x80 remains as a DPL-3 fallback (it also serves any code that
 * only knows the older gate). The convention is the x86-64 Linux one:
 * number in RAX, arguments in RDI/RSI/RDX/R10/R8/R9, result in RAX, or a
 * negative errno. The numbers are the Linux x86-64 ones, so a static
 * Linux-targeted binary reaches the implemented calls unchanged. */
#define K64_SYS_READ          0u
#define K64_SYS_WRITE         1u
#define K64_SYS_OPEN          2u
#define K64_SYS_CLOSE         3u
#define K64_SYS_LSEEK         8u
#define K64_SYS_MMAP          9u
#define K64_SYS_MUNMAP        11u
#define K64_SYS_WRITEV        20u
#define K64_SYS_DUP           32u
#define K64_SYS_DUP2          33u
#define K64_SYS_NANOSLEEP     35u
#define K64_SYS_GETPID        39u
#define K64_SYS_CLONE         56u
#define K64_SYS_FORK          57u
#define K64_SYS_EXECVE        59u
#define K64_SYS_EXIT          60u
#define K64_SYS_WAIT4         61u
#define K64_SYS_KILL          62u
#define K64_SYS_MKDIR         83u
#define K64_SYS_GETPPID       110u
#define K64_SYS_ARCH_PRCTL    158u
#define K64_SYS_INIT_MODULE   175u
#define K64_SYS_DELETE_MODULE 176u
#define K64_SYS_GETTID        186u
#define K64_SYS_FUTEX         202u
#define K64_SYS_CLOCK_GETTIME 228u
#define K64_SYS_EXIT_GROUP    231u

/* arch_prctl(2) subcommands. Haku follows Linux: SET_GS/SET_FS install a
 * user GS/FS base and GET_* copy the current base back through a user
 * pointer. The base is the only piece of the x86-64 thread state the kernel
 * owns; what a program puts at that address is its own business. */
#define K64_ARCH_SET_GS 0x1001u
#define K64_ARCH_SET_FS 0x1002u
#define K64_ARCH_GET_FS 0x1003u
#define K64_ARCH_GET_GS 0x1004u

/* The rest of the x86-64 Linux table this port answers. They are grouped
 * the way the kernel implements them: metadata and paths first, then the
 * process and time calls. */
#define K64_SYS_STAT          4u
#define K64_SYS_FSTAT         5u
#define K64_SYS_LSTAT         6u
#define K64_SYS_POLL          7u
#define K64_SYS_MPROTECT      10u
#define K64_SYS_BRK           12u
#define K64_SYS_RT_SIGACTION  13u
#define K64_SYS_RT_SIGPROCMASK 14u
#define K64_SYS_RT_SIGRETURN  15u
#define K64_SYS_IOCTL         16u
#define K64_SYS_PREAD64       17u
#define K64_SYS_PWRITE64      18u
#define K64_SYS_ACCESS        21u
#define K64_SYS_PIPE          22u
#define K64_SYS_SELECT        23u
#define K64_SYS_SCHED_YIELD   24u
#define K64_SYS_MREMAP        25u
#define K64_SYS_MADVISE       28u
#define K64_SYS_DUP3          292u
#define K64_SYS_FCNTL         72u
#define K64_SYS_FSYNC         74u
#define K64_SYS_FDATASYNC     75u
#define K64_SYS_TRUNCATE      76u
#define K64_SYS_FTRUNCATE     77u
#define K64_SYS_GETCWD        79u
#define K64_SYS_CHDIR         80u
#define K64_SYS_FCHDIR        81u
#define K64_SYS_RENAME        82u
#define K64_SYS_RMDIR         84u
#define K64_SYS_UNLINK        87u
#define K64_SYS_CHMOD         90u
#define K64_SYS_FCHMOD        91u
#define K64_SYS_CHOWN         92u
#define K64_SYS_FCHOWN        93u
#define K64_SYS_LCHOWN        94u
#define K64_SYS_UMASK         95u
#define K64_SYS_GETTIMEOFDAY  96u
#define K64_SYS_GETRLIMIT     97u
#define K64_SYS_GETRUSAGE     98u
#define K64_SYS_TIMES         100u
#define K64_SYS_GETUID        102u
#define K64_SYS_GETGID        104u
#define K64_SYS_SETEUID       105u
#define K64_SYS_GETEUID       107u
#define K64_SYS_GETEGID       108u
#define K64_SYS_SETPGID       109u
#define K64_SYS_GETPGRP       111u
#define K64_SYS_SETSID        112u
#define K64_SYS_GETPGID       121u
#define K64_SYS_GETSID        124u
#define K64_SYS_SIGALTSTACK   131u
#define K64_SYS_SETPGID       109u
#define K64_SYS_SET_ROBUST_LIST 273u
#define K64_SYS_GET_ROBUST_LIST 274u
#define K64_SYS_PRCTL         157u
#define K64_SYS_SETRLIMIT     160u
#define K64_SYS_SYNC          162u
#define K64_SYS_UNAME         63u
#define K64_SYS_TIME          201u
#define K64_SYS_GETDENTS64    217u
#define K64_SYS_SET_TID_ADDRESS 218u
#define K64_SYS_CLOCK_GETRES  229u
#define K64_SYS_TGKILL        234u
#define K64_SYS_OPENAT        257u
#define K64_SYS_MKDIRAT       258u
#define K64_SYS_FCHOWNAT      260u
#define K64_SYS_NEWFSTATAT    262u
#define K64_SYS_UNLINKAT      263u
#define K64_SYS_RENAMEAT      264u
#define K64_SYS_FCHMODAT      268u
#define K64_SYS_FACCESSAT     269u
#define K64_SYS_PSELECT6      270u
#define K64_SYS_PPOLL         271u
#define K64_SYS_UTIMENSAT     280u
#define K64_SYS_PIPE2         293u
#define K64_SYS_PRLIMIT64     302u
#define K64_SYS_RENAMEAT2     316u
#define K64_SYS_GETRANDOM     318u
#define K64_SYS_STATX         332u
#define K64_SYS_FACCESSAT2    439u

/* *at flags. Haku has no symbolic links, so NOFOLLOW is accepted and
 * ignored; EMPTY_PATH makes an empty path mean "the dirfd itself". */
#define K64_AT_SYMLINK_NOFOLLOW 0x100u
#define K64_AT_REMOVEDIR        0x200u
#define K64_AT_EMPTY_PATH       0x1000u
/* access(2) modes. */
#define K64_F_OK 0u
#define K64_X_OK 1u
#define K64_W_OK 2u
#define K64_R_OK 4u
/* The extra open flags the syscall layer accepts as no-ops. */
#define K64_O_NOCTTY   0x100u
#define K64_O_NONBLOCK 0x800u
#define K64_O_LARGEFILE 0x8000u
#define K64_O_NOFOLLOW 0x20000u
#define K64_O_CLOEXEC  0x80000u
/* fcntl(2) commands. */
#define K64_F_DUPFD          0u
#define K64_F_GETFD          1u
#define K64_F_SETFD          2u
#define K64_F_GETFL          3u
#define K64_F_SETFL          4u
#define K64_F_GETLK          5u
#define K64_F_SETLK          6u
#define K64_F_SETLKW         7u
#define K64_F_DUPFD_CLOEXEC  1030u
#define K64_F_UNLCK          2u
/* prctl(2) subset. */
#define K64_PR_SET_PDEATHSIG  1u
#define K64_PR_GET_DUMPABLE   3u
#define K64_PR_SET_DUMPABLE   4u
#define K64_PR_SET_NAME       15u
#define K64_PR_GET_NAME       16u
/* poll(2) events and clock ids used by the port. */
#define K64_POLLIN   0x0001u
#define K64_POLLOUT  0x0004u
#define K64_POLLERR  0x0008u
#define K64_POLLHUP  0x0010u
#define K64_POLLNVAL 0x0020u

/* clone() flags, with the Linux numbering. Only these are honoured; the
 * rest are refused so a caller never believes it got a guarantee. CLONE_VM
 * and CLONE_THREAD are the two that make a thread: one address space, one
 * process id. CLONE_FS and CLONE_SYSVSEM are accepted as no-ops because
 * there is no per-task cwd, umask or semaphore undo list to share yet, so
 * "shared" is the only truthful answer. CLONE_SETTLS puts the fifth argument
 * in the child's FS base (the SysV thread pointer); it does not touch GS,
 * because the Windows-x64 TLS convention needs a per-thread TEB chosen by
 * user code, which installs it with arch_prctl(ARCH_SET_GS). The three TID
 * flags use the fourth argument: PARENT_SETTID writes the child's tid into
 * the caller's space before clone() returns, CHILD_SETTID writes it into the
 * child's space before the child runs, and CHILD_CLEARTID zeroes that word
 * and wakes one futex waiter when the child ends, which is what makes a
 * join possible. DETACH is the pthread layer's "nobody will wait for this
 * thread": the slot is released by the CPU that ran it instead of staying
 * waitable. */
#define K64_CLONE_VM              0x00000100u
#define K64_CLONE_FS              0x00000200u
#define K64_CLONE_FILES           0x00000400u
#define K64_CLONE_SIGHAND         0x00000800u
#define K64_CLONE_THREAD          0x00010000u
#define K64_CLONE_SYSVSEM         0x00040000u
#define K64_CLONE_PARENT_SETTID   0x00100000u
#define K64_CLONE_CHILD_CLEARTID  0x00200000u
#define K64_CLONE_SETTLS          0x00080000u
#define K64_CLONE_CHILD_SETTID    0x01000000u
#define K64_CLONE_DETACH          0x40000000u
#define K64_CLONE_SUPPORTED \
    (K64_CLONE_VM | K64_CLONE_FS | K64_CLONE_FILES | K64_CLONE_SIGHAND | \
     K64_CLONE_THREAD | K64_CLONE_SYSVSEM | K64_CLONE_PARENT_SETTID | \
     K64_CLONE_CHILD_SETTID | K64_CLONE_CHILD_CLEARTID | K64_CLONE_SETTLS | \
     K64_CLONE_DETACH)

/* futex(): op is the low seven bits. FUTEX_PRIVATE_FLAG is accepted and
 * treated like the plain op, because a waiter is matched on the address it
 * parked on, which is the private case. WAIT without a timeout only. */
#define K64_FUTEX_WAIT          0u
#define K64_FUTEX_WAKE          1u
#define K64_FUTEX_PRIVATE_FLAG  128u
#define K64_FUTEX_OP_MASK       127u

/* The bits a pthread mutex keeps in its futex word. `owner` is the tid of the
 * holder, so an unlock only releases the mutex if the caller is the owner and
 * a robust unlock can tell "the owner died" from "the owner released it".
 * The kernel sets OWNER_DIED when it walks a dead thread's robust list. */
#define K64_FUTEX_TID_MASK     0x3FFFFFFFu
#define K64_FUTEX_OWNER_DIED   0x40000000u
#define K64_FUTEX_WAITERS      0x80000000u

/* struct robust_list_head, the three-word record set_robust_list(273)
 * registers: the first list node, the offset from a node to its futex word,
 * and the node of an operation in progress. Only the first two are read at
 * exit; a thread that dies mid-lock is handled by the mutex's own owner
 * checks. */
#define K64_ROBUST_LIST_SIZE 24u
/* A user list is walked at exit, so it needs a hard bound: a cycle or a
 * hostile chain must not turn one exit into unbounded work. */
#define K64_ROBUST_LIST_MAX  512u

/* PROT_* and MAP_* as Linux spells them. mmap takes either an anonymous
 * range (fd must be -1) or an open descriptor from fs64.c. MAP_SHARED
 * aliases the page cache, so a store through the mapping is visible to
 * read(), to another descriptor, and to a child created by fork();
 * MAP_PRIVATE copies the pages at map time because there is no COW.
 * MAP_FIXED maps at a caller-chosen address if that range is free; it does
 * not replace an existing mapping. */
#define K64_PROT_READ   1u
#define K64_PROT_WRITE  2u
#define K64_PROT_EXEC   4u
#define K64_MAP_SHARED    0x01u
#define K64_MAP_PRIVATE   0x02u
#define K64_MAP_FIXED     0x10u
#define K64_MAP_ANONYMOUS 0x20u

/* open() flags, with the Linux numbering. */
#define K64_O_RDONLY 0u
#define K64_O_WRONLY 1u
#define K64_O_RDWR   2u
#define K64_O_CREAT  0x40u
#define K64_O_EXCL   0x80u
#define K64_O_TRUNC  0x200u
#define K64_O_APPEND 0x400u
#define K64_O_DIRECTORY 0x10000u

/* Path resolution. A relative path is joined to the task's working
 * directory; AT_FDCWD (-100) is the "no dirfd" marker the *at calls use. */
#define K64_AT_FDCWD (-100)
#define K64_PATH_MAX  K64_FS_PATH_MAX

/* wait4() options and the status encoding. A stopped child reports
 * (signal << 8) | 0x7F, which is the wait-status "stopped" pattern a libc
 * tests with WIFSTOPPED, and a resumed one reports 0xFFFF, its WIFCONTINUED
 * pattern. The low byte of an exit status is 0 and of a signal death is the
 * signal, so the three cannot be confused. */
#define K64_WNOHANG    1u
#define K64_WUNTRACED  2u
#define K64_WCONTINUED 8u
#define K64_WSTATUS_CODE(status) (((status) >> 8) & 0xFFu)
#define K64_WSTATUS_STOP(status) (((status) & 0xFFu) == 0x7Fu)
#define K64_WSTATUS_CONTINUED(status) ((status) == 0xFFFFull)
#define K64_WSTATUS_STOPPED(sig) ((((u64)(sig)) << 8) | 0x7Full)

/* Signals, Linux numbering, 1..31. Real-time signals are not implemented and
 * are rejected with -EINVAL, so the pending and blocked masks fit one u32. */
#define K64_SIGHUP   1u
#define K64_SIGINT   2u
#define K64_SIGQUIT  3u
#define K64_SIGILL   4u
#define K64_SIGTRAP  5u
#define K64_SIGABRT  6u
#define K64_SIGBUS   7u
#define K64_SIGFPE   8u
#define K64_SIGKILL  9u
#define K64_SIGUSR1  10u
#define K64_SIGSEGV  11u
#define K64_SIGUSR2  12u
#define K64_SIGPIPE  13u
#define K64_SIGALRM  14u
#define K64_SIGTERM  15u
#define K64_SIGCHLD  17u
#define K64_SIGCONT  18u
#define K64_SIGSTOP  19u
#define K64_SIGTSTP  20u
#define K64_SIGTTIN  21u
#define K64_SIGTTOU  22u
#define K64_SIGURG   23u
#define K64_SIGWINCH 28u
#define K64_NSIG     31u
/* Every bit a mask may carry: signals 1..31. */
#define K64_SIG_MASK_ALL ((u32)((1ull << K64_NSIG) - 1ull))
/* Signal 1 maps to bit 0, so a mask bit is 1 << (sig - 1). */
#define K64_SIG_BIT(sig) ((u32)1u << ((sig) - 1u))
#define K64_SIGKILL_BIT  K64_SIG_BIT(K64_SIGKILL)
#define K64_SIGSTOP_BIT  K64_SIG_BIT(K64_SIGSTOP)

/* rt_sigaction(2) handler values, flags and rt_sigprocmask(2) operations,
 * with the Linux numbering. The kernel only honours these four flags; the
 * rest are accepted and ignored. */
#define K64_SIG_DFL 0ull
#define K64_SIG_IGN 1ull
#define K64_SIG_BLOCK   0u
#define K64_SIG_UNBLOCK 1u
#define K64_SIG_SETMASK 2u
#define K64_SA_SIGINFO   0x00000004ull
#define K64_SA_ONSTACK   0x08000000ull
#define K64_SA_RESTORER  0x04000000ull
#define K64_SA_RESTART   0x10000000ull
#define K64_SA_NODEFER   0x40000000ull
#define K64_SA_RESETHAND 0x80000000ull

/* sigaltstack(2) flags. SS_ONSTACK is reported while a handler frame is on
 * the alternate stack; SS_AUTODISARM is accepted and ignored. */
#define K64_SS_ONSTACK    1u
#define K64_SS_DISABLE    2u
#define K64_SS_AUTODISARM 0x80000000u
#define K64_SIGSTKSZ_MIN  2048ull
/* errno a blocked syscall reports when a signal it will act on arrives. */
#define K64_SIG_EINTR     4u

/* lseek() whence values, with the Linux numbering. */
#define K64_SEEK_SET 0u
#define K64_SEEK_CUR 1u
#define K64_SEEK_END 2u

#define K64_CLOCK_REALTIME  0u
#define K64_CLOCK_MONOTONIC 1u

/* Stack frame isr64.asm builds; see the comment at the top of that file. */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} regs64_t;

/* --------------------------------------------------------------- signals
 *
 * A task carries one disposition per signal, a blocked mask and the pending
 * bits. A signal is raised into `pending` and acted on at the last moment
 * before the task returns to ring 3, which is why a handler can interrupt a
 * syscall but never the middle of one.
 *
 * The frames the kernel builds on the user stack are part of the ABI between
 * the kernel and the restorer that comes back with rt_sigreturn(2); the
 * matching user-side layout is in include/haku/abi64.h.
 *
 *   +0x000  restorer     the handler's return address
 *   +0x008  shadow       Microsoft x64 shadow space: a handler spills its
 *                        register arguments here, so the frame stays clear
 *   +0x028  magic        identifies the frame for rt_sigreturn
 *   +0x030  blocked      the mask in effect when the signal was raised
 *   +0x038  siginfo      128 bytes
 *   +0x0B8  ucontext     the interrupted register set
 *
 * The handler arguments follow the Microsoft x64 C ABI this toolchain uses,
 * not the Linux System V one: RCX is the signal number, RDX the siginfo
 * pointer (SA_SIGINFO only) and R8 the ucontext pointer (SA_SIGINFO only).
 */
#define K64_SIGFRAME_MAGIC   0x48414B5536345349ull   /* "HAKU64SI" */
#define K64_SIGFRAME_SHADOW  32u
#define K64_SIGFRAME_SIGINFO 0x38u
#define K64_SIGFRAME_INFO_SIZE 128u
#define K64_SIGFRAME_UCONTEXT (K64_SIGFRAME_SIGINFO + K64_SIGFRAME_INFO_SIZE)

typedef struct {
    u64 handler;   /* K64_SIG_DFL, K64_SIG_IGN, or a ring-3 address */
    u64 flags;     /* K64_SA_* */
    u64 restorer;  /* SA_RESTORER trampoline, required for a handler */
    u64 mask;      /* extra signals blocked inside the handler */
} k64_sigaction_t;

typedef struct {
    u64 flags;
    u64 link;
    u64 ss_sp;     /* alternate stack, or 0 when disabled */
    u32 ss_flags;
    u32 pad0;
    u64 ss_size;
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, cs, rflags, rsp, ss;
    u64 sigmask;   /* the blocked mask to restore */
} k64_ucontext_t;

typedef struct {
    u64 restorer;
    u64 shadow[K64_SIGFRAME_SHADOW / 8u];
    u64 magic;
    u64 blocked;
    u8 info_block[K64_SIGFRAME_INFO_SIZE];
    k64_ucontext_t uc;
} k64_sigframe_t;

/* Per-task signal state. The pending and blocked words cover signals 1..31,
 * so a task table of K64_TASK_MAX slots costs about 16 KiB of dispositions;
 * that is the price of a handler table per task and is paid once at boot. */
typedef struct {
    u32 pending;
    u32 blocked;
    u64 alt_sp;
    u64 alt_size;
    u32 alt_flags;
    u32 alt_depth;  /* handler frames currently running on the alt stack */
    k64_sigaction_t actions[K64_NSIG];
} k64_sigstate_t;

/* ------------------------------------------------------------- per-CPU block
 * One of these per processor, reached through GS after a SWAPGS. The first
 * three fields have fixed offsets because isr64.asm reads them with no help
 * from the compiler:
 *
 *   +0   self      the block's own address, so gs:[0] is a plain pointer
 *   +8   rsp0      kernel stack top the SYSCALL stub switches to
 *   +16  user_rsp  scratch for the user stack pointer during that switch
 *
 * percpu64_offsets_ok() compares offsets (8, 16) with the constants in the
 * assembly, so a reordering that broke them fails the boot test instead of
 * corrupting a system call. Everything after +24 is ordinary kernel state.
 *
 * The block is 64-byte aligned and padded to 128 bytes: two adjacent CPUs
 * never share a cache line, so `ticks` on one CPU does not invalidate the
 * other's `current`. */
typedef struct k64_percpu {
    u64 self;
    u64 rsp0;
    u64 user_rsp;
    u32 index;        /* 0 for the bootstrap processor */
    u32 apic_id;      /* local APIC id, for IPIs */
    u32 current;      /* task slot running here, (u32)-1 before scheduling */
    u32 idle;         /* this CPU's idle task slot, (u32)-1 before it exists */
    volatile u32 online;
    u32 pad0;
    u64 ticks;        /* local timer ticks delivered to this CPU */
    u64 switches;     /* context switches taken by this CPU */
    u64 active_space; /* PML4 physical address currently in CR3 */
    volatile u32 tlb_ack;    /* shootdown round this CPU has completed */
    volatile u32 tlb_target; /* round it was asked to complete */
    u64 flushes;      /* TLB flushes this CPU performed for another */
    u32 cursor;       /* last task slot this CPU dispatched (round robin) */
    u32 pad1;
    /* A detached thread that just ended: its slot is finalized at the next
     * switch on this CPU, once the CPU has left the thread's kernel stack.
     * (u32)-1 means nothing is pending. Tasks are pinned, so the pending
     * slot is always one this CPU created and can reap. */
    u32 reap_pending;
    u8 pad2[28];
} k64_percpu_t;

/* A test-and-set lock that also owns interrupts. `flags` is the caller's
 * RFLAGS from before the acquisition: unlocking restores them, so the lock
 * nests inside interrupt-disabled code and re-enables interrupts only for
 * the context that had them on. Two CPUs can contend; the same CPU cannot
 * deadlock itself, because the holder never gets preempted with the lock
 * held. */
typedef struct {
    volatile u32 locked;
} spinlock64_t;

#define SPINLOCK64_INIT { 0u }
u64 spin64_lock(spinlock64_t *lock);
void spin64_unlock(spinlock64_t *lock, u64 flags);

/* percpu64.c. K64_SMP_MAX_CPUS bounds the per-CPU array, the AP descriptor
 * tables and the task affinity field; eight slots is the smallest power of
 * two that leaves room for the two such a QEMU run brings up plus the
 * workers the scheduling test pins to them.
 *
 * The blocks are reached through a pointer rather than a plain array because
 * they have to be cache-line aligned (see the layout comment above) and the
 * kernel image is linked with /ALIGN:16, which rejects a 64-byte-aligned
 * section; percpu64.c aligns the storage once at the first install. */
#define K64_SMP_MAX_CPUS 8u
extern k64_percpu_t *cpus64;
void percpu64_init_bsp(u32 apic_id);
void percpu64_init_ap(u32 index, u32 apic_id);
void percpu64_set_apic_id(u32 index, u32 apic_id);
u32 k64_cpu_index(void);
int percpu64_offsets_ok(void);

#pragma pack(push, 1)
typedef struct {
    u16 offset_low;
    u16 selector;
    u8 ist;        /* bits 0..2: which IST stack to switch to (0 = none) */
    u8 type_attr;  /* P, DPL, gate type */
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} idt64_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} table_desc64_t;
#pragma pack(pop)

/* 64-bit TSS. Only RSP0 and the IST pointers are used: there is no hardware
 * task switching in long mode, so the descriptor exists for the stack
 * switches a privilege change or a double fault needs. */
#pragma pack(push, 1)
typedef struct {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} tss64_t;
#pragma pack(pop)

/* entry64.asm */
void serial_write64(const char *text);
void serial_putc64(char c);
void invlpg64(u64 address);
void cpu_halt64(void);

/* isr64.asm */
void isr64_load_idt(u64 idtr_addr);
void gdt64_load(u64 gdtr_addr);
void tss64_load(u16 selector);
/* SYSCALL/SYSRET; the entry point is taken by address in syscall64_init. */
void syscall64_setup(u64 star, u64 lstar, u64 fmask);
extern u8 syscall64_entry[];
void sti64(void);
void cli64(void);
u64 read_cr2_64(void);
u64 read_cr3_64(void);
void write_cr3_64(u64 cr3);
void enable_sse64(void);
void fxsave64(void *area);
void fxrstor64(void *area);
void cpu_pause64(void);
void outb64(u16 port, u8 value);
u8 inb64(u16 port);
u16 inw64(u16 port);
void outw64(u16 port, u16 value);
void inw_string64(u16 port, void *destination, u32 count);
void int3_probe64(void);
u64 probe_load64(u64 address);
u64 probe_div64(u64 a, u64 b);
extern u8 probe_load64_fault[];
extern u8 probe_load64_resume[];
extern u8 probe_div64_fault[];
extern u8 probe_div64_resume[];

/* Per-CPU state, MSR access and the two flag helpers. These are the only
 * instructions MSVC cannot emit, so everything else about the per-CPU block
 * is ordinary C. */
k64_percpu_t *k64_cpu(void);
u64 read_msr64(u32 msr);
void write_msr64(u32 msr, u64 value);
u64 read_rflags64(void);
void write_rflags64(u64 flags);
/* Unwind into a saved frame instead of returning to the caller; used to
 * start an application processor's first task. Does not come back. */
void isr64_resume64(regs64_t *frame);
/* (rsp0 offset << 32) | user_rsp offset, so C can cross-check the layout the
 * assembly hardcodes. */
u64 isr64_percpu_offsets(void);

/* Freestanding string/memory primitives: the compiler is allowed to emit
 * calls to these even with /Zl, so they must exist. */
void *memset(void *dst, int value, usize count);
void *memcpy(void *dst, const void *src, usize count);
int memcmp(const void *a, const void *b, usize count);

/* CMOS real-time clock (rtc64.c). The kernel snapshots the RTC once at
 * boot; clock_gettime anchors CLOCK_REALTIME on it and falls back to
 * time since boot when the chip is unreadable. */
int rtc64_init(void);
u64 rtc64_epoch(void);
u64 rtc64_realtime(u64 ticks, u64 hz);

/* Console output (serial64.c) */
void k64_puts(const char *text);
void k64_putc(char c);
void k64_hex(u64 value);
void k64_dec(u64 value);
void k64_fail(const char *what);

/* console64.c: the console tee. Every byte the kernel prints goes to COM1
 * (what the headless check reads) and, once console64_init() has picked a
 * backend, to the screen. Before that the display is still the boot
 * firmware's, so only the serial side is live. */
void console64_init(void);
/* 1 when the screen is being painted (a backend was selected). */
int console64_ready(void);
/* Hand the screen to userland: from here k64_putc writes COM1 only, so a
 * kernel marker cannot draw over a running program. console64_reclaim() is
 * the fatal-path exception (the screen has to show why the kernel stopped). */
void console64_handover(void);
void console64_reclaim(void);
/* Output that belongs to the program rather than the kernel log. It always
 * paints the screen, latched or not; the console write path and the line
 * discipline's echo use it. */
void console64_user_putc(char c);
/* Answers the screen size the console writer can use: the framebuffer's text
 * grid, or the VGA text grid. Both are always valid after console64_init. */
u32 console64_columns(void);
u32 console64_rows(void);
const char *console64_name(void);

/* fb64.c: map the VBE aperture stage 2 selected and start the shared
 * renderer. Returns 1 when the framebuffer console is live, 0 when the
 * handover is missing or unusable (VGA text stays the backend). */
int fb64_init(void);

/* kbd64.c: PS/2 keyboard. Set-1 scancodes from IRQ1 are decoded to ASCII and
 * queued; console read() pops from that queue and parks on
 * K64_CONSOLE_WAIT_ADDR when it is empty, so IRQ1 is the wakeup. */
#define K64_KBD_QUEUE 128u
/* A kernel pseudo-address, far above any user mapping: the scheduler only
 * compares it for equality when it matches waiters. */
#define K64_CONSOLE_WAIT_ADDR 0xFFFFFFFFFFFFF000ull
void kbd64_init(void);
/* Decode-only self-test with synthesised scancodes, so it does not depend on
 * the headless run typing anything. Prints HAKU64-KBD-OK. */
int kbd64_self_test(void);
/* Pop one decoded byte; -1 when the queue is empty. */
int kbd64_read(void);
/* Number of queued bytes (safe from interrupt context). */
u32 kbd64_pending(void);
/* IRQ1: read the controller and queue what it produced. */
void kbd64_irq(void);

/* tty64.c: the console's canonical line discipline. The keyboard queue holds
 * raw keys; this layer turns them into one echoed line at a time. The
 * reported termios is the contract it implements: Enter commits, 0x08 and
 * VERASE 0x7F erase, ^D ends the line (an empty line is EOF), and ^C
 * discards the line and raises SIGINT for the foreground group. Control
 * characters reach it because kbd64.c tracks Ctrl and maps Ctrl+letter to
 * 0x01..0x1A, which is what makes ^D and ^C typeable on a PC keyboard. */
void tty64_reset(void);
/* 1 when a finished line (or an EOF result) is waiting for the reader. */
int tty64_pending_line(void);
/* Drain queued keys into the line, echoing them. Stops at a finished line, so
 * the work is bounded by the keyboard ring. Returns keys consumed; sets
 * `interrupted` when ^C discarded the line and queued SIGINT. */
u32 tty64_pump(int id, int *interrupted);
/* Copy up to `count` bytes of the finished line; 1 when a read result was
 * produced (a ^D on an empty line produces a zero-byte result). */
int tty64_take_line(void *dst, u32 count, u32 *copied);
/* TCFLSH: drop queued keys and the line being edited. */
void tty64_flush(void);

/* bootmem64.c: bump allocator for the structures that must exist before
 * the frame allocator and heap do (page tables, frame bitmaps). */
void bootmem64_init(void);
void *bootmem64_alloc(u64 bytes, u64 alignment);
u64 bootmem64_used(void);

/* paging64.c */
int paging64_init(u64 phys_limit);
int paging64_map_page(u64 virt, u64 phys, int writable);
int paging64_unmap_page(u64 virt);
u64 paging64_translate(u64 virt);
u64 paging64_phys_map_bytes(void);
void *phys_to_virt64(u64 phys);
/* Map an MMIO page into the kernel half with PCD+PWT (uncached). Used for
 * the local APIC page; the caller supplies a window outside the physmap. */
int paging64_map_device_page(u64 virt, u64 phys);
/* Idempotently map K64_LAPIC_VIRT -> K64_LAPIC_PHYS. Returns 0 when no
 * boot memory was left for the intermediate tables. */
int paging64_map_lapic(void);
/* Map a VBE linear framebuffer aperture into K64_FB_VIRT, uncached. The
 * range must be page aligned and fit the 4 MiB window (stage 2 caps a mode at
 * 1024x768x32, which is 3 MiB). Returns 1 on success. */
int paging64_map_framebuffer(u64 phys, u64 bytes);

/* Per-process address spaces. A space is the physical address of a PML4
 * that shares the kernel's identity map and physmap and owns PML4 slot 1.
 * Tables come from the frame allocator, so a space can be created once the
 * frame allocator is up (not before paging64_init). */
u64 paging64_create_space(void);
int paging64_map_space(u64 space, u64 virt, u64 phys, int writable, int user,
                       int executable);
u64 paging64_translate_space(u64 space, u64 virt);
/* Query one leaf of a space without changing it. Returns 0 when the page
 * is absent (or the address is not in the user half); on success `phys`
 * carries the page base plus the offset, `writable`/`user` the leaf bits. */
int paging64_query_space(u64 space, u64 virt, u64 *phys, int *writable,
                         int *user);
/* Clear one user leaf and return the frame it mapped, or 0. */
u64 paging64_unmap_space(u64 space, u64 virt);
/* Change writable/executable of one mapped user page (mprotect). */
int paging64_protect_space(u64 space, u64 virt, int writable, int executable);
void paging64_destroy_space(u64 space);
/* fork support: share every user page copy-on-write (shared ranges are
 * aliased) and free a space's frames, keeping the ones `keep` claims. */
u64 paging64_clone_space(u64 parent,
                         int (*shared)(u64 virt, void *ctx), void *ctx);
void paging64_free_space_frames(u64 space,
                                int (*keep)(u64 virt, void *ctx), void *ctx);
/* Give the faulting space its own copy of a shared page. Returns 1 when it
 * broke the sharing, 0 when the page is absent, not COW or out of memory. */
int paging64_break_cow(u64 space, u64 virt);
/* Make one page ready for a kernel direct-map write, breaking COW first.
 * Returns 1 when the page is writable afterwards. */
int paging64_write_prepare(u64 space, u64 virt);
void paging64_switch_space(u64 space);

/* ------------------------------------------------------------- ELF64 loader
 * On-disk layout (the loader validates it before touching memory). The
 * supported subset is static ET_EXEC: no PT_INTERP, no PT_DYNAMIC, readable
 * PT_LOAD segments that are not both writable and executable. */
#define K64_ELF64_MAX_PHDRS 16u
#define K64_ELF64_MAX_PAGES 64u

#pragma pack(push, 1)
typedef struct {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_header_t;

typedef struct {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr_t;
#pragma pack(pop)

/* What elf64_load produced. `writable` is the page-aligned start of the
 * first writable segment (0 when the image has none) and is what the
 * self-test reads back after ring 3 has run. `phdr` is the virtual address
 * of the program header table when a PT_LOAD maps the file offset the table
 * lives at (the PE repacker adds that header segment); a program with no
 * mapped headers gets 0 and no AT_PHDR, because a pointer to unmapped
 * memory would be worse than a missing entry. */
typedef struct {
    u64 space;
    u64 entry;
    u64 stack_top;
    u64 stack_bottom;
    u64 image_low;
    u64 image_high;
    u64 writable;
    u64 phdr;
    u32 pages;
    u32 phent;
    u32 phnum;
} loaded_elf64_t;

int elf64_load(const u8 *image, u64 image_size, loaded_elf64_t *loaded);
void elf64_unload(loaded_elf64_t *loaded);
const char *elf64_last_error(void);

/* user64_image.asm: the ELF64 test program, embedded by the build. */
extern const u8 user64_image[];
extern const u8 user64_image_end[];

/* frame64.c */
int frame64_init(u64 phys_limit);
u64 frame64_alloc(void);
void frame64_free(u64 phys);
/* fork copies-on-write: a shared frame is counted, and the last owner
 * frees it. An uncounted frame is released by its only owner at once. */
void frame64_ref_share(u64 phys);
void frame64_user_release(u64 phys);
u32 frame64_ref_count(u64 phys);
u64 frame64_free_count(void);
u64 frame64_usable_count(void);
u64 frame64_limit(void);
u64 frame64_highest_free(void);
/* Keep a physical range out of the pool. The framebuffer aperture can sit
 * inside RAM on some firmware, and handing it to a task would paint over the
 * screen; called once, after frame64_init and before the first user space. */
void frame64_reserve_range(u64 start, u64 end);

/* heap64.c */
int heap64_init(void);
void *kmalloc64(u64 bytes);
void kfree64(void *ptr);
u64 heap64_free_bytes(void);

/* gdt64.c: GDT, TSS and the IST stacks */
void gdt64_init(void);
/* Per-AP descriptor table and TSS. Prepare on the bootstrap processor,
 * load on the AP. rsp0_top is what a ring-3 entry on that CPU would use. */
int gdt64_prepare_ap(u32 cpu, u64 rsp0_top);
int gdt64_load_ap(u32 cpu);
u64 gdt64_ap_rsp0(u32 cpu);
u64 gdt64_tss_base(void);
u64 gdt64_ist_top(u32 index);
void gdt64_set_rsp0(u64 top);
u64 gdt64_rsp0(void);
/* Per-CPU RSP0: a ring-3 entry on `cpu` must land on the kernel stack of the
 * task that CPU is running, so the scheduler calls this on every switch. */
void gdt64_set_rsp0_cpu(u32 cpu, u64 top);

/* task64.c: tasks and the preemptive round-robin scheduler.
 *
 * A task's context is the regs64_t frame the interrupt stubs already
 * build, so a switch is "return a different frame pointer from
 * isr64_dispatch". New tasks start from a frame assembled on their own
 * kernel stack; a user task's frame carries CS=0x23/SS=0x1B and is
 * entered with IRETQ from the same path. */
/* 16 slots: the boot tests leak the slots of the tasks they finish (nothing
 * reaps them), and every application processor now owns an idle task, so the
 * eight slots the uniprocessor port needed no longer cover a test run. The
 * static cost is about 1 KiB per slot, all of it per-task bookkeeping rather
 * than stacks, which are allocated on demand. */
#define K64_TASK_MAX        16u
#define K64_TASK_STACK_SIZE 16384ull

enum {
    K64_TASK_UNUSED = 0,
    K64_TASK_READY,
    K64_TASK_RUNNING,
    /* Blocked in futex(FUTEX_WAIT). The saved frame is the syscall frame
     * with RAX=0, so a wake resumes the task as if the call had returned. */
    K64_TASK_WAIT,
    /* Parked by a stop signal (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU). The
     * saved frame is the return-to-ring-3 frame the signal interrupted, so
     * SIGCONT resumes the task exactly where it left off. A stopped task is
     * not runnable and not reapable: only SIGCONT, SIGKILL or exit_group
     * moves it again. */
    K64_TASK_STOP,
    K64_TASK_DONE
};

void sched64_init(void);
int task64_create_kernel(void (*entry)(void));
/* Create a kernel task pinned to `cpu`. Affinity is explicit and fixed:
 * nothing migrates a task, so a worker started on an AP stays there even if
 * that AP is busy. Returns the slot or -1. */
int task64_create_kernel_on(u32 cpu, void (*entry)(void));
/* Build the idle task for `cpu` on the bootstrap processor. Returns its slot
 * or -1. Called once per AP by smp64_release_aps(). */
int sched64_prepare_ap(u32 cpu);
/* Enter `cpu`'s idle task; runs on the AP and does not return. */
void sched64_start_ap(u32 cpu);
int task64_create_user(u64 entry, u64 user_stack_top, u64 space);
regs64_t *sched64_tick(regs64_t *frame);
regs64_t *sched64_exit(regs64_t *frame);
/* end every task of the caller's thread group, then the caller */
regs64_t *sched64_exit_group(regs64_t *frame);
/* kill() of the caller's own thread group: a fatal signal ends every peer,
 * then the caller, and never resumes its frame. */
regs64_t *sched64_signal_group(regs64_t *frame, u32 signal);
/* The default action of a stop signal: save `frame`, mark the caller
 * K64_TASK_STOP with `signal`, and switch away. SIGCONT resumes it through
 * that same frame. A SIGCONT or SIGKILL that is already pending wins, and
 * the frame is returned so the signal is delivered on the next entry. */
regs64_t *sched64_stop(regs64_t *frame, u32 signal);
/* The signal state of `id`, or 0 when the slot is unused. syscall64.c owns
 * the copy-in/copy-out and frame building; task64.c owns the table. */
k64_sigstate_t *task64_signals(int id);
/* 1 when `id` has a signal that is pending and not blocked. The blocking
 * syscalls poll this in their halt loops so a signal ends them with EINTR. */
int task64_signal_pending(int id);
/* Raise `signal` for `id` (self allowed): set the bit, wake a parked task
 * with EINTR, and reschedule its CPU. */
int task64_raise(int id, u32 signal);
/* Called on the way back to ring 3: act on the first unblocked pending
 * signal (ignore, default action, or run the handler). */
regs64_t *sys64_signal_deliver(regs64_t *frame);
/* A ring-3 exception is not resumable: mark the task done, record why, and
 * switch to the next runnable task. */
regs64_t *sched64_fault(regs64_t *frame, u32 vector, u64 address);
void task64_kill(int id);
int task64_state(int id);
u64 task64_kernel_stack(int id);
u64 sched64_switches(void);
int sched64_current(void);
u64 sched64_space(int id);
/* Status passed to exit/exit_group, kept after the task is done. */
u64 task64_exit_status(int id);
/* Why a task stopped, when it stopped on a user fault instead of exit():
 * the vector and, for a page fault, CR2. */
u64 task64_fault_vector(int id);
u64 task64_fault_address(int id);
/* Next anonymous mapping address, and a reservation that fails when the
 * request would reach the stack guard or exceed the page cap. */
u64 task64_mmap_base(int id);
u64 task64_get_brk(int id);
void task64_set_brk(int id, u64 value);
/* Per-task user FS/GS bases, the addresses arch_prctl(158) reads and writes.
 * The scheduler reloads both MSRs from these fields on every switch, so a
 * fork inherits them and execve clears them with the rest of the old image. */
u64 task64_fs_base(int id);
u64 task64_gs_base(int id);
void task64_set_fs_base(int id, u64 base);
void task64_set_gs_base(int id, u64 base);
u64 task64_mmap_reserve(int id, u64 pages);
/* Mapping records: munmap has to know whether the frames in a range belong
 * to the mapping (anonymous or a private file copy) or to a cache page that
 * a MAP_SHARED mapping aliases, because only the first kind may be freed.
 * `shared` also tells fork whether the child aliases the frame or gets a
 * private copy. */
int task64_map_record(int id, u64 base, u64 pages, int owned, int shared);
int task64_map_owned(int id, u64 address);
int task64_map_shared(int id, u64 address);
void task64_map_drop(int id, u64 base, u64 length);

/* Per-task working directory, kept as a canonical absolute path ("/" at the
 * volume root). Threads share their group's path through the space owner. */
int task64_get_cwd(int id, char *out, u64 size);
int task64_set_cwd(int id, const char *path);
int task64_set_name(int id, const char *name);
void task64_get_name(int id, char out[16]);
/* Yield to another READY task, returning at the same syscall completion. */
regs64_t *sched64_yield(regs64_t *frame);

/* fork/wait: a child gets a copy of its parent's user pages (shared ranges
 * are aliased, everything else is copied) and of its descriptor table. The
 * parent blocks in wait4 until the child is DONE, then the child's space,
 * frames and kernel stack are released. `flags` carries the clone() TID
 * flags when a clone() without CLONE_VM degrades to a fork; the child's tid
 * word is written in its own (copied) space, so `child_tid` must name a
 * mapping the parent has too. */
regs64_t *sched64_fork(regs64_t *frame, u64 flags, u64 child_tid);
/* clone(): with CLONE_VM the child runs in the caller's address space on
 * `child_stack` (16-byte aligned, inside the user half), shares the space's
 * frames and mapping records, and returns 0 in the child. CLONE_THREAD keeps
 * the caller's process id (getpid) while gettid differs; CLONE_FILES shares
 * the descriptor table, otherwise the child gets a copy. CLONE_SETTLS makes
 * `tls` the child's FS base and leaves it inherited without the flag.
 * CLONE_CHILD_SETTID writes the child's tid to `child_tid` before the child
 * runs, and CLONE_CHILD_CLEARTID zeroes it and wakes one futex waiter when
 * the child ends, which is how haku64_thread_join() waits. The child is
 * reapable by the task that created it. */
regs64_t *sched64_clone(regs64_t *frame, u64 flags, u64 child_stack,
                        u64 tls, u64 child_tid);
/* Process id of the thread group `id` belongs to, as a task index. */
int sched64_tgid(int id);
/* Process groups and sessions. A group id and a session id are user-visible
 * ids (a task index plus one), the way Linux numbers them. Every thread of a
 * thread group shares one group and one session, so the setters move the whole
 * group. setpgid/setsid take the user argument, validate it here, and return 0
 * or a negative errno; kill_pgrp delivers a signal to every task in a group
 * and answers how many were signalled (0 targets means ESRCH). */
u32 sched64_pgrp(int id);
u32 sched64_sid(int id);
int sched64_set_pgrp(int id, u32 pgid);
int sched64_pgrp_exists(u32 pgid);
int sched64_setsid(int id);
int sched64_signal_pgrp(u32 pgid, u32 signal);
/* The console's foreground process group. Zero means "not set", and every
 * caller is then foreground. The setter answers 0, -EINVAL for group 0 and
 * -EPERM for a group with no live member. It lives beside the process groups
 * because the exit path hangs up the foreground group when a session leader
 * ends, not only the ioctl path. */
u32 sched64_console_fg(void);
int sched64_console_set_fg(u32 pgrp);
/* Robust futex list (set_robust_list 273 / get_robust_list 274). `head` is
 * the user address of a struct robust_list_head; the scheduler walks it when
 * the registering task exits and marks every futex it finds still owned by
 * that task as owner-died, which is how a robust pthread mutex tells a
 * crashed holder from an ordinary release. Registering replaces the previous
 * list; the head is not copied, since the owner may relink its nodes. */
int sched64_set_robust_list(int id, u64 head, u64 length);
int sched64_get_robust_list(int id, u64 *head, u64 *length);
/* futex wake: make up to `count` tasks parked on `address` runnable again and
 * answer how many were woken. A WAIT is sched64_park_until() with the value
 * recheck as its `ready` hook, which is what closes the lost-wakeup window. */
int sched64_futex_wake(int id, u64 address, int count);
/* Park the caller on `address` with `result` in RAX and switch away; the
 * wake side is sched64_futex_wake. Used for the console read when the
 * keyboard queue is empty, so a blocking read costs no CPU. Returns the frame
 * to resume the next task with. */
regs64_t *sched64_park(regs64_t *frame, u64 address, u64 result);
/* As sched64_park, but `ready` decides whether parking is still necessary. It
 * runs with the task lock held, so a producer that publishes under the same
 * lock cannot be missed. `parked`, when given, is 1 after a real switch
 * away; a task that did not park sees `result` in RAX and retries. */
regs64_t *sched64_park_until(regs64_t *frame, u64 address, u64 result,
                             int (*ready)(void), int *parked);
/* reap a DONE child of `parent`; returns the child id or a negative errno */
int task64_exec(int id, u64 space);
/* Reap or report one child of `parent`. `options` is a mask of K64_WNOHANG,
 * K64_WUNTRACED and K64_WCONTINUED. A stop is reported once under WUNTRACED
 * and leaves the child in place; a resume is reported once under WCONTINUED.
 * Returns the child's pid, -ECHILD when there is no such child, or -EAGAIN
 * when a matching child exists but has nothing to report. */
int task64_reap(int parent, int pid, u32 options, u64 *status);
/* terminate a task as if by a signal; returns 0 or a negative errno */
int task64_signal(int id, u32 signal);
/* kill(2): end the whole thread group `id` leads. A process with threads
 * dies as one, because the default action of a fatal signal has to reach
 * every thread. Returns 0, -ESRCH when nothing in the group is alive, or the
 * signal-0 existence answer. */
int task64_signal_group(int id, u32 signal);
int task64_parent(int id);
/* 1 when `pgrp` names a process group that still has a live member. The
 * console's foreground group is checked with this so a shell that exits
 * without cleaning up cannot leave a stale answer behind. */
int task64_pgrp_valid(u32 pgrp);

/* fs64.c: a page cache in front of every mounted filesystem. A cached file
 * is at most FS64_PAGE_MAX pages (128 KiB) and eight paths are cached at
 * once. The FAT12 volume is mounted at /disk and the RAM initramfs at /. */
#define K64_FS_FDS      16u
#define K64_FS_PATH_MAX 64u
#define FS64_PAGE_MAX   32u
/* Size of a directory-entry name buffer, including the NUL. */
#define FS64_NAME_MAX   48u
/* fs64_fd_file() result for a duplicated console descriptor. */
#define K64_FS_CONSOLE  (-2)
int fs64_init(void);
int fs64_lookup(const char *path);
void fs64_release(int file);
int fs64_create_dir(const char *path);
u64 fs64_size(int file);
u64 fs64_pages(int file);
u64 fs64_page(int file, u64 index);
u64 fs64_read(int file, u64 offset, void *dst, u64 count);
u64 fs64_write(int file, u64 offset, const void *src, u64 count);
int fs64_truncate(int file, u64 size);
int fs64_open_fd(int task, const char *path, u64 flags, int *error);
int fs64_fd_file(int task, int fd);
/* Mount-table device descriptors: a device is neither a file (no cache
 * slot) nor the console. The read/write helpers return a byte count or a
 * negative -errno, and they enforce the descriptor's access mode. */
int fs64_fd_device(int task, int fd);
int fs64_fd_dev_read(int task, int fd, void *dst, u64 count);
int fs64_fd_dev_write(int task, int fd, const void *src, u64 count);
/* Module-backed files: the backend owns the handle and the position, so these
 * forward to the module and mirror the offset into the description. They
 * return a byte count or a negative -errno; seek returns 0 and reports the
 * new offset through `position`. */
int fs64_fd_module(int task, int fd);
int fs64_fd_module_read(int task, int fd, void *dst, u64 count);
int fs64_fd_module_write(int task, int fd, const void *src, u64 count);
int fs64_fd_module_seek(int task, int fd, s64 offset, u64 whence,
                        u64 *position);
int fs64_fd_module_truncate(int task, int fd, u64 size);
u64 fs64_fd_pos(int task, int fd);
int fs64_fd_seek(int task, int fd, u64 pos);
int fs64_fd_access(int task, int fd);
/* A pipe end is a description with no cache slot: both ends name the same
 * index in pipe64.c, and the writer flag is the access mode the end was
 * created with. nonblock is the per-description O_NONBLOCK bit F_SETFL
 * controls. */
int fs64_fd_pipe(int task, int fd);
int fs64_fd_pipe_index(int task, int fd);
int fs64_fd_pipe_writer(int task, int fd);
int fs64_fd_nonblock(int task, int fd);
int fs64_fd_set_nonblock(int task, int fd, int on);
/* pipe()/pipe2(): two fresh descriptions on the lowest free descriptors, or a
 * negative errno. `flags` accepts O_NONBLOCK and O_CLOEXEC. */
int fs64_pipe(int task, u64 flags, int *read_fd, int *write_fd);
int fs64_fd_dup(int task, int fd, int minimum);
int fs64_fd_dup2(int task, int fd, int target);
int fs64_fd_console(int task, int fd);
void fs64_fd_close(int task, int fd);
void fs64_fork_fds(int parent, int child);
/* clone(CLONE_FILES): the child uses the parent's table, so a descriptor one
 * thread opens, closes or dup2s is visible to the other. The table is closed
 * only when the last user is reaped. */
void fs64_share_fds(int parent, int child);
void fs64_task_cleanup(int task);

/* Metadata and path operations on the mounted filesystems. They share the
 * page cache, so a write through an open descriptor is visible to a stat or
 * a directory listing once the slot is flushed. All return 0 or a negative
 * errno. `kind` is FS64_KIND_*. */
#define FS64_KIND_FILE   0
#define FS64_KIND_DIR    1
#define FS64_KIND_CHAR   2
#define FS64_KIND_FIFO   3
int fs64_stat_path(const char *path, u64 *size, u32 *ino, int *kind);
int fs64_stat_fd(int task, int fd, u64 *size, u32 *ino, int *kind);
int fs64_unlink(const char *path);
int fs64_rename(const char *oldpath, const char *newpath);
int fs64_rmdir(const char *path);
int fs64_fd_is_dir(int task, int fd);
int fs64_fd_dir_path(int task, int fd, char out[K64_FS_PATH_MAX + 1]);
int fs64_fd_sync(int task, int fd);
void fs64_sync_all(void);
/* Entry `index` of the directory open on `fd`, with the cursor advanced in
 * the description. Returns 1 with the entry filled, 0 at the end, or a
 * negative errno. `kind` is FS64_KIND_*. */
int fs64_fd_entry(int task, int fd, u32 index, char name[FS64_NAME_MAX],
                  u32 *ino, int *kind, u32 *next_index);
int fs64_dir_entries(const char *path, u32 index, char name[FS64_NAME_MAX],
                     u32 *ino, int *kind, u32 *next_index);

/* pipe64.c: the bounded ring behind one pipe. fs64.c owns the descriptions and
 * the reference accounting; this module owns the bytes and the fill level.
 * `writer` selects the end for unref and for the reader/writer counts, so a
 * close of one end never disturbs the other. */
int pipe64_alloc(void);
void pipe64_unref(int index, int writer);
u32 pipe64_readable(int index);
u32 pipe64_writable(int index);
int pipe64_readers(int index);
int pipe64_writers(int index);
int pipe64_read(int index, void *dst, u32 count);
int pipe64_write(int index, const void *src, u32 count);

/* initramfs64.c: read /disk/INITRD and unpack its cpio "newc" archive into
 * the RAM root. When the archive is absent, the FAT12 volume becomes the
 * root instead, so an older image still boots. Returns 1 on success. */
int initramfs64_init(void);

/* module64.c: the x86-64 loader for the container in
 * kernel/module/haku_module.h. It accepts version 2 only, applies the two
 * 32-bit-word relocations module code produces, and exports logging, ticks,
 * the heap, device and filesystem registration, mount and the stat/dirent
 * fillers. Registration is only valid inside the init hook and is bound to
 * the loading slot; unload reports -EBUSY while any descriptor opened through
 * a device or mount that slot owns is still live. `file` is kernel memory
 * holding the whole .hkm image and is not retained. */
int module64_load(const u8 *file, u32 size);
int module64_unload(const char *name);
u32 module64_count(void);

/* syscall64.c: SYSCALL/SYSRET dispatch */
void syscall64_init(void);
regs64_t *sys64_dispatch(regs64_t *frame);
u64 sys64_count(void);
/* Build the same argc/argv/envp/auxv stack image execve produces for a
 * kernel-launched image. `path` becomes argv[0] and AT_EXECFN. */
int sys64_build_init_stack(loaded_elf64_t *loaded, const char *path);

/* idt64.c: gate table plus the C exception/IRQ dispatcher */
void idt64_init(void);
/* Load the shared IDT on an application processor. */
void idt64_load_ap(void);
regs64_t *isr64_dispatch(regs64_t *r);
u32 exc64_count(u32 vector);
u64 exc64_last_cr2(void);
u8 idt64_gate_ist(u32 vector);
u8 idt64_gate_type(u32 vector);
u64 idt64_gate_handler(u32 vector);

/* pic64.c: 8259 remap, masks and EOIs */
void pic64_remap(u8 master_base, u8 slave_base);
void pic64_mask_all(void);
void pic64_set_mask(u8 irq);
void pic64_clear_mask(u8 irq);
void pic64_send_eoi(u8 irq);

/* timer64.c: PIT on IRQ0 for the bootstrap processor, local-APIC timer for
 * the application processors. */
void timer64_init(u32 hz);
void timer64_tick(void);
u64 timer64_ticks(void);
u32 timer64_hz(void);
/* Measure the local APIC timer against the PIT tick counter once, on the
 * bootstrap processor with interrupts enabled. Returns counts/second. */
u32 timer64_lapic_calibrate(void);
u32 timer64_lapic_counts_per_sec(void);
/* Start the periodic local-APIC timer on the calling CPU. */
void timer64_lapic_start_local(void);

/* smp64.c: ACPI + local APIC discovery, AP bring-up, IPIs and TLB shootdown.
 * The APs come online parked; smp64_release_aps() hands them their idle task
 * and lets the per-CPU scheduler take over. Returns the online CPU count (1
 * when the machine stayed uniprocessor). */
int smp64_init(void);
u32 smp64_cpu_count(void);
/* Entry point the long-mode trampoline calls on the AP. */
void smp64_ap_entry(u64 cpu);
/* Start scheduling on every AP: build its idle task on the bootstrap
 * processor, then let the APs enter it. Called after sched64_init(). */
int smp64_release_aps(void);
/* Ask `cpu` to reschedule now (no-op for the caller's own CPU). */
void smp64_resched_cpu(u32 cpu);
/* Flush `space` from the TLB of every other CPU that could have it cached.
 * 0 means "every online CPU", which is what a full space teardown wants.
 * Returns the number of CPUs addressed. */
u32 smp64_tlb_shootdown(u64 space);
u32 smp64_tlb_timeouts(void);
/* The K64_VEC_TLB handler body: flush this CPU and acknowledge. */
void smp64_tlb_ipi(void);
/* From isr64.asm: set APICBASE.EN in IA32_APIC_BASE. */
void apic_msr_enable64(void);
/* From isr64.asm: atomic exchange, returns the previous word. */
u32 cpu_atomic_exchange64(volatile u32 *address, u32 value);

#endif
