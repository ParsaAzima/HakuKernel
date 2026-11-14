# HakuKernel Roadmap

HakuKernel is a small, freestanding x86 operating system: a NASM bootloader, a
C kernel built with MSVC or clang-cl, ring-3 programs, a Linux-inspired syscall
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
program. What is left in it is a TLS base and a third-party musl probe; the
next milestone that needs new kernel work is 2.

## Where the project stands

| Area | x86-64 (primary, `python build.py`) | i386 (`--arch=i386`) |
|---|---|---|
| Boot and memory | long-mode entry, four-level paging, 2 MiB physmap, frame allocator, 8 MiB heap | 32-bit protected mode, two-level paging, direct map |
| Interrupts | 64-bit IDT/TSS with IST, 8259 + PIT, RTC; SMP bring-up with per-CPU scheduling, LAPIC timers and TLB shootdown | same, including per-CPU scheduling on the APs (`--smp=1..4`) |
| Ring 3 | `SYSCALL`/`SYSRET` + `INT 0x80`, page-by-page user copies, NX | `INT 0x80`, page-by-page user copies |
| Processes | round-robin scheduler, `fork`/COW, `execve`, `wait4`, threads, `futex` | same, plus signals, job control and wait queues |
| Syscalls | ported subset on the x86-64 numbers; no pipes, no signal handlers | the subset a static musl/busybox userland needs, including pipes and signals |
| Storage | ATA PIO + FAT12 behind a page cache, file writes | ATA PIO + FAT12 + VFS with ramfs, devfs, procfs, initramfs |
| Display and input | shared VBE renderer (`drivers/fbcon.c`) drawing Terminus 8x16, VGA text fallback, PS/2 keyboard, status bar with uptime | the same renderer and font, plus the terminal line discipline and CSI handling |
| Modules | version 2 loader: log, ticks, heap; capability calls return `-ENOSYS` | version 1 loader with device and filesystem registration |
| Userland | `user/lib64` C library and `/INIT.ELF` from the FAT12 volume; no shell, no console device | freestanding user C library, `busybox ash`, probes |

Verified on every push by CI and locally:

```cmd
python build.py headless                  :: x64: prints "X64 CHECK PASSED"
python build.py headless --arch=i386      :: 32-bit: "HEADLESS CHECK PASSED"
python build.py shell --check             :: 32-bit busybox shell markers
```

A change is not done until both headless runs are green, the relevant
documentation under `docs/` describes the implemented subset, and any new
observable behavior has a marker of its own. Never claim a capability that the
marker set does not prove.

## Milestone map

| # | Milestone | Depends on | Exit marker |
|---|---|---|---|
| 1 | 64-bit userland foundation: libc, CRT, `/init` | — | `HAKU64-LIBC-OK` |
| 2 | 64-bit VFS, devfs and module capabilities | 1 | `HAKU64-VFS-OK` |
| 3 | Pipes, signals and job control | 1 | `HAKU64-PIPE-OK`, `HAKU64-SIGNAL-OK` |
| 4 | Interactive x86-64 userland: console, TTY, busybox | 2, 3 | `X64 SHELL CHECK PASSED` |
| 5 | True SMP scheduling — delivered on both architectures (see below) | — | `HAKU64-SMP-SCHED-OK`, `SMP-SCHED-OK` |
| 6 | Networking: loopback, then virtio-net | 4 | `NET-TCP-OK` |
| 7 | Hardening: SMEP/SMAP, W^X, syscall fuzzing | 2, 5 | `HAKU64-FUZZ-OK` |
| 8 | Tooling, tracing and performance measurement | any | `BENCH-OK` |

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
- [x] The 64-bit entry state: `argc`/`argv`/`envp` on the stack, and the
      minimal auxv (`AT_PAGESZ`, `AT_ENTRY`, `AT_NULL`) for `execve`. A forked
      child leads its own thread group, so `getpid()` reports the child.
- [x] The user library itself: string/memory, formatted output, a `brk`
      allocator, the file and process calls, `mmap`, `errno`, and the raw
      syscall stubs.
- [x] The user test is the first ring-3 program the boot test runs from the
      FAT12 volume: `user/tests/init64.c` as `/INIT.ELF`, started by the
      kernel, printed to the serial console, checked through
      `HAKU64-LIBC-OK` and its exit status.
- [ ] A small auxv-driven TLS: `arch_prctl(158)` or a mapping the program can
      put the `%fs` base in, so a compiler-generated `__declspec(thread)`
      variable works.
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

- [ ] `vfs64.c`: a mount table, path resolution, and an fd table that dispatches
      to the backend a mount names. `fs64.c` becomes the FAT12 backend rather
      than the whole filesystem.
- [ ] `ramfs64` as the boot root, `devfs64` with `null`, `zero` and `console`,
      and a minimal `procfs64` (`meminfo`, `uptime`, `self/cmdline`).
- [ ] An x86-64 initramfs: reuse `tools/mkcpio.py`, unpack in the kernel, and
      mount the archive as `/` with the disk at `/disk` — the same namespace
      shape the 32-bit image already uses.
- [ ] Enable the module capability exports: `haku_module_register_device`,
      `haku_module_register_fs`, `haku_module_mount`, `haku_module_fill_stat`
      and `haku_module_fill_dirent`. Keep the existing `-EBUSY` rule: unload
      fails while a descriptor opened through the module is still live.
- [ ] Port `hello.c` and `romfs.c` to the 64-bit build and run them from the
      disk, so the capability path is exercised by real C modules rather than
      stubs.
- [ ] Keep the build-time size assertion between `haku_module_fs_ops_t` and
      `vfs_fs_ops_t`, now for the 64-bit layouts too.

**Acceptance.** `HAKU64-VFS-OK`, `HAKU64-DEVFS-OK`, `HAKU64-INITRAMFS-OK`,
`HAKU64-MODULE-CAP-OK`; `/dev/hello` reads, `/rom` mounts and lists, and
`delete_module` reports `-EBUSY` until the last descriptor closes.

## 3. Pipes, signals and job control

**Goal.** The process model a shell expects: pipelines, handlers, stop and
continue, and a foreground process group tied to the console.

**Work.**

- [ ] `pipe`/`pipe2` with a bounded ring buffer, shared by both descriptors,
      EOF when the last writer closes, and `SIGPIPE` on a write with no reader.
      Wire pipes into `poll`/`select` and into `vfs_read`/`vfs_write` waits.
- [ ] Real signal delivery: `rt_sigaction` (13), `rt_sigprocmask` (14) and
      `rt_sigreturn` (15) on the x86-64 numbers, a user handler frame with a
      restorer, `sigaltstack` (131), and correct pending/masked state across
      `fork` and `execve`.
- [ ] Stop and continue: `SIGSTOP`/`SIGCONT`, `wait4` with `WUNTRACED`, and
      `setpgid`/`getpgrp`/`setsid` with the same rules the 32-bit side has.
- [ ] A console TTY: `TIOCGPGRP`/`TIOCSPGRP`, `TCGETS`/`TCSETS`, `SIGHUP` when
      the session leader exits, and `SIGTTOU`/`SIGTTIN` for background reads.
- [ ] Exit semantics: a thread group exits only when its last thread does, and
      the robust futex list is walked on exit.

**Acceptance.** `HAKU64-PIPE-OK`, `HAKU64-SIGNAL-OK`, `HAKU64-JOBCTL-OK`; a
ring-3 test runs a two-stage pipeline, catches and returns from a handler,
stops and resumes a child, and observes the foreground group change.

## 4. Interactive x86-64 userland (console, TTY, busybox)

**Goal.** `python build.py shell` boots the 64-bit image into `busybox ash`,
with the 32-bit image available as a fallback.

**Work.**

- [x] A 64-bit console: the shared VBE renderer (`kernel/drivers/fbcon.c`)
      draws the same Terminus 8x16 cells on both ports, VGA text stays the
      fallback when stage 2 hands over no linear framebuffer, and a status bar
      carries the machine identity and the uptime (`HAKU64-FB-OK`,
      `HAKU64-TERMINAL-OK`).
- [x] A PS/2 keyboard driver for x86-64 with the 32-bit set-1 scancode
      translation (`kbd64.c`, IRQ1) feeding a queue that console `read()`
      drains; an empty queue parks the task with `-EAGAIN` and IRQ1 wakes it
      (`HAKU64-KBD-OK`, `HAKU64-KEY-OK`).
- [ ] Line discipline and scrollback on the x86-64 console: erase, line
      buffering and history belong to a terminal layer rather than `read()`
      (which today only echoes), and the kernel log latch the 32-bit terminal
      has must exist here too.
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

| Release | Content |
|---|---|
| v0.3 | Milestones 1 and 2: a 64-bit libc, `/init`, VFS/devfs and capability modules |
| v0.4 | Milestones 3 and 4: pipes, signals, job control, and the 64-bit shell |
| v0.5 | Milestones 5 and 7: true SMP and the hardening pass |
| v0.6 | Milestones 6 and 8: networking, tracing and benchmarks |

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
