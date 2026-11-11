# Building and running Haku

[Back to documentation index](README.md)

Prerequisites, build targets, the data image lifecycle, CI, releases, and
manual test runs.

## Prerequisites (Windows 11)

| Tool | Version here | Notes |
|---|---|---|
| Visual Studio 2022 | 17.x, **Desktop development with C++** | we use the x86 `cl.exe` directly — no project/solution needed |
| NASM | 3.02 | `winget install -e --id NASM.NASM` |
| QEMU | 11.x | `C:\Program Files\qemu\qemu-system-i386.exe` |
| Python 3 | 3.14 | drives the whole build (`build.py` + helpers) |

## Build & run

```cmd
cd D:\Source\Haku
python build.py                 :: build haku.img only (data image preserved)
python build.py run             :: build + boot in a QEMU window
python build.py run --fresh     :: recreate the FAT12 data image, then boot
python build.py headless        :: fresh data image + boot headless, verify
python build.py shell           :: boot a busybox ash shell (interactive)
python build.py shell --check   :: scripted shell run, assert its markers
python build.py x64             :: build haku64.img (64-bit kernel) and boot it
python build.py probe --init=X  :: boot an external ELF as /init, check markers
python build.py --memory=1024   :: guest RAM in MiB (default 512, kernel cap 1020)
python build.py inspect ...     :: examine a FAT12 image (list/check/extract)
```

The kernel does not assume a memory size: it reads the E820 map, sizes the
supervisor direct map and the frame pool from it, and reports both in the
boot log (`MEM-OK: usable=…kB free=…kB direct-map=…MB`). `--memory=` only
changes how much RAM QEMU gives the guest; the kernel uses whatever it finds,
up to the 1020 MiB the 32-bit address space can map without PAE.

`python build.py headless` verifies ATA detection on both legacy channels
(`ATA-SCAN-OK devices=2`), the software-reset recovery path
(`ATA-RECOVERY-OK`), cross-channel sector reads (`ATA-MULTI-OK`),
FAT12 mounting and
multi-cluster file reads, subdirectory/path resolution (`USER-DIR-OK`),
ring-3 process entry, per-process kernel stacks,
syscall handling, `spawn`/`waitpid` and exit-status reporting, page-protection
faults (`USER-PROT-OK`), `execve` image replacement, process exit/reaping,
copy-on-write `fork` and `vfork` (`USER-COW-OK`, `USER-VFORK-OK`,
`VFORK-EXEC-RAN`), signal delivery and dispositions (`USER-SIGNAL-OK`),
`clone`-created threads that share the address space and descriptors and
report a distinct tid under the same pid (`USER-THREAD-OK`),
the user C library (`USER-LIB-OK`), and timer IRQs
through the serial log.

`python build.py x64` builds and boots `haku64.img` separately and asserts
its own marker set. That run starts a fresh FAT12 data disk
(`build/haku64_fs.img`) seeded with `HELLO.TXT`, `PATTERN.BIN` and the
`USER.ELF` program the test execs, ends with `HAKU64-ELF-REJECT-OK` (six
malformed ELF64 images refused with a reason), `HAKU64-ELF-OK` (the embedded
static executable mapped into a fresh address space), `HAKU64-ELF-RUN-OK`
(the program ran in ring 3, stored its BSS word and exited with status 0),
`HAKU64-ABI-OK` (the six-argument `INT 0x80` ABI passed its success and error
matrix, including `mmap`/`munmap`), `HAKU64-DISK-OK` (the program created
`/TMP`, wrote `NOTE.TXT` there and the kernel read it back from the volume
after `close`), `HAKU64-FORK-OK` (a forked child and its parent observed the
same `MAP_SHARED` page), `HAKU64-DUP-OK` (a duplicated descriptor shared
one file offset, `dup2` redirected it, and `nanosleep` advanced the monotonic
clock), `HAKU64-EXEC-OK` (a child `execve`d `USER.ELF` off the volume with
two arguments and one environment variable, the program checked the stack
image and printed `HAKU64-EXEC-PROG`/`HAKU64-EXEC-ARGS`, and it exited 42) and
`HAKU64-THREAD-OK` (a `clone`d thread shared the address space and the
descriptor table, bumped a shared word, woke the parent through a futex, and
the parent's write through the thread's `dup2` target printed the marker
before `wait4` reaped the thread with status 7), and `HAKU64-PORT-OK` (the
ported table: `stat`/`fstat`/`statx`, `chdir`/`getcwd` with a relative
open, `openat`/`mkdirat`, `getdents64` on an empty and a seeded directory,
`ftruncate`/`truncate`/`renameat`/`unlinkat`, `uname`/`times`/
`getrlimit`/`clock_getres`, `prctl` names, two different `getrandom`
words, `brk`+`mprotect`, `poll`/`select`, `sched_yield` and `dup3`/
`fcntl`).
After QEMU exits, `tools/lsfat12.py --check` verifies the volume the guest
just wrote. The 32-bit `haku.img` stays the default image.

`python build.py shell` boots something else entirely: a real static
`busybox` (musl, i386) as PID 1, so the guest comes up in `ash` instead of
Haku's own test userland. The image is a separate FAT12 volume
(`build/haku_shell.img`) holding `BUSYBOX` and, for the scripted run, a
one-line `INIT.CMD`; the kernel prefers `disk/BUSYBOX` over the initramfs
when it is there and hands it `sh -c "…"`. busybox is 1 MiB and belongs to
someone else, so it is cached under `build/` and never committed —
`tools/getbusybox.py` fetches the Alpine `busybox-static` build on first use
and `--busybox=PATH` points the build at a local copy instead (offline
machines, CI). `--cmd=TEXT` replaces the boot command line, and
`shell --check` runs the fixed script
(`mkdir -p /bin`, `--install -s /bin`, `export PATH=/bin`, then `uname`,
`ls /bin | wc -l`, `ls /`, `cat /proc/version`) and asserts
`SHELL-BOOT`, `SHELL-OK`, `UNAME:haku`, `APPS:<count ≥ 100>`, `HakuKernel`,
and `SHELL-DONE`. Today it reports 306 applets resolved through `$PATH`.

### Data image lifecycle

`build/haku_fs.img` is the ATA data disk and is treated as user data: an
existing image is reused untouched, so files created in a previous QEMU
session survive the next build. It is (re)created from `kernel/arch/x86/user/user.asm`,
`kernel/arch/x86/user/exec_target.asm`, `kernel/arch/x86/user/child.asm`,
`kernel/arch/x86/user/vfork_target.asm`, the
user library in `user/lib/`, and `user/tests/libtest.c` only when missing or
with `--fresh`. If any test-program source is newer than a reused image,
the build warns that its test ELFs on disk may be stale. `headless` always
starts from a fresh image (its fault-injection tests mutate the disk) and,
after the serial-log check passes, re-verifies the post-run image on the
host side.

`python build.py inspect` (default: `build/haku_fs.img`) lists the root
directory and FAT summary without touching the image; `--chains` also prints
cluster chains, `--cat NAME.EXT` / `--dump NAME.EXT OUT.BIN` extract a file,
and `--check` enforces the kernel's mount invariants (BPB sanity, mirrored
FATs, exact chains, no orphans) with a nonzero exit on any damage.
The build is one Python script on purpose — `cmd.exe` quoting of spaced
paths (e.g. `C:\Program Files\...`) is fragile, while `subprocess` argument
lists just work.

### Continuous integration and releases

`.github/workflows/ci.yml` runs on every push to `main`, every pull request,
and manual dispatch. It uses `ubuntu-latest`, installs NASM, Clang/LLD, and
QEMU from apt, runs `python build.py headless` through the
`clang-cl`/`lld-link` path in `build.py`, and uploads `haku.img`, the data
image, serial/QEMU logs, and any crash screenshots as artifacts even when the
boot check fails. A missing marker, an unexpected `*-FAIL` marker, or
`KERNEL PANIC` fails the job.

A second job, `musl`, builds `user/tests/musl_probe.c` with a prebuilt
`i486-linux-musl` cross toolchain and boots it as `/init` with
`python build.py probe --init=build/musl_probe --expect=MUSL-PROBE-OK`. The
cross compiler matters: `musl-gcc` from `musl-tools` is host-native, so on an
x86-64 runner it produces a 64-bit ELF that a 32-bit kernel must reject. The
job checks `ELF32`/`Intel 80386` before booting so that mistake fails at the
build step instead of looking like a loader bug. This
is the only check that runs an outside C library on top of the kernel, so it
is what proves the ABI in [the syscall documentation](syscalls.md): musl's own
`_start`, auxiliary-vector
parsing, `%gs`-based TLS, `writev`-backed stdio, and file/directory wrappers
all have to work for the probe to print `MUSL-PROBE-OK`. The probe binary is
built outside Haku's toolchain and packed into the initramfs as `/init`; the
`probe` mode boots it without the keyboard injection the headless suite needs
and asserts `HAKU-BOOT-OK`, `INITRAMFS-UNPACK-OK`, `INIT-READ-OK name=init`,
`USERMODE-ENTER`, and the requested marker.

`.github/workflows/release.yml` runs for `v*` tags. It repeats the headless
check, deletes the mutated test image, rebuilds a pristine
`build/haku_fs.img`, and publishes `Haku-<tag>.zip` containing `haku.img`,
`haku_fs.img`, `README.md`, `LICENSE`, and `SHA256SUMS.txt`:

```bash
git tag v0.2.0
git push origin v0.2.0
```

Slow runners can tune headless timing with `HAKU_BOOT_DELAY` (seconds before
the injected key, default 1) and `HAKU_HEADLESS_TIMEOUT` (seconds to wait for
guest shutdown, default 30).

## Manual testing and image recovery

`python build.py run` boots the floppy in a QEMU window with the same disks
as the automated run, so the whole flow can be driven by hand: the injected
key only exists in `headless`, so press any key yourself at the read prompt.
`build/serial.log` is only written by `headless`; in a windowed run the same
text appears on the VGA/framebuffer console.

A data image that a test corrupted can be thrown away safely: delete
`build/haku_fs.img` or run `python build.py --fresh` (add `headless` to also
run the tests), and the image is rebuilt from the test programs. Before
discarding one, `python build.py inspect --check` says whether it is really
damaged — it verifies the BPB, both FAT mirrors, every cluster chain, and
orphans, and exits nonzero on any inconsistency.
