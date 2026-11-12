# Processes and scheduling

[Back to documentation index](README.md)

Process creation, copy-on-write `fork`, `vfork`, `execve`, the preemptive
scheduler, job control, and per-process resource accounting.

## Processes and preemptive scheduling

`kernel/kernel/scheduler.c` starts the initial process from `HAKU.ELF`; a successful
`spawn` creates a child process from a root-filesystem ELF, `fork` creates one
as a copy of the caller, and `waitpid`/`wait4` suspends the parent until the
requested child exits. Exit status is returned in the traditional high-byte
wait status format; a successful wait reaps the child record. The current
spawn interface accepts a pathname only and starts the child with empty
argument and environment vectors; the child inherits a copy of the parent's
descriptor table (standard descriptors and any open files, each sharing the
parent's open file description). `waitpid` accepts a specific child PID or
`-1` (any child) and takes no options; `wait4` follows the Linux call,
accepting `WNOHANG` and writing a zeroed `struct rusage` (the per-process
accounting described below is not yet wired into it). `WUNTRACED` and
`WCONTINUED` are accepted and ignored: a task never enters a stopped state,
but a job-control shell passes `WUNTRACED` to every `waitpid`. Each task
records a process group and a session, inherited across `fork`; `setpgid`
moves the caller or one of its children into a live group, `setsid` makes the
caller a leader of both, and `kill` with a negative pid signals a whole
group. That is what lets `ash` run a foreground pipeline: it puts both
children in a fresh group and hands that group to the terminal.
`exit_group` ends the calling process like `exit`. `clone` accepts the
thread-creating form: with `CLONE_VM` the child shares the caller's address
space, `CLONE_FILES` shares the descriptor row, and `CLONE_THREAD` puts the
child in the caller's thread group, so `getpid` stays the same while
`gettid` differs. Without `CLONE_VM` it behaves exactly like `fork`. A
thread costs a task slot, and the group's heap break and `mmap` ledger live
in one owner slot that every thread reads (`space_state`), so `brk`,
`mmap`, `mprotect`, and `munmap` in one thread are visible to the others.
`exit` ends only the calling thread; `exit_group`, a fatal signal, and
`execve` end the other threads, and when the owner leaves, the group id and
the address space move to a surviving thread. A forked child gets a private
copy
of the address space (code, data, stacks, heap, and mappings), a copy of the
caller's saved register frame with `eax` zeroed, and its own descriptor table;
the parent sees the child PID. Pages are shared copy-on-write: each frame
carries a reference count, both address spaces keep the same read-only
mapping, and the first write on either side takes a protection fault that
hands the writer a private copy (the kernel also breaks the share before
writing through the direct map on the caller's behalf). `vfork` goes further
and shares the address space outright: the child runs in the parent's space
and on its stack while the parent stays suspended, and the parent resumes
with the child PID as soon as the child calls `execve` or exits. `execve`
replaces a process image from the root filesystem.
Each process has its own address space and an 8 KiB ring-0 stack; the stack
is allocated as physical frames and addressed through the kernel direct map.
`gdt_set_kernel_stack` updates `TSS.esp0` at every task switch so the next
ring-3 interrupt or syscall enters on the correct stack. The 100 Hz PIT
scheduler saves and restores complete interrupt frames through `iret`.
User exceptions and `exit` mark a process for reaping; its address space and
kernel stack are reclaimed after execution has moved to another stack. When
no user process is runnable, the scheduler runs a dedicated kernel idle
context.

Scheduling is single-CPU. Both builds bring the application processors online,
give each one a private descriptor table and TSS plus the shared IDT, and park
them (see [SMP](architecture.md#smp-acpi-the-local-apic-and-application-processors)).
Only the bootstrap processor takes timer interrupts and selects the next task,
so the scheduler, wait queues and run-state bitmap still need no per-CPU
replication. Letting an AP schedule requires per-CPU current-task state and a
per-CPU timer; that is the next SMP stage in the
[roadmap](../ROADMAP.md#4-concurrency-and-memory-management).

`kernel/arch/x86/kernel/gdt.c` installs flat ring-0/ring-3 code and data descriptors plus a
32-bit TSS. `kernel/arch/x86/mm/paging.c` gives each process supervisor-only kernel
identity and direct-map entries and maps its user program and two-page user
stack with user permissions.

`kernel/arch/x86/user/user.asm` is built as an ELF32 image and entered via `iret` at CPL 3.
It tests `spawn` (`0x1000`, Haku-specific), `fork` (2), `wait4` (114), and
`waitpid` (7), checks that a forked child really shares pages copy-on-write,
runs `vfork` both to a plain `exit` and across an `execve`, as well as
`execve` (11), which loads another ELF32 image, builds an aligned
initial user stack containing `argc`, `argv`, `envp`, and a terminating
`AT_NULL` auxiliary-vector entry, and replaces the calling process without
changing its PID or open file descriptors. Each vector is limited to 64
strings; their strings are limited to 4096 combined bytes including NUL
terminators and must fit the two-page initial stack. Larger inputs return
`E2BIG`, and invalid user pointers return `EFAULT`. The first process starts
on a stack built the same way, with `argv[0]` set to the program name, so a
binary written for Linux finds its arguments where its startup code expects
them. The auxiliary vector carries `AT_PHDR`, `AT_PHENT`, and `AT_PHNUM`
(where the program headers are, which a static libc walks to find its own
`PT_TLS` segment), `AT_PAGESZ`, `AT_CLKTCK`, `AT_BASE`, `AT_FLAGS`,
`AT_ENTRY`, the identity words, `AT_SECURE`, and `AT_RANDOM` pointing at 16
seed bytes. Haku's own images start their first segment at a page-aligned
file offset, so the header table is not part of any segment; the loader maps
a copy on a page just below the stack and points `AT_PHDR` there, which is
what keeps the vector meaningful for every image.

## Per-process memory and resource accounting

`paging_map_user_page` and `paging_unmap_user_page` maintain per-address-space
counters for present user pages, non-directory user page tables, and a
mapped-page high-water mark. ELF loading, `brk`, `mmap`, and `munmap` all go
through those helpers, so the counters follow every mapping change without a
table walk. The scheduler attributes each 100 Hz timer tick to the interrupted
context by its CS privilege level, counts syscalls at the `int 0x80` entry,
and records a voluntary switch when a task blocks, sleeps, or waits and an
involuntary switch when the timer moves a runnable task aside. `execve`
preserves the process's peak and resource totals while replacing the current
image's memory counters with those of the new address space.

`getusage` (0x1001, Haku-specific) copies a 64-byte `haku_usage_t` to a
validated user buffer. It reports current and peak mapped pages, page-table
and kernel-stack pages, current `brk` and anonymous `mmap` bytes, and derived
resident/kernel/total byte counts, along with the pid, user/kernel timer
ticks, syscall count, and voluntary/involuntary context switches.
`resident_bytes` covers mapped user pages; `kernel_bytes` covers the user page
tables, the page directory, and the 8 KiB kernel stack; `total_bytes` is their
sum. A NULL or unmapped buffer returns `EFAULT`. Memory accounting is per
address space (a thread reads the owner slot), while the timer and syscall
counters are per task, there is no demand paging
apart from the user stack, and ticks have 10 ms granularity.
