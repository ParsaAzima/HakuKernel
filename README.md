<h1 align="center">
 Haku v0.2.0 — a tiny x86-64 operating system in C
</h1>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/Assembly-NASM-333333?style=for-the-badge" alt="NASM" />
  <img src="https://img.shields.io/badge/Architecture-x86--64%20%2F%20i386-0071C5?style=for-the-badge" alt="x86-64 and i386" />
  <img src="https://img.shields.io/badge/Kernel-Bare%20Metal-222222?style=for-the-badge" alt="Bare Metal" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Compiler-Clang--CL-5C2D91?style=for-the-badge&logo=llvm&logoColor=white" alt="Clang-CL" />
  <img src="https://img.shields.io/badge/Linker-LLD-68217A?style=for-the-badge" alt="LLD" />
  <img src="https://img.shields.io/badge/Build-Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python" />
  <img src="https://img.shields.io/badge/Emulator-QEMU-FF6600?style=for-the-badge&logo=qemu&logoColor=white" alt="QEMU" />
</p>

<p align="center">
  <a href="https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/ParsaAzima/HakuKernel/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI status" />
  </a>
  <img src="https://img.shields.io/badge/Filesystem-FAT12-4CAF50?style=for-the-badge" alt="FAT12" />
  <img src="https://img.shields.io/badge/ABI-x86--64%20%2F%20i386%20Syscalls-455A64?style=for-the-badge" alt="x86-64 and i386 syscall ABI" />
</p>

A NASM bootloader loads a C kernel, switches the CPU to long mode and runs
ELF64 programs at ring 3 behind the Linux x86-64 syscall ABI. The same source
tree still builds the original 32-bit kernel (`python build.py --arch=i386`),
which is where the static musl/busybox userland, the user C library and the
shell live. There is no C runtime inside the kernel and no hosted library
under it: LLVM's `clang-cl` and NASM produce freestanding objects that
`lld-link` combines into a flat boot image, and QEMU boots it.

The practical target is a small, honest POSIX-*inspired* subset that a real
static Linux binary can use. CI boots both ports headless and checks their
marker sets on every push, and the `python build.py shell` target brings up a
1 MiB static **busybox** as PID 1 and lands in `ash` with job control and
pipelines working. `python build.py rootfs` boots the same busybox from a
Linux-style FAT12 root — `/init`, `/bin`, `/etc`, `/dev`, `/proc` — with the
volume itself as `/`.

## Status

- **Boot** — BIOS floppy image, two-stage 16-bit loader (Linux-style A20, GDT,
  protected mode), E820 map, 448 KiB kernel window.
- **Memory** — paging with a supervisor direct map, E820-sized frame allocator
  and bitmaps, 4 KiB pages, `kmalloc`/`kfree` heap, copy-on-write `fork`, W^X
  enforced by the loader on i386 without PAE.
- **Processes** — 100 Hz preemptive scheduler, per-process address spaces and
  8 KiB kernel stacks, `spawn`/`fork`/`vfork`/`execve`/`wait4`, real process
  groups and sessions, `kill(-pgid)`, foreground group on the console, signals,
  and `clone`-created threads that share one address space and descriptor table.
  Both ports also carry a pthread subset (create/join/detach, robust mutexes,
  condition variables) with per-thread `errno`; a detached thread's slot and
  pages are reclaimed as soon as it exits.
- **SMP** — ACPI MADT discovery, a local-APIC driver and INIT-SIPI trampolines
  that bring the application processors online on both kernels, with atomic
  spinlocks and a private GDT/TSS per processor. Every CPU then schedules: a
  per-CPU block behind `GS`, a timer per CPU, per-CPU idle tasks, reschedule
  IPIs and TLB shootdown, so tasks run in parallel. On i386 the bootstrap
  processor keeps IRQ0 as its tick while the APs use the local APIC.
- **Syscalls** — the i386 numbers for a working file/process/memory/time/signal
  subset, validated user pointers, `-errno` results, and the 64-byte i386
  `struct stat` family that glibc and musl expect.
- **Modules** — `haku module`: a packed `.hkm` container (built on the host by
  `tools/mkhakumod.py` from NASM ELF32 or COFF i386/amd64 objects) loaded with
  `init_module`, relocated against an eleven-entry kernel export table, and
  unloaded with `delete_module`. A module can register a `/dev` character
  device or a filesystem backend and mount it at its own prefix; the kernel
  ties both to the module slot, refuses the unload while a descriptor is open
  (`-EBUSY`), and detaches everything before the image is freed. C modules are
  first-class: `include/haku/module.h` declares the services and `libhakumod`
  supplies the string and 64-bit helpers the compiler calls, with the packer
  dropping whatever a module does not reach. Modules are trusted ring-0 code.
  Both kernels load versioned containers and both wire the full capability
  set: logging, ticks, the kernel heap, a `/dev` character device, and a
  filesystem backend mounted at its own prefix. The x86-64 port runs
  `romfs.c`, the same C source as the 32-bit `/rom`, as `ROMFS.HKM`.
- **Storage** — ATA PIO on both legacy channels with reset/retry recovery, a
  writable FAT12 driver, a VFS with per-process descriptors, `ramfs` root,
  `initramfs` (cpio newc), `/dev`, and a generated `/proc`. The x86-64 kernel
  mounts FAT12 at `/` through a small mount table and adds a `/dev` with
  `null`, `zero` and `console`.
- **Console** — one framebuffer renderer shared by both ports, drawing the
  Terminus 8x16 font as plain full-screen text over a VBE mode with a VGA text
  fallback, PS/2 keyboard, serial mirror, CSI sequence handling with DSR
  replies, kernel-log display latch.
- **Userland** — freestanding C library (`user/lib`), busybox/musl as PID 1,
  a Linux-style rootfs image whose `/INIT` starts `sh` from the FAT12 root,
  headless self-tests for every subsystem listed above.
- **x86-64** — the primary target: long-mode entry, four-level paging with a
  physmap, frame allocator, heap, GDT/TSS with IST stacks, IDT/PIC, timer, RTC
  wall clock, round-robin scheduler, ring 3 with `SYSCALL`/`SYSRET` and the
  Linux syscall convention, NX enforcement, an ELF64 loader, a FAT12 disk with
  file writes behind a page cache that file-backed `mmap` maps from,
  `fork`/`wait4` with copy-on-write pages and default-action signals,
  `dup`/`dup2` over a shared file
  description, `nanosleep`, `execve` of an ELF64 file streamed from the disk
  (up to 1.5 MiB) with `argc`/`argv`/`envp` on the new stack, threads:
  `clone` with
  `CLONE_VM`/`CLONE_FILES`/`CLONE_THREAD`, `gettid`, `futex` wait/wake, a
  detached `CLONE_DETACH` form, the pthread subset and robust mutexes,
  the ported i386 syscall table on the x86-64 ABI (`stat`/`statx`,
  `getdents64`, `rename`/`unlink`/`truncate`, `chdir`/`getcwd`/`openat`,
  `pread`/`pwrite`, `mprotect`/`brk`/`madvise`, `poll`/`select`, `fcntl`/
  `ioctl`/`dup3`, `uname`/`times`/`getrusage`/`getrlimit` and the identity,
  `prctl` and `getrandom` calls), a shared VBE framebuffer console drawing the
  Terminus font as plain text, a PS/2 keyboard whose queue a blocking
  console `read()` drains, a module loader for version 2
  containers, and a 64-bit userland built on `user/lib64`: a freestanding C
  library (string/memory, `printf`, `malloc`, files, `fork`, `mmap`, `errno`,
  pthreads) whose self-test the kernel loads from the FAT12 volume as
  `/INIT.ELF`.
  The disk is `/disk`, a cpio initramfs is the RAM root, `/proc` generates
  `meminfo`/`uptime`/`self/cmdline`, and `/dev` provides `null`, `zero`,
  `console` and devices a module registers; a module filesystem such as `/rom`
  joins the same namespace and is read and listed at ring 3. The shell image
  replaces `/INIT.ELF` with static busybox, and signal handlers, stop/continue
  and `pipe`/`pipe2` with a blocking `poll` connect forked processes.

## Quick start

Requirements: LLVM (`clang-cl` and `lld-link`; no Visual Studio install is
needed), NASM, QEMU (i386 and x86-64), and Python 3. `build.py` picks the
i686 or x86-64 triple for the same compiler, and CI uses that same path on
Ubuntu.

```cmd
python build.py                 :: build haku64.img only (x86-64)
python build.py run             :: build + boot in a QEMU window
python build.py headless        :: build + boot headless, verify
python build.py shell           :: boot a busybox ash shell (64-bit image)
python build.py shell --check   :: scripted shell run, assert its markers
python build.py rootfs          :: boot a Linux-style rootfs (32-bit image)
python build.py rootfs --check  :: scripted rootfs boot, assert its markers
python build.py probe --init=X  :: boot an external ELF64 as /INIT.ELF (64-bit)
python build.py shell --arch=i386 --check :: the original 32-bit shell
python build.py headless --arch=i386   :: the full 32-bit headless suite
```

`headless` prints `X64 CHECK PASSED` after the serial log shows the boot,
storage, process, syscall, memory and module self-test markers.
`headless --arch=i386` prints `HEADLESS CHECK PASSED` for the 32-bit suite.
`shell --check` prints `X64 SHELL CHECK PASSED` after busybox is launched as
PID 1, a PATH lookup, `cat`/`ls`/`ps` and a forked foreground pipeline run;
the 32-bit form prints `SHELL CHECK PASSED`. `probe` prints
`X64 PROBE CHECK PASSED` once the external ELF exits cleanly. `rootfs --check`
prints `ROOTFS CHECK PASSED` after `/INIT` mounts the FAT12 volume as `/`, reads
`/etc/motd` and `/etc/rc`, and runs busybox applets. All exit nonzero on a
missing marker, an unexpected `*-FAIL`, or `KERNEL PANIC`.

`build/haku_fs.img` and `build/haku64_fs.img` are the ATA data disks. The
32-bit image is user data: it is reused untouched unless you pass `--fresh`,
and `python build.py inspect --check` verifies its BPB, FAT mirrors, and
cluster chains without modifying it. Report a damaged image rather than
deleting it out of hand. The x64 seed disk is rebuilt on every run; `shell`
seeds that same `build/haku64_fs.img` with `INIT.CMD` and busybox, while the
32-bit shell uses its own generated `build/haku_shell.img`. `rootfs` boots
`build/haku_rootfs.img` (plus `build/haku_rootfs.tar` for a host).

## Documentation

The project site renders the overview, build instructions and documentation
index for a browser: <https://parsaazima.github.io/HakuKernel/>.

| Document | Covers |
|---|---|
| [docs/](docs/README.md) | Documentation index. |
| [Building and running](docs/building.md) | Prerequisites, every build target, image lifecycle, CI, releases, manual runs. |
| [Architecture](docs/architecture.md) | Boot chain, memory map, IDT, frame allocator, the x86-64 port, and modules. |
| [Processes and scheduling](docs/processes.md) | Process creation, COW forks, scheduler, job control, accounting. |
| [System calls](docs/syscalls.md) | The syscall ABI, the call table, mmap/brk/mprotect, POSIX subset. |
| [Userland](docs/userland.md) | User C library, busybox/musl, the shell and the rootfs image. |
| [x86-64 C library](docs/libc64.md) | `user/lib64`, the Microsoft x64 ABI, the `/INIT.ELF` self-test and its limits. |
| [Terminal and display](docs/terminal.md) | Console backends, input, logging latch, panic/crash reports. |
| [Storage](docs/storage.md) | ATA PIO, FAT12, VFS, boot namespace, design limits. |
| [Kernel internals](docs/kernel-internals.md) | Locking, sector cache, ELF loading, the PE-to-binary build. |
| [Project layout](docs/project-layout.md) | Source tree and generated artifacts. |
| [Troubleshooting](docs/troubleshooting.md) | Common build and boot failures. |
| [Roadmap](ROADMAP.md) | Open milestones, acceptance markers, and the release train. |

## Project layout

```
kernel/arch/x86_64/  long-mode entry and kernel (primary target)
kernel/arch/x86/     boot stages, entry/ISR, GDT/IDT/PIC/PIT, paging, frames, ELF
kernel/drivers/      ATA, terminal, display backends, serial, keyboard
kernel/fs/           VFS, FAT12, ramfs, devfs, procfs, initramfs, fs self-tests
kernel/kernel/       startup, scheduler, syscalls, locking, klog, panic, heap
kernel/module/       .hkm container, 32-bit loader, libhakumod, demo modules
user/lib/            freestanding user C library and CRT (32-bit ABI)
user/lib64/          x86-64 user C library and CRT (Microsoft x64 ABI)
user/tests/          C library self-tests (LIB.ELF, INIT.ELF) and the musl probe
include/haku/        shared ABI (abi.h/abi.inc, abi64.h/abi64.inc), user headers
tools/               pe2bin, pe2elf, pe2elf64, mkimg, mkfat12, mkcpio,
                     mkhakumod, getbusybox, getbusybox64
build.py             the whole build, test, and QEMU orchestration
```

## Limitations

Haku does not claim POSIX compatibility. It implements a subset aimed at
statically linked Linux binaries, without demand paging, swap, or networking.
Both images have `clone`/`futex` threads and bring the application processors
online with their own TSS and the shared IDT. Every CPU schedules: each has
its own current task, idle task and timer, and wakes other CPUs with an IPI.
Tasks are pinned to the CPU that created them, and there is no load balancing
or migration. A libc-linked ring-3 probe now runs on an application processor
on both ports: it prints its getcpu, TLS, signal, syscall, sleep and exit
markers there, while the bootstrap processor collects its status. The x86-64
kernel has scrollback, a canonical line discipline and a PS/2 keyboard, and
`shell` boots static musl busybox as PID 1 into `ash` with job control and
pipelines; its namespace is still limited to the boot mounts and the
filesystems modules mount themselves, and there are no symlinks, so applets
are reached as `busybox <applet>` through `PATH=/disk` rather than installed
as individual `/bin` links. FAT12 has 8.3 names, no timestamps, and no permission
bits. Haku's
filesystem cache holds eight paths of at most 128 KiB each; `execve` streams an
image of up to 1.5 MiB instead of caching it. Everything
beyond the implemented subset returns `ENOSYS` rather than pretending to work.

## Policies

- [Contributing](CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](SECURITY.md) — private vulnerability reporting guidance.
- [Support](SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](AGENTS.md) — repository-wide coding and validation rules.

License: see [LICENSE](LICENSE). The Terminus font embedded in
`kernel/drivers/font_terminus.c` keeps its own SIL Open Font License 1.1; see
[LICENSES/Terminus-OFL.txt](LICENSES/Terminus-OFL.txt).
