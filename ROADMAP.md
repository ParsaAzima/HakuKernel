# HakuKernel Roadmap

HakuKernel is a small, freestanding x86 operating system: a NASM bootloader, a  
C kernel built with LLVM's clang-cl and linked by lld-link, ring-3 programs, a  
Linux-inspired syscall  
ABI and loadable ring-0 modules. This roadmap covers the work after v0.2.0.  
x86-64 is the primary architecture, so the first goal is to make the 64-bit  
side carry a real userland; the later goals add capabilities neither side had  
(networking, hardening, measurement).

Every capability is expected to exist on both architectures: whatever lands on  
one port is a deliverable on the other, with a marker of its own.

Detailed history of everything that is already done lives in  
[the roadmap history](docs/roadmap-history.md). This file only tracks open  
work.

Milestone 1 is mostly landed: the x86-64 side has its own C library  
(`user/lib64`) and `/INIT.ELF` runs from the FAT12 volume as a real ring-3  
program. What is left in it is the third-party musl probe; the next milestone  
that needs new kernel work is 2.

## Where the project stands

| Area              | x86-64 (primary, `python build.py`)                                                                                                                                                                                                                                                                                                                            | i386 (`--arch=i386`)                                                           |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| Boot and memory   | long-mode entry, four-level paging, 2 MiB physmap, frame allocator, 8 MiB heap                                                                                                                                                                                                                                                                                 | 32-bit protected mode, two-level paging, direct map                            |
| Interrupts        | 64-bit IDT/TSS with IST, 8259 + PIT, RTC; SMP bring-up with per-CPU scheduling, LAPIC timers and TLB shootdown                                                                                                                                                                                                                                                 | same, including per-CPU scheduling on the APs (`--smp=1..4`)                   |
| Ring 3            | `SYSCALL`/`SYSRET` + `INT 0x80`, page-by-page user copies, NX                                                                                                                                                                                                                                                                                                  | `INT 0x80`, page-by-page user copies                                           |
| Processes         | round-robin scheduler, `fork`/COW, `execve`, `wait4` with `WUNTRACED`/`WCONTINUED`, threads and the pthread subset, `futex`, process groups and sessions, per-task signal dispositions with a handler frame and `sigaltstack`, `SIGSTOP`/`SIGCONT` with a stopped task state, `SIGTTIN` on a background console read and `SIGHUP` from a session leader's exit | same, plus job control and wait queues                                         |
| Syscalls          | ported subset on the x86-64 numbers, including pipes, a blocking `poll`, signals 1..31 (`rt_sigaction`/`rt_sigprocmask`/`rt_sigreturn`/`sigaltstack`) and the console `ioctl` surface                                                                                                                                                                          | the subset a static musl/busybox userland needs, including pipes and signals   |
| Storage           | ATA PIO + FAT12 at `/disk` behind a page cache, RAM initramfs root, procfs, devfs (including module devices), read-only module filesystems (`/rom`), file writes                                                                                                                                                                                               | ATA PIO + FAT12 + VFS with ramfs, devfs, procfs, initramfs                     |
| Display and input | shared VBE renderer (`drivers/fbcon.c`) drawing Terminus 8x16 as plain full-screen text with a 128-row scrollback ring, VGA text fallback, PS/2 keyboard with `0xE0` arrow/Home/End/Page keys, canonical line discipline (`tty64.c`) with 32-entry history and in-line editing, kernel-log display latch                                                       | the same renderer and font, plus the terminal line discipline and CSI handling |
| Modules           | version 2 loader: log, ticks, heap, device and filesystem registration; a module filesystem is mounted at its own prefix and read through the VFS                                                                                                                                                                                                              | version 1 loader with device and filesystem registration                       |
| Userland          | `user/lib64` C library and `/INIT.ELF` from the FAT12 volume; no shell, no console device                                                                                                                                                                                                                                                                      | freestanding user C library, `busybox ash`, the FAT12 rootfs image, probes     |

CI runs the two headless gates at one and four CPUs on every push; the shell  
and rootfs checks are local gates:

```cmd
python build.py headless                  :: x64: prints "X64 CHECK PASSED"
python build.py headless --arch=i386      :: 32-bit: "HEADLESS CHECK PASSED"
python build.py shell --check             :: 32-bit busybox shell markers
python build.py rootfs --check            :: 32-bit rootfs (init + busybox) markers
```

A change is not done until both headless runs are green, the relevant  
documentation under `docs/` describes the implemented subset, and any new  
observable behavior has a marker of its own. Never claim a capability that the  
marker set does not prove.

## Milestone map

| # | Milestone                                                         | Depends on | Exit marker                           |
| - | ----------------------------------------------------------------- | ---------- | ------------------------------------- |
| 1 | 64-bit userland foundation: libc, CRT, `/init`                    | —          | `HAKU64-LIBC-OK`                      |
| 2 | 64-bit VFS, devfs and module capabilities                         | 1          | `HAKU64-VFS-OK`                       |
| 3 | Pipes, signals and job control                                    | 1          | `HAKU64-PIPE-OK`, `HAKU64-SIGNAL-OK`  |
| 4 | Interactive x86-64 userland: console, TTY, busybox                | 2, 3       | `X64 SHELL CHECK PASSED`              |
| 5 | True SMP scheduling — delivered on both architectures (see below) | —          | `HAKU64-SMP-SCHED-OK`, `SMP-SCHED-OK` |
| 6 | Networking: loopback, then virtio-net                             | 4          | `NET-TCP-OK`                          |
| 7 | Hardening: SMEP/SMAP, W^X, syscall fuzzing                        | 2, 5       | `HAKU64-FUZZ-OK`                      |
| 8 | Tooling, tracing and performance measurement                      | any        | `BENCH-OK`                            |

Once their prerequisites are met, milestones 5 to 8 can land in any order. The  
order above is the recommended one: it turns the 64-bit kernel into the default  
system before adding new surface area.

## 1. 64-bit userland foundation (libc, CRT, `/init`)

**Goal.** A C program compiled for x86-64 runs at ring 3, calls the kernel  
through the library, and reaches the disk, without the kernel self-test doing  
the work on its behalf.

**Work.**

- [x] Split the user library along the ABI instead of `#ifdef`-ing one tree.  
  The structures and the syscall entry differ at every level, so x86-64  
  gets its own `user/lib64` (documented in  
  [the x86-64 C library](docs/libc64.md)) and `user/lib` stays 32-bit.
- [x] Add a 64-bit C ABI header (`include/haku/abi64.h`) with the x86-64  
  `struct stat` (144 bytes), `iovec`, `timespec` and syscall numbers, and  
  keep `include/haku/abi64.inc` in step with it; the build's ABI-sync  
  check covers both architectures.
- [x] The 64-bit entry state: `argc`/`argv`/`envp` on the stack and the  
  Linux auxiliary vector (`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_PAGESZ`,  
  `AT_BASE`, `AT_FLAGS`, `AT_ENTRY`, the identity words, `AT_SECURE`,  
  `AT_RANDOM`, `AT_EXECFN`, `AT_HWCAP`). The kernel's own `/INIT.ELF`  
  launch uses the same builder `execve` does, so both paths agree. A  
  forked child leads its own thread group, so `getpid()` reports the  
  child.
- [x] The user library itself: string/memory, formatted output, a `brk`  
  allocator, the file and process calls, `mmap`, `errno`, and the raw  
  syscall stubs.
- [x] The user test is the first ring-3 program the boot test runs from the  
  FAT12 volume: `user/tests/init64.c` as `/INIT.ELF`, started by the  
  kernel, printed to the serial console, checked through  
  `HAKU64-LIBC-OK` and its exit status.
- [x] A small auxv-driven TLS: `arch_prctl(158)` or a mapping the program can  
  put the `%fs` base in, so a compiler-generated `__declspec(thread)`  
  variable works. The x86-64 kernel serves `ARCH_SET_FS`/`ARCH_SET_GS`  
  and `ARCH_GET_FS`/`ARCH_GET_GS`, keeps the bases per task, and reloads  
  them on every switch; `user/lib64/tls64.c` finds `PT_TLS` through  
  `AT_PHDR`, copies the template, builds a TEB/TlsSlots block and installs  
  it as the user GS base before `main()`. `HAKU64-USER-TLS-OK` covers the  
  initializer, zero fill, read/write, both base calls and fork isolation.
- [x] Threads with TLS: `clone` honours `CLONE_SETTLS` (fifth argument becomes  
  the child's FS base), and `user/lib64/thread64.c` builds a thread from a  
  64 KiB stack plus a freshly copied TLS template and TEB.  
  `haku64_thread_entry` installs GS in the child, `_exit` ends one thread,  
  `exit_group` ends the group, and `HAKU64-USER-THREAD-OK` checks the  
  per-thread block, the shared pid and `waitpid(tid)`.
- [x] Reclaimable threads: `clone` also honours `CLONE_CHILD_SETTID` and  
  `CLONE_CHILD_CLEARTID` (with or without `CLONE_VM`), publishing the tid in  
  the child's space before it runs and zeroing the word with one futex wake  
  at exit. `haku64_thread_spawn`/`haku64_thread_join` keep the handle inside  
  the thread's own stack, sleep on that word, then reap and unmap, so a  
  joined thread costs no allocation and leaks nothing  
  (`HAKU64-USER-THREAD-JOIN-OK`). `kill` is process-directed, so the signal  
  goes to the group leader (or the first live thread when it is gone),  
  while `tgkill` stays per task and validates the group id. A thread can  
  `execve`: the rest of the group ends and the caller keeps the address  
  space (`HAKU64-USER-THREAD-EXEC-OK`), and a ring-3 fault ends the group  
  rather than leaving a sibling in a broken space.
- [x] Detached threads that clean up after themselves: `clone` accepts  
  `CLONE_DETACH` (a Haku flag, valid only with `CLONE_THREAD`), and the CPU  
  that ran the task reclaims its slot at the next tick or switch instead of  
  keeping a waitable zombie. `haku64_thread_create` is now such a thread —  
  it frees its own TLS block and unmaps its own stack on return instead of  
  leaking both until process exit — and the kernel's detached reap is  
  checked by `HAKU64-USER-DETACH-OK` on x86-64 and `USER-DETACH-OK` on  
  i386.
- [x] The pthread subset of `include/haku/pthread.h` on both ports:  
  create/join/detach/exit, a robust mutex (`lock`, `trylock`, `unlock`,  
  `destroy`, `consistent`, `EOWNERDEAD`, poisoning) and a condition  
  variable. Every thread is a detached kernel task with a control-block  
  page and a 128 KiB stack, and a join or detach claims it with a  
  `FINISHED → REAPED` compare-and-swap *after* the kernel zeroes its tid  
  inside the exit syscall, so the pages cannot be pulled out from under a  
  thread still issuing that call. Robust-list heads live in a static table  
  indexed by task id, because a thread frees its control block first; the  
  kernel walks the list on exit, bounded by 64 nodes, and marks each owned  
  futex `OWNER_DIED` with one wake. `HAKU64-USER-PTHREAD-OK`/  
  `USER-PTHREAD-OK` and `HAKU64-USER-ROBUST-OK`/`USER-ROBUST-OK` cover it.
- [x] Thread-safe `printf` and `errno`. The formatter builds every line in a  
  buffer on the caller's stack instead of one shared static buffer  
  (`HAKU64-USER-ERRNO-THREAD-OK`/`USER-ERRNO-THREAD-OK` also prove that one  
  thread's `errno` write never reaches its creator: i386 keeps it at  
  `%fs:4`, x86-64 reads GS:32 from the TEB).
- [x] `kill` follows the POSIX pid rules on both ports: pid 0 is the caller's  
  process group, -1 every other task, and any other negative pid the  
  members of that group.
- [ ] The x86-64 third-party probe: the i386 suite builds a static musl  
  binary and runs it; the 64-bit side should prove the same thing with an  
  `x86_64-linux-musl` build, so the ABI is tested by code the project did  
  not write.


**Acceptance.** `HAKU64-LIBC-START`, `HAKU64-LIBC-OK` and  
`HAKU64-LIBC-RUN-OK` in the x64 serial log (the existing `HAKU64-USER-OK`  
keeps covering the small hand-written ring-3 test); the i386 suite unchanged.  
The check list is now in the marker set, so a regression in the library fails  
the build instead of only changing an exit status.

## 2. 64-bit VFS, devfs and module capabilities

**Goal.** One namespace with more than one backend, so the 64-bit image can  
have a root filesystem, a `/dev`, and modules that publish devices and  
filesystems.

**Work.**

- [x] `vfs64.c`: a mount table, path resolution, and an fd table that dispatches  
  to the backend a mount names. `fs64.c` becomes the FAT12 backend rather  
  than the whole filesystem.
- [x] devfs with `null`, `zero` and `console` mounted at `/dev`; the namespace  
  refuses create, unlink, rename and rmdir below a mount that is not the  
  disk.
- [x] `ramfs64` as the boot root. A fixed node table whose pages are the very  
  frames the page cache aliases, so `read`, `write` and `mmap` stay  
  coherent without a copy.
- [x] `procfs64` at `/proc`: `meminfo`, `uptime` and `self/cmdline` are  
  generated at open time into the cache page, and every mutation answers  
  `-EACCES`. The ring-3 test reads all three.
- [x] `X64MOD.HKM` registers `/dev/hello` through  
  `haku_module_register_device`; the boot test reads its marker, proves  
  `delete_module` reports `-EBUSY` while a descriptor is open, and checks  
  the node is detached on unload.
- [x] An x86-64 initramfs: `tools/mkcpio.py` builds `INITRD`, `initramfs64.c`  
  unpacks it into the RAM root through the descriptor path, and the disk  
  stays at `/disk` — the same namespace shape the 32-bit image uses. An  
  image without the archive falls back to FAT12 as `/`.
- [x] Wire the remaining module capability exports:  
  `haku_module_register_fs`, `haku_module_mount` and the two struct  
  fillers. Keep the existing `-EBUSY` rule for devices and apply it to a  
  filesystem whose mount still has open descriptions.
- [x] Port `hello.c` and `romfs.c` to the 64-bit build and run them from the  
  disk, so the capability path is exercised by real C modules rather than  
  stubs. `X64MOD.HKM` covers the device half; `ROMFS.HKM` is the same  
  `romfs.c` compiled for amd64 and mounted at `/rom`.
- [x] Keep the build-time size assertion between `haku_module_fs_ops_t` and  
  `vfs64_fs_ops_t` (`module64.c`).

**Acceptance.** `HAKU64-VFS-OK`, `HAKU64-DEVFS-OK`, `HAKU64-PROCFS-OK`,  
`HAKU64-USER-DEV-OK`, `HAKU64-USER-PROCFS-OK`, `HAKU64-INITRAMFS-OK`,  
`HAKU64-USER-INITRAMFS-OK`, `HAKU64-MODULE-DEV-OK`, `HAKU64-MODULE-FS-OK`,  
`HAKU64-USER-MODULE-OK` and `HAKU64-MODULE-FS-RELEASE-OK`; `/dev/hello` reads,  
`/rom` is read and listed from ring 0 and ring 3 (including the `-EROFS`  
refusals), and `delete_module` reports `-EBUSY` for either resource until the  
last descriptor closes.

## 3. Pipes, signals and job control

**Goal.** The process model a shell expects: pipelines, handlers, stop and  
continue, and a foreground process group tied to the console.

**Work.**

- [x] `pipe`/`pipe2` with a bounded ring buffer, shared by both descriptors,  
  EOF when the last writer closes, and `SIGPIPE` on a write with no reader.  
  Eight 4 KiB rings, counted per open description so `dup`/`fork` share  
  one end, blocking reads and writes in the halt loop, and readiness wired  
  into `poll`/`select`. Proven by `HAKU64-USER-PIPE-OK` (ring, EOF,  
  `poll`, `O_NONBLOCK`) and `HAKU64-USER-PIPE-SIGPIPE-OK` (a forked  
  child's write with no reader ends in the default `SIGPIPE` action) and  
  `HAKU64-USER-PIPE-BLOCK-OK` (a write larger than the ring blocks until  
  the reader drains it).
- [x] Real signal delivery: `rt_sigaction` (13), `rt_sigprocmask` (14) and  
  `rt_sigreturn` (15) on the x86-64 numbers, a user handler frame with a  
  restorer, `sigaltstack` (131), and correct pending/masked state across  
  `fork` and `execve`. Each task owns its dispositions and mask; a signal is  
  queued and delivered on the way back to ring 3, so a handler frame returns  
  through `IRETQ` (SYSRET would clobber `RCX`/`R11`). Handler arguments use  
  the Microsoft x64 convention (`RCX` = signal) because that is what the  
  toolchain compiles. Proven by `HAKU64-USER-SIGNAL-OK`.
- [x] Stop and continue: `SIGSTOP`, `SIGTSTP`, `SIGTTIN` and `SIGTTOU` park  
  the task in a `K64_TASK_STOP` state, `SIGCONT` (and `SIGKILL`) make it  
  runnable again, `wait4` reports the stop once under `WUNTRACED` and the  
  resume once under `WCONTINUED`, and the stop state is per task like the  
  rest of this port's signal state. Proven by `HAKU64-USER-STOP-OK`.
- [x] The console's terminal surface: `TIOCGPGRP`/`TIOCSPGRP` against a live  
  foreground group, `TCGETS`/`TCSETS*`, `TCSBRK`/`TCFLSH`, `TIOCGWINSZ`,  
  `FIONREAD` and `ENOTTY` everywhere else, with an `ioctl` wrapper in  
  `user/lib64`. Proven by `HAKU64-USER-TTY-OK`.
- [x] Terminal-generated signals: a console read from a process group that is  
  not the foreground one raises `SIGTTIN` for the group and fails with  
  `-EINTR` (or `-EIO` when the signal is blocked or ignored), and a ring-3  
  session leader's exit raises `SIGHUP` for the console's foreground  
  group. `SIGTTOU` has no trigger because the console does not set  
  `TOSTOP`, so a background write runs. Proven by  
  `HAKU64-USER-TTY-SIGNAL-OK`.
- [x] Exit semantics: a thread group exits only when its last thread does (a  
  leader that exits hands the address space, heap and mappings to a  
  survivor), and `set_robust_list`/`get_robust_list` let the kernel walk a  
  thread's robust futex list on exit, bounded by 64 nodes, marking each  
  owned word `OWNER_DIED` and waking a waiter.

**Acceptance.** `HAKU64-PIPE-OK`, `HAKU64-SIGNAL-OK`, `HAKU64-JOBCTL-OK`; a  
ring-3 test runs a two-stage pipeline, catches and returns from a handler,  
stops and resumes a child, and observes the foreground group change. The pipe,  
handler, stop/continue, TTY and terminal-signal quarters are done:  
`HAKU64-USER-PIPE-OK`, `HAKU64-USER-PIPE-SIGPIPE-OK`,  
`HAKU64-USER-SIGNAL-OK`, `HAKU64-USER-STOP-OK`, `HAKU64-USER-TTY-OK` and  
`HAKU64-USER-TTY-SIGNAL-OK` run in `/INIT.ELF`; the line discipline landed in  
milestone 4 (`tty64.c`), and the remaining work is the wait queues a real  
shell drives.

## 4. Interactive x86-64 userland (console, TTY, busybox)

**Goal.** `python build.py shell` boots the 64-bit image into `busybox ash`,  
with the 32-bit image available as a fallback.

**Work.**

- [x] A 64-bit console: the shared VBE renderer (`kernel/drivers/fbcon.c`)  
  draws the same Terminus 8x16 cells on both ports as plain full-screen  
  text, VGA text stays the fallback when stage 2 hands over no linear  
  framebuffer (`HAKU64-FB-OK`, `HAKU64-TERMINAL-OK`).
- [x] A PS/2 keyboard driver for x86-64 with the 32-bit set-1 scancode  
  translation (`kbd64.c`, IRQ1) feeding a queue that console `read()`  
  drains; an empty queue parks the task with `-EAGAIN` and IRQ1 wakes it  
  (`HAKU64-KBD-OK`, `HAKU64-KEY-OK`).
- [x] A canonical line discipline on the x86-64 console (`tty64.c`): Enter  
  commits an echoed line, Backspace/`VERASE` erase the last character,  
  `^D` is EOF on an empty line, `^C` raises `SIGINT` for the foreground  
  group, and `TCFLSH` drops queued input  
  (`HAKU64-USER-TTY-LINE-OK`, `HAKU64-USER-TTY-EOF-OK`,  
  `HAKU64-USER-TTY-INTR-OK`). The kernel log  
  latch exists too: `console64_handover()` sends later kernel markers to  
  COM1 only, while `k64_fail` reclaims the screen.
- [x] Scrollback and history on the x86-64 console. `fbcon.c` copies the  
  departing row into a 128-row ring before scrolling and repaints the  
  grid from scrollback plus the live shadow on demand; any putc returns  
  to the live view first (`fbcon_scroll_view`). `kbd64.c` keeps the `0xE0`  
  prefix as state and delivers arrow, Home/End and Page Up/Down markers  
  above `0x7F`, and `tty64.c` consumes them: Up/Down walk a 32-entry  
  history ring with the in-progress line staged, Left/Right/Home/End  
  move an insertion point, and Page Up/Down review the scrollback  
  (`HAKU64-HIST-OK`, `HAKU64-HIST-RECALL-OK`, `HAKU64-HIST-CYCLE-OK`,  
  `HAKU64-SCROLL-UP`, `HAKU64-SCROLL-DOWN`, `HAKU64-SCROLL-OK`). Wrapped  
  lines are redrawn in full: both display backends gained relative cursor  
  moves, the line editor remembers the line's start cell, and a boot-time  
  self-test checks the cursor and the text after a mid-line edit  
  (`HAKU64-TTY-REDRAW-OK`).
- [x] Large executables: `execve` now streams the file into one heap buffer  
  (`fs64_load_image`) instead of going through the 128 KiB page cache, and  
  `elf64_load` accepts up to 384 pages (1.5 MiB) per image including the  
  four-page stack, so a static musl binary fits. `/disk/USER.ELF` carries  
  320 KiB of filler and prints `HAKU64-EXEC-LARGE` from its last page,  
  which proves the FAT12 read, the loader and the mapping of an image  
  bigger than a cache slot.
- [ ] Build a static x86-64 musl busybox in CI (`tools/getbusybox.py` or a  
  prebuilt artifact) and place it in the initramfs with `/init` reading  
  `INIT.CMD` when present.
- [ ] Make `shell`, `shell --check` and `probe` default to x86-64; `--arch=i386`  
  keeps the old image for comparison. Delete the "these modes force i386"  
  special case from `build.py`.
- [ ] Re-verify the shell markers on the 64-bit image: prompt, `ls`, `cat`,  
  `ps`, a forked pipeline and a PATH lookup.

**Acceptance.** `X64 SHELL CHECK PASSED` and `HAKU64-INIT-OK`; `run` starts a  
usable interactive shell without serial; the i386 suite stays green.

## 5. True SMP scheduling

**Goal.** The application processors stop being parked: tasks run on every CPU,  
with per-CPU scheduling state and correct cross-CPU memory invalidation.

**Work.** Delivered in four stages on both architectures; A–C (AP bring-up,  
atomic locks, per-CPU descriptor tables) are described in  
[architecture](docs/architecture.md#smp-acpi-the-local-apic-and-application-processors).

- [x] Per-CPU state: current task, idle task, round-robin cursor, tick and  
  switch counters, active address space, and a GS-based accessor  
  (`k64_cpu()` on x86-64, `percpu_self()` through GDT entry 7 on 32-bit)  
  instead of globals.
- [x] A local-APIC timer per CPU, calibrated once against the PIT, plus IPIs  
  for reschedule and TLB shootdown (vectors `0x40`–`0x42`).
- [x] Scheduling on APs: each CPU picks, switches and idles (`HLT`)  
  independently, and each AP owns an idle task. On 32-bit a kernel worker  
  hands its slot back (`scheduler_kernel_exit`/`scheduler_join_kernel`)  
  because the process table only has four entries.
- [x] Lock audit: one interrupt-owning spin lock, documented as such; the  
  scheduler, frame allocator, heap, TLB round and disk mutex use it and  
  nothing blocking happens under it. The 32-bit scheduler adds  
  `sched_lock` for slot reservation/reaping only.
- [x] TLB shootdown on `munmap`, `mprotect` and COW breaks that touch another  
  CPU's address space, targeting only CPUs whose CR3 names the space.
- [x] A one-CPU mode that still passes (`--smp=1`) so the scheduler can be  
  bisected, on both architectures.
- [ ] User tasks on APs: pin a ring-3 task to an AP and prove it survives  
  preemption, syscalls and exit there, so the syscall layer's bookkeeping  
  (current task, `FS`-relative TLS, signal delivery) is exercised on more  
  than the bootstrap processor. On 32-bit this also needs per-AP user  
  address-space switching and a wider process table.
- [ ] Run queues instead of a pinned scan, with wake-up placement and  
  migration, once tasks may move between CPUs.
- [ ] PCID/ASID tagging, so a shootdown can flush a single address space  
  instead of reloading CR3.


**Acceptance.** `HAKU64-SMP-SCHED-OK`/`SMP-SCHED-OK` (a pinned worker per AP  
proves every CPU dispatches from the shared task table) and  
`HAKU64-SMP-TLB-OK`/`SMP-TLB-OK` (`targets=` equals the number of other CPUs,  
`flushes=` matches, `timeouts=0`), with the whole suite green at `--smp=1`  
(`…-SKIP` markers), `--smp=2` and `--smp=4` on both architectures. Met.

## 6. Networking

**Goal.** A static userland program can talk to another host: first over  
loopback, then over QEMU's user-mode network with a virtio-net card.

**Work.**

- [ ] Socket descriptors inside the fd table: `socket`, `bind`, `connect`,  
  `listen`, `accept`, `send`, `recv`, `shutdown` for `AF_INET`/`SOCK_STREAM`  
  and `SOCK_DGRAM`; `poll` works on them.
- [ ] Loopback interface and the stack beneath it: ARP, IPv4 with checksums,  
  ICMP echo, UDP, and a TCP subset (handshake, in-order data, ACKs,  
  retransmission timer, close).
- [ ] `virtio-net` over PCI: modern virtio 1.0, RX/TX virtqueues, interrupt  
  handling, and MAC/MTU discovery.
- [ ] Configuration without DHCP first: a static address from the boot command  
  line or an `/etc/network` file, with `ifconfig`-style reporting through  
  `ioctl`.
- [ ] `busybox ping` and a plain HTTP `GET` via `wget` against a host service.

**Acceptance.** `NET-LOOPBACK-OK`, `NET-VIRTIO-OK` and `NET-TCP-OK`;  
`ping 10.0.2.2` succeeds and a small HTTP object is fetched inside QEMU.

## 7. Hardening

**Goal.** Narrow the ring-0 attack surface that userland, syscalls and modules  
now expose.

**Work.**

- [ ] Enable SMEP and SMAP when CPUID offers them (with a `qemu64` fallback),  
  and use `stac`/`clac` around the user-copy helpers only.
- [ ] W^X in the kernel: map text read-only/executable, data and heap  
  non-executable, and stop allocating module images from an executable  
  heap — load them into an RX mapping after relocation instead.
- [ ] Guard pages below user stacks and kernel stacks, and a checked  
  `K64_STACK_SIZE` boundary in the context switch.
- [ ] A deterministic syscall fuzz pass in headless: every dispatched call is  
  invoked with hostile pointers, sizes and descriptors from a fixed seed  
  table, and no input may panic the kernel.
- [ ] Document the module trust model precisely: a container check is not a  
  sandbox, and loading a module is a privileged action.

**Acceptance.** `HAKU64-SMEP-OK`, `HAKU64-WX-OK`, `HAKU64-FUZZ-OK`; a module  
whose text page is written after load faults instead of silently executing.

## 8. Tooling, tracing and performance

**Goal.** Make behavior measurable and failures diagnosable, instead of relying  
on a serial log and eyeballing.

**Work.**

- [ ] `python build.py bench`: syscall round-trip, `fork`+`execve` cost,  
  context-switch rate, page-fault counts, and FAT12 read throughput, with  
  the same workload on both architectures.
- [ ] A kernel trace ring buffer (syscall, IRQ, scheduler events) exposed at  
  `/proc/trace` or as a device, plus `tools/trace.py` to decode it.
- [ ] `tools/fsck.py`: FAT mirror comparison, chain validation, orphan and  
  cross-link detection, and a repair mode that refuses to guess.
- [ ] Symbolized panics: `tools/symbolize.py` reads `build/kernel.map` and turns  
  a fault report into function names, on the serial log and in the crash  
  screenshot.
- [ ] A `docs/performance.md` page with the measured numbers, the QEMU flags  
  used, and an explicit note that they are not hardware benchmarks.

**Acceptance.** `BENCH-OK` with a printed table, `TRACE-OK`, and a deliberately  
faulting test whose report contains symbolized frames.

## Release train

| Release | Content                                                                      |
| ------- | ---------------------------------------------------------------------------- |
| v0.3    | Milestones 1 and 2: a 64-bit libc, `/init`, VFS/devfs and capability modules |
| v0.4    | Milestones 3 and 4: pipes, signals, job control, and the 64-bit shel         |
| v0.5    | Milestones 5 and 7: true SMP and the hardening pass                          |
| v0.6    | Milestones 6 and 8: networking, tracing and benchmarks                       |

Dates are deliberately absent: a milestone ships when its acceptance markers  
are green and its limitations are documented.

## Out of scope for this roadmap

- Architectures other than x86-64 and i386 (ARM, RISC-V), and 32-bit PAE.
- USB, audio, GPU acceleration, and a desktop environment.
- Full POSIX/glibc compatibility, a dynamic linker, and shared libraries; the  
  userland target stays a statically linked musl userland.
- Journaling, transactional filesystems, and power-loss guarantees.
- In-kernel TLS or an HTTP server; networking stops at the IPv4 subset above.
- Virtualization and container hosting.

## Delivery process

Keep each change small and reversible, update the matching document under  
`docs/`, and validate with both headless runs. An item moves from this file to  
[the history](docs/roadmap-history.md) only once it has a repeatable test, its  
markers are in the required list, and its limitations are written down.
