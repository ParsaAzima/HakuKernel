#!/usr/bin/env python3
"""Haku build — one straightforward script, no shell-quoting games.

Usage:
    python build.py                 build haku64.img only (x86-64, no boot)
    python build.py run             build + boot haku64.img in a QEMU window
    python build.py headless        build + boot haku64.img headless, verify
    python build.py x64             alias for the headless x86-64 check
    python build.py ... --arch=i386 build/run the 32-bit kernel instead
    python build.py --memory=MB     guest RAM (default 512, max 1020)
    python build.py inspect ...     examine a FAT12 image (see --help there)

    python build.py shell           busybox ash on the 32-bit kernel
    python build.py rootfs          boot a minimal Linux-style rootfs (32-bit)
    python build.py rootfs --check  boot it headless and assert the markers
    python build.py probe --init=P --expect=M
                                    run an ELF32 probe as /init (32-bit)
    python build.py run --arch=i386 --fresh
                                    recreate the 32-bit FAT12 data image first

Pipeline (x86-64, the default):
    kernel/arch/x86_64 C sources --cl--> x64 objects
    + arch/x86_64/kernel/{entry64,isr64,ap_trampoline64}.asm --nasm-->
    kernel64.pe --pe2bin--> kernel64.bin
    kernel/module/{demo/x64demo.c,lib/*} --cl/nasm-->
        x64 objects --mkhakumod (COFF/AMD64, container v2)--> X64MOD.HKM
    arch/x86/boot/stage1.asm --nasm--> stage1.bin (512 B)
    arch/x86/boot/stage2.asm --nasm (kernel entry + size from PE)--> stage2.bin
    stage1 + stage2 + kernel64.bin --mkimg--> haku64.img
    QEMU boots haku64.img and attaches build/haku64_fs.img as the
    primary-master ATA disk, then verifies the volume after QEMU exits.

The 32-bit pipeline (`--arch=i386`, `shell`, `rootfs`, `probe`) builds
haku.img from kernel/**/*.c and arch/x86, and attaches build/haku_fs.img plus
a generated spare disk on the secondary channel. It remains the one with the
user C library, busybox and the shell. `shell` keeps the RAM root and puts
busybox on the data disk; `rootfs` mounts that same volume at / instead and
runs /INIT from it. `python build.py x64` is kept as an alias for the x64
headless check; it shares nothing with the i386 link line.

The 32-bit data image (build/haku_fs.img) is user data: an existing image is
reused as-is so files from previous QEMU sessions survive the build, and
it is recreated only when missing or with --fresh. headless always starts
from a fresh image — its fault-injection tests need pristine state — and
verifies the disk again after QEMU exits (tools/lsfat12.py --check). The x64
seed disk is rebuilt on every run because the kernel writes NOTE.TXT into it.

All tools are invoked with argument lists (never through cmd.exe), so paths
with spaces just work. Both targets are built by Clang in its MSVC-compatible
driver mode (clang-cl) and linked with lld-link: the kernel needs no Visual
Studio install, only LLVM, and the switches (/c, /Fo, /ENTRY, /ALIGN,
/NODEFAULTLIB ...) are the same ones the build always used. On Windows the
tools are taken from C:\Program Files\LLVM\bin when it exists; the
architecture comes from --target, so only tool discovery differs.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
os.makedirs(BUILD, exist_ok=True)

# What the shell target runs as PID 1. Alpine's busybox is not built with a
# standalone shell, so an applet is only found through $PATH, and $PATH only
# helps if there is a file to exec: `--install -s /bin` fills /bin with one
# symlink per applet, and every one of those links names the binary
# relatively ("busybox"), so /bin/busybox has to exist first or the whole
# directory dangles. The setup then exports PATH and the check proves the
# whole path -- ash parsing, applet dispatch through $PATH, the syscalls
# behind uname/ls/cat/free, and /proc -- and leaves an interactive shell
# running so PID 1 never exits and the guest never panics on an empty run
# queue.
SHELL_SETUP = ("/disk/busybox mkdir -p /bin; "
               "/disk/busybox ln -s /disk/busybox /bin/busybox; "
               "/disk/busybox --install -s /bin; export PATH=/bin; ")
SHELL_INTERACTIVE_SCRIPT = (
    'sh -c "' + SHELL_SETUP +
    'echo Haku shell: busybox ash, type help or exit; exec sh"')
SHELL_CHECK_SCRIPT = (
    'sh -c "' + SHELL_SETUP +
    'echo SHELL-OK; echo UNAME:$(uname -n); '
    'echo APPS:$(ls /bin | wc -l); '
    'ls /; cat /proc/version; '
    # -i makes ash turn job control on, which runs exactly the sequence the
    # interactive shell does on start-up: dup the controlling terminal above
    # fd 9, read the foreground group, and install it back. Without the
    # kernel's process-group answers this prints "can't access tty". The
    # pipeline that follows forks two children that ash moves into a fresh
    # process group and makes the terminal's foreground group; if setpgid or
    # TIOCSPGRP only accepts the caller's own pid the second child dies with
    # "can't set tty process group" and PIPE-DONE never appears.
    "sh -i -c 'echo SHELL-JOBCTL-OK; echo PIPE-OUT | cat; echo PIPE-DONE'; "
    'echo SHELL-DONE; exec sh"')
# Output the check insists on. A boot that reaches the prompt but cannot run
# an applet still prints SHELL-OK, so the markers alone are not enough: the
# nodename proves uname ran, the applet count proves the links resolve, the
# /proc line proves a file read went through the VFS, the job-control marker
# shows the interactive shell started, and the pipeline markers prove a
# forked foreground pipeline ran to completion.
SHELL_EXPECTED = ["SHELL-BOOT", "SHELL-OK", "UNAME:haku", "HakuKernel",
                  "TERMINAL-CSI-OK", "SHELL-JOBCTL-OK", "PIPE-OUT",
                  "PIPE-DONE", "SHELL-DONE"]

# What `rootfs --check` writes to /etc/rc, the script /INIT hands to `sh -c`.
# The volume holds one busybox binary and FAT12 has no symlinks, so an applet
# is reached through the multiplexer (`/bin/busybox uname`), not through $PATH.
# The checks prove the boot reached ring 3, read /etc/motd and /etc/rc off the
# FAT12 root, dispatched applets, read /proc and listed the root directory.
ROOTFS_CHECK_SCRIPT = (
    'echo ROOTFS-SH-OK; '
    'echo UNAME:$(/bin/busybox uname -n); '
    '/bin/busybox cat /proc/version; '
    '/bin/busybox ls /; '
    'echo ROOTFS-DONE; '
    'exec /bin/busybox sh')
ROOTFS_EXPECTED = ["ROOTFS-MOUNT-OK",   # the kernel chose the FAT12 volume
                   "ROOTFS-INIT-OK",    # /INIT ran as pid 1
                   "ROOTFS-EXEC: /bin/busybox sh -c /etc/rc",
                   "HakuKernel rootfs", # /etc/motd was read and printed
                   "ROOTFS-SH-OK",      # busybox ash parsed /etc/rc
                   "UNAME:haku",        # an applet ran through the multiplexer
                   "HakuKernel",        # /proc/version was read
                   "ROOTFS-DONE"]


def log(msg):
    print(msg, flush=True)


def wait_for_serial(path, needle, timeout):
    """Poll the serial log until `needle` shows up, or the timeout expires.

    Used instead of a fixed sleep before the keystroke: the kernel prints
    SCHED-WAITQ-OK exactly when pid 1 parks on the console read, so sending
    the key then is deterministic however long the boot took.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, errors="replace") as f:
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(0.05)
    return False


def monitor_cmd(port, cmds, timeout=5):
    """Run HMP commands over the TCP monitor, return the raw transcript."""
    import socket
    out = []
    s = None
    try:
        # QEMU may need a moment to listen (Windows TCP); retry instead
        # of dropping keystrokes that user processes block on.
        for _ in range(12):
            try:
                s = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
                break
            except OSError:
                time.sleep(0.5)
        if s is None:
            raise OSError("monitor never listened")
        s.settimeout(timeout)
        try:
            out.append(s.recv(65536))
        except OSError:
            pass
        for cmd in cmds:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(0.7)
            try:
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    out.append(chunk)
                    if b"(qemu)" in chunk:
                        break
            except OSError:
                pass
        s.close()
    except OSError as e:
        log(f"(monitor unavailable: {e})")
    return b"".join(out)


def take_screenshot(port, path):
    """Ask QEMU's human monitor for a PPM screenshot (best effort)."""
    blob = monitor_cmd(port, [f"screendump {path}"])
    time.sleep(1.0)
    if os.path.exists(path):
        log(f"screenshot: {path}")
    elif blob:
        log(f"(screendump produced no file; monitor said: {blob[-200:]!r})")


def find_tool(name, fallbacks, label):
    found = shutil.which(name)
    if found:
        return found
    for path in fallbacks:
        if os.path.isfile(path):
            return path
    sys.exit(f"Tool not found: {label}")


def verify_abi_headers(include_dir):
    header = os.path.join(include_dir, "haku", "abi.h")
    nasm_include = os.path.join(include_dir, "haku", "abi.inc")
    c_pattern = re.compile(
        r"^\s*#define\s+(HAKU_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\s*(?:/\*.*\*/)?\s*$")
    nasm_pattern = re.compile(
        r"^\s*%define\s+(HAKU_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*(?:;.*)?\s*$")

    def read_definitions(path, pattern):
        values = {}
        with open(path, encoding="ascii") as source:
            for line in source:
                match = pattern.match(line)
                if match:
                    values[match.group(1)] = int(match.group(2), 0)
        return values

    c_values = read_definitions(header, c_pattern)
    nasm_values = read_definitions(nasm_include, nasm_pattern)
    if not c_values or c_values != nasm_values:
        sys.exit("C and NASM ABI headers are missing or out of sync")


def verify_abi64_headers(include_dir):
    """The x86-64 C ABI header must not drift from the NASM constants.

    abi64.inc carries extra NASM-only values (clone flags, poll events), so
    this check requires every HAKU64_* the C header defines to exist in the
    .inc with the same number, rather than requiring the sets to be equal.
    """
    header = os.path.join(include_dir, "haku", "abi64.h")
    nasm_include = os.path.join(include_dir, "haku", "abi64.inc")
    number = r"(-?(?:0x[0-9A-Fa-f]+|\d+))"
    c_pattern = re.compile(
        r"^\s*#define\s+(HAKU64_\w+)\s+" + number +
        r"u?\s*(?:/\*.*\*/)?\s*$")
    nasm_pattern = re.compile(
        r"^\s*%define\s+(HAKU64_\w+)\s+" + number + r"\s*(?:;.*)?\s*$")

    def read_definitions(path, pattern):
        values = {}
        with open(path, encoding="ascii") as source:
            for line in source:
                match = pattern.match(line)
                if match:
                    values[match.group(1)] = int(match.group(2), 0)
        return values

    c_values = read_definitions(header, c_pattern)
    nasm_values = read_definitions(nasm_include, nasm_pattern)
    if not c_values:
        sys.exit("the x86-64 C ABI header defines no HAKU64_* constants")
    missing = sorted(name for name, value in c_values.items()
                     if nasm_values.get(name) != value)
    if missing:
        sys.exit("abi64.h and abi64.inc are out of sync: "
                 + ", ".join(missing))


def _llvm_bin_dirs():
    """Directories to search for LLVM tools before PATH.

    Windows installs Clang to Program Files\LLVM\bin and does not put it on
    PATH unless the installer is told to, so check the standard location
    first. The Linux paths below keep the existing distro layout working.
    """
    dirs = [r"C:\Program Files\LLVM\bin", r"C:\Program Files (x86)\LLVM\bin"]
    dirs += ["/usr/lib/llvm-*/bin", "/usr/bin", "/usr/local/bin"]
    found = []
    for d in dirs:
        if "*" in d:
            found += glob.glob(d)
        elif os.path.isdir(d):
            found.append(d)
    return found


def _latest(paths):
    """Pick the highest-numbered tool; distros ship clang-cl-19, lld-link-19."""
    def version(path):
        nums = re.findall(r"\d+", os.path.basename(path))
        return int(nums[-1]) if nums else -1
    return max(paths, key=version)


def find_clang_cl():
    """Locate a cl-driver clang: clang-cl, a versioned one, or clang in cl mode."""
    for d in _llvm_bin_dirs():
        direct = os.path.join(d, "clang-cl.exe" if os.name == "nt"
                              else "clang-cl")
        if os.path.isfile(direct):
            return [direct]
    direct = shutil.which("clang-cl")
    if direct:
        return [direct]
    cands = []
    for d in _llvm_bin_dirs():
        cands += glob.glob(os.path.join(d, "clang-cl*"))
    cands = [c for c in cands if not c.endswith(".lib")]
    if cands:
        return [_latest(cands)]
    clang = shutil.which("clang")
    if clang:
        return [clang, "--driver-mode=cl"]
    sys.exit("clang-cl not found: install LLVM (C:\\Program Files\\LLVM\\bin "
             "on Windows, e.g. apt install clang lld elsewhere)")


def find_lld_link():
    """Locate lld-link (or a versioned lld-link-19)."""
    for d in _llvm_bin_dirs():
        direct = os.path.join(d, "lld-link.exe" if os.name == "nt"
                              else "lld-link")
        if os.path.isfile(direct):
            return [direct]
    direct = shutil.which("lld-link")
    if direct:
        return [direct]
    cands = []
    for d in _llvm_bin_dirs():
        cands += glob.glob(os.path.join(d, "lld-link*"))
    if not cands:
        sys.exit("lld-link not found: install LLVM lld (it ships with "
                 "C:\\Program Files\\LLVM\\bin; e.g. apt install lld)")
    return [_latest(cands)]


def detect_toolchain():
    """Return (cc, linker, label) for the 32-bit target.

    Clang in cl mode emits i686 COFF with the MSVC ABI and lld-link accepts
    the same /ENTRY, /ALIGN and /NODEFAULTLIB switches as link.exe, so the
    NASM win32 objects, the linked PE and pe2bin's output are unchanged; only
    the tool that produces them differs. `-fno-builtin` is required: Clang
    turns the counted-store loop in the project's own memset/memcpy into a call
    to those same functions, and a freestanding kernel has no libc to break
    the recursion.
    """
    cc = find_clang_cl() + ["--target=i686-pc-windows-msvc",
                            "-fno-builtin"]
    return cc, find_lld_link(), "clang-cl/lld-link (i686-pc-windows-msvc)"


def find_x64_toolchain():
    """Return (cc, linker, label) for x86-64, or exit with a clear reason.

    The same clang-cl/lld-link pair is retargeted at
    x86_64-pc-windows-msvc, mirroring the i686 path, so NASM win64 objects,
    the PE32+ image, and pe2bin line up.
    """
    cc = find_clang_cl() + ["--target=x86_64-pc-windows-msvc",
                            "-fno-builtin"]
    return cc, find_lld_link(), "clang-cl/lld-link (x86_64-pc-windows-msvc)"


def shell_busybox(explicit):
    """Path to a static i386 busybox, fetching one on first use.

    The binary is 1 MiB and belongs to someone else, so it is cached under
    build/ and never committed. `--busybox=PATH` points the build at a local
    copy instead, which is what an offline machine or CI wants.
    """
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit(f"no such busybox: {explicit}")
        return explicit
    cached = os.path.join(BUILD, "busybox")
    if os.path.isfile(cached) and os.path.getsize(cached) > 0:
        return cached
    log("no busybox cached; fetching a static musl build (Alpine x86)")
    rc = subprocess.run([sys.executable,
                         os.path.join(ROOT, "tools", "getbusybox.py"),
                         cached]).returncode
    if rc != 0:
        sys.exit("could not obtain busybox; pass --busybox=PATH")
    return cached


def build_shell_image(py, user_elf, busybox, command):
    """FAT12 image holding busybox and, optionally, the boot command line.

    The kernel picks /disk/BUSYBOX over the initramfs when it is present, so
    this image needs no archive: the ramfs root, /dev and /proc are mounted
    either way, and busybox is the whole userland.
    """
    image = os.path.join(BUILD, "haku_shell.img")
    args = [py, os.path.join(ROOT, "tools", "mkfat12.py"),
            user_elf, image, f"BUSYBOX={busybox}"]
    if command is not None:
        cmd_path = os.path.join(BUILD, "init.cmd")
        with open(cmd_path, "w", encoding="ascii", newline="\n") as f:
            f.write(command + "\n")
        args.append(f"INIT.CMD={cmd_path}")
    run(args)
    log(f"shell image ready ({os.path.getsize(image)} bytes, "
        f"busybox {os.path.getsize(busybox)} bytes)")
    return image


def write_rootfs_tar(path, entries):
    """Pack the same tree as a tar so a host can inspect or unpack it.

    Entries are (name, source-or-None); None marks a directory. Timestamps and
    owners are fixed so the archive is byte-identical across builds.
    """
    import tarfile

    with tarfile.open(path, "w", format=tarfile.GNU_FORMAT) as archive:
        for name, source in entries:
            info = tarfile.TarInfo(name)
            info.uid = 0
            info.gid = 0
            info.uname = "root"
            info.gname = "root"
            info.mtime = 0
            if source is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
                continue
            info.mode = 0o755 if name in ("INIT", "BIN/BUSYBOX") else 0o644
            info.size = os.path.getsize(source)
            with open(source, "rb") as handle:
                archive.addfile(info, handle)


def build_rootfs_image(py, init_elf, busybox, command):
    """Stage the Linux-shaped tree and pack it as a FAT12 volume plus a tar.

    The volume is what QEMU mounts as /: /INIT is pid 1, /BIN/BUSYBOX the
    whole userland, /ETC/{MOTD,PROFILE,RC} the text /INIT reads, and /DEV,
    /PROC, /TMP the mount points the kernel fills. FAT12 names are 8.3, so
    the tree is upper case here; the filesystem folds case, which is what
    makes /etc/rc and /bin/busybox work from user space. The tar is the same
    tree for a host, so the image can be inspected without QEMU.
    """
    stage = os.path.join(BUILD, "rootfs")
    os.makedirs(stage, exist_ok=True)
    texts = [
        ("ETC/MOTD",
         "HakuKernel rootfs\n"
         "32-bit x86 port, FAT12 root, busybox 1.37\n"
         "applets run as `/bin/busybox APP` (the volume has no symlinks)\n"
         "type `exit` to stop the guest\n"),
        ("ETC/PROFILE",
         "PATH=/bin\n"
         "HOME=/\n"
         "TERM=linux\n"
         "PS1=haku# \n"
         "export PATH HOME TERM PS1\n"),
        ("README.TXT",
         "HakuKernel rootfs tree\n"
         "\n"
         "INIT            pid 1, reads /etc/motd and /etc/rc\n"
         "BIN/BUSYBOX     static musl busybox; every applet is this file\n"
         "ETC/MOTD        banner /INIT prints\n"
         "ETC/PROFILE     login shell environment\n"
         "ETC/RC          boot command, when built with --check or --cmd\n"
         "DEV, PROC       mount points for devfs and procfs\n"
         "TMP             scratch space\n"),
    ]
    if command is not None:
        texts.append(("ETC/RC", command + "\n"))
    entries = []
    for name, text in texts:
        source = os.path.join(stage, name.replace("/", "_").lower())
        with open(source, "w", encoding="ascii", newline="\n") as handle:
            handle.write(text)
        entries.append((name, source))

    image = os.path.join(BUILD, "haku_rootfs.img")
    run([py, os.path.join(ROOT, "tools", "mkfat12.py"), "--empty", image,
         f"INIT={init_elf}", f"BIN/BUSYBOX={busybox}"]
        + [f"{name}={source}" for name, source in entries]
        + [f"--dir={name}" for name in ("DEV", "PROC", "TMP")])
    write_rootfs_tar(os.path.join(BUILD, "haku_rootfs.tar"),
                     [("BIN", None), ("DEV", None), ("ETC", None),
                      ("PROC", None), ("TMP", None),
                      ("INIT", init_elf), ("BIN/BUSYBOX", busybox)]
                     + entries)
    log(f"rootfs image ready ({os.path.getsize(image)} bytes, "
        f"busybox {os.path.getsize(busybox)} bytes)")
    return image


def build_x64_fs(py, image, exec_elf, module_image, romfs_image, init_elf):
    """Seed the x86-64 disk: a FAT12 volume with the boot archive on it.

    The kernel mounts FAT12 at /disk, reads /disk/INITRD, and unpacks it into
    the RAM filesystem that becomes /. The archive therefore carries what the
    boot tests open by absolute path: HELLO.TXT with the 16-byte marker,
    PATTERN.BIN with three pages of a byte pattern, USER.ELF (the program the
    ring-3 test execs, so the kernel image does not contain it), INIT.ELF (the
    C program built against user/lib64 that the kernel starts first),
    X64MOD.HKM (the packed module the loader reads and relocates), ROMFS.HKM
    (the read-only filesystem module the capability test mounts at /rom), and
    etc/banner.txt for the ring-3 initramfs check. The pattern is the same
    (offset*37+11) & 0xFF formula boot64.c uses to verify it.

    The FAT12 volume keeps its own copies of HELLO.TXT and PATTERN.BIN so the
    kernel can prove that /disk and / reach two different backends. The image
    is rebuilt on every x64 run because the kernel also writes /disk/NOTE.TXT
    into it.
    """
    hello = b"HAKU64-FILE-OK\r\n"
    pattern = bytes(((i * 37 + 11) & 0xFF) for i in range(3 * 4096))
    banner = b"Haku x86-64 initramfs\n/etc/banner.txt\n"
    hello_path = os.path.join(BUILD, "haku64_hello.txt")
    pattern_path = os.path.join(BUILD, "haku64_pattern.bin")
    banner_path = os.path.join(BUILD, "haku64_banner.txt")
    archive = os.path.join(BUILD, "initrd64.cpio")
    with open(hello_path, "wb") as f:
        f.write(hello)
    with open(pattern_path, "wb") as f:
        f.write(pattern)
    with open(banner_path, "wb") as f:
        f.write(banner)

    # cpio "newc" archive. Directories first so the entries below resolve
    # without an intermediate mkdir; the kernel creates parents anyway.
    run([py, os.path.join(ROOT, "tools", "mkcpio.py"), archive,
         "--dir=tmp", "--dir=dev", "--dir=disk",
         f"HELLO.TXT={hello_path}", f"PATTERN.BIN={pattern_path}",
         f"USER.ELF={exec_elf}", f"INIT.ELF={init_elf}",
         f"X64MOD.HKM={module_image}", f"ROMFS.HKM={romfs_image}",
         f"etc/banner.txt={banner_path}"])
    if os.path.getsize(archive) > 32 * 4096:
        raise SystemExit("x64 initramfs exceeds the 128 KiB cached-file limit")

    run([py, os.path.join(ROOT, "tools", "mkfat12.py"), "--empty", image,
         f"HELLO.TXT={hello_path}", f"PATTERN.BIN={pattern_path}",
         f"INITRD={archive}", f"USER.ELF={exec_elf}",
         f"INIT.ELF={init_elf}", f"X64MOD.HKM={module_image}",
         f"ROMFS.HKM={romfs_image}"])
    log(f"x64 FAT12 seed disk ready ({os.path.getsize(image)} bytes, "
        f"initramfs {os.path.getsize(archive)} bytes)")


def build_x64(nasm, py, memory_mb, smp_cpus, mode):
    """Build haku64.img, then boot it when `mode` asks for a run.

    The entry path, memory subsystem, interrupts, per-process address
    spaces, the scheduler, ring-3 entry, the six-argument syscall ABI, the
    ELF64 loader, a FAT12 disk (the same ATA driver and filesystem the
    32-bit kernel uses, behind a small compatibility layer), the module
    loader and its device/filesystem capabilities (the ROMFS module mounts a
    read-only filesystem at /rom) are all 64-bit. `mode` is "build" (compile
    and lay out the images), "run" (boot with a QEMU window) or "headless"
    (boot, capture the serial log and assert the marker list).
    """
    cc, linker, label = find_x64_toolchain()
    log(f"x64 toolchain: {label}")
    verify_abi_headers(os.path.join(ROOT, "include"))
    verify_abi64_headers(os.path.join(ROOT, "include"))

    arch32 = os.path.join(ROOT, "kernel", "arch", "x86")
    arch64 = os.path.join(ROOT, "kernel", "arch", "x86_64")
    entry_asm = os.path.join(arch64, "boot", "entry64.asm")
    isr_asm = os.path.join(arch64, "kernel", "isr64.asm")
    ap_trampoline64_asm = os.path.join(arch64, "kernel", "ap_trampoline64.asm")
    user_src = os.path.join(arch64, "user", "ring3_elf64.asm")
    user_image_asm = os.path.join(arch64, "user", "user64_image.asm")
    stage1_asm = os.path.join(arch32, "boot", "stage1.asm")
    stage2_asm = os.path.join(arch32, "boot", "stage2.asm")

    entry_obj = os.path.join(BUILD, "entry64.obj")
    isr_obj = os.path.join(BUILD, "isr64.obj")
    ap_trampoline64_obj = os.path.join(BUILD, "ap_trampoline64.obj")
    user_flat = os.path.join(BUILD, "ring3_elf64.bin")
    user_elf = os.path.join(BUILD, "ring3_elf64.elf")
    exec_src = os.path.join(arch64, "user", "exec_elf64.asm")
    exec_flat = os.path.join(BUILD, "exec_elf64.bin")
    exec_elf = os.path.join(BUILD, "exec_elf64.elf")
    user_image_obj = os.path.join(BUILD, "user64_image.obj")
    kernel_pe = os.path.join(BUILD, "kernel64.pe")
    kernel_bin = os.path.join(BUILD, "kernel64.bin")
    stage1_bin = os.path.join(BUILD, "stage1.bin")
    stage2_bin = os.path.join(BUILD, "stage2_64.bin")
    serial = os.path.join(BUILD, "serial64.log")
    img = os.path.join(ROOT, "haku64.img")
    fs_img = os.path.join(BUILD, "haku64_fs.img")
    user_base = 0x0000008000400000

    # 1. the 64-bit entry stub and every C file under arch/x86_64. The ATA
    #    driver and FAT12 filesystem are the 32-bit sources, compiled against
    #    kernel/arch/x86_64/compat so the 64-bit port reuses tested logic
    #    instead of growing a second disk stack.
    include_dirs = [os.path.join(arch64, "kernel"), os.path.join(arch64, "mm"),
                    os.path.join(arch64, "compat"),
                    os.path.join(ROOT, "include"),
                    os.path.join(ROOT, "kernel", "module"),
                    os.path.join(ROOT, "kernel", "drivers"),
                    os.path.join(ROOT, "kernel", "fs", "fat12")]
    sources = sorted(glob.glob(os.path.join(arch64, "**", "*.c"),
                               recursive=True))
    sources += [os.path.join(ROOT, "kernel", "drivers", "ata.c"),
                os.path.join(ROOT, "kernel", "drivers", "acpi.c"),
                os.path.join(ROOT, "kernel", "drivers", "apic.c"),
                # The console is the same source on both ports: the display
                # backend table, the VGA text fallback, the shared VBE
                # renderer and the Terminus table it draws.
                os.path.join(ROOT, "kernel", "drivers", "display.c"),
                os.path.join(ROOT, "kernel", "drivers", "vga_text.c"),
                os.path.join(ROOT, "kernel", "drivers", "fbcon.c"),
                os.path.join(ROOT, "kernel", "drivers", "font_terminus.c"),
                os.path.join(ROOT, "kernel", "fs", "fat12", "fat12.c")]
    if not sources:
        sys.exit("no kernel/arch/x86_64/**/*.c files found")
    run([nasm, "-f", "win64", entry_asm, "-o", entry_obj])
    run([nasm, "-f", "win64", isr_asm, "-o", isr_obj])
    run([nasm, "-f", "win64", ap_trampoline64_asm, "-o", ap_trampoline64_obj])
    objects = []
    for src in sources:
        obj = os.path.join(BUILD, "x64_" +
                           os.path.splitext(os.path.basename(src))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Os", "/W3"]
            + ["/I" + d for d in include_dirs]
            + ["/Fo" + obj, src])
        objects.append(obj)
    log(f"{len(objects)} x64 C objects built")

    # 1b. the module ABI for x86-64. Same libhakumod source as the 32-bit
    #     build, but the runtime that supplies __chkstk is the 64-bit one and
    #     the packer writes a version 2 container whose relocations fit the
    #     amd64 COFF MSVC emits. X64MOD.HKM is seeded on the disk below, not
    #     linked into the kernel, so loading it proves the on-disk path too.
    module_dir = os.path.join(ROOT, "kernel", "module")
    module_lib_dir = os.path.join(module_dir, "lib")
    module_flags = (["/nologo", "/c", "/GS-", "/Zl", "/Os", "/Gy", "/W3"]
                    + ["/I" + os.path.join(ROOT, "include"),
                       "/I" + module_dir, "/I" + os.path.join(arch64, "compat")])
    x64_modlib = os.path.join(BUILD, "x64_modlib.obj")
    run(cc + module_flags +
        ["/Fo" + x64_modlib, os.path.join(module_lib_dir, "module_lib.c")])
    x64_modruntime = os.path.join(BUILD, "x64_modruntime.obj")
    run([nasm, "-f", "win64",
         os.path.join(module_lib_dir, "runtime64.asm"), "-o", x64_modruntime])
    x64_demo = os.path.join(BUILD, "x64_demo.obj")
    run(cc + module_flags +
        ["/Fo" + x64_demo, os.path.join(module_dir, "demo", "x64demo.c")])
    x64_module_image = os.path.join(BUILD, "X64MOD.HKM")
    run([py, os.path.join(ROOT, "tools", "mkhakumod.py"),
         x64_demo, x64_modlib, x64_modruntime, x64_module_image,
         "--name", "HAKU-X64"])
    # The 32-bit demo romfs.c is portable C: the same source becomes an
    # amd64 module that publishes a read-only filesystem, which is what
    # proves the capability chain (register -> mount -> VFS dispatch) on
    # this port too.
    x64_romfs = os.path.join(BUILD, "x64_romfs.obj")
    run(cc + module_flags +
        ["/Fo" + x64_romfs, os.path.join(module_dir, "demo", "romfs.c")])
    x64_romfs_image = os.path.join(BUILD, "ROMFS.HKM")
    run([py, os.path.join(ROOT, "tools", "mkhakumod.py"),
         x64_romfs, x64_modlib, x64_modruntime, x64_romfs_image,
         "--name", "HAKU-ROMFS"])
    log("libhakumod built for x86-64 (modlib + runtime64), "
        "X64MOD.HKM and ROMFS.HKM packed")

    # 2. the ring-3 test program: flat binary -> static ELF64 -> embedded
    #    .rdata object. The program outgrew one code page, so it carries its
    #    BSS two pages above the base; mkelf64.py and the assembler get the
    #    same number, and mkelf64.py still refuses a program that overflows.
    user_bss = 0x2000
    run([nasm, "-I" + os.path.join(ROOT, "include"), "-f", "bin",
         f"-DUSER64_BASE={user_base:#x}", f"-DUSER64_BSS_OFFSET={user_bss:#x}",
         user_src, "-o", user_flat])
    run([py, os.path.join(ROOT, "tools", "mkelf64.py"),
         user_flat, user_elf, f"--base={user_base:#x}",
         f"--data-offset={user_bss:#x}"])
    # NASM's incbin accepts forward slashes on Windows and Linux alike, while
    # a backslash in the macro becomes a literal escaped name on a POSIX host.
    embedded = user_elf.replace("\\", "/")
    run([nasm, "-f", "win64", f"-DUSER64_IMAGE={embedded}",
         user_image_asm, "-o", user_image_obj])

    # 2b. the program stored on the disk and run by execve(). Same layout,
    #     same base: it never coexists with the image above because it runs
    #     in a space of its own.
    run([nasm, "-I" + os.path.join(ROOT, "include"), "-f", "bin",
         exec_src, "-o", exec_flat])
    run([py, os.path.join(ROOT, "tools", "mkelf64.py"),
         exec_flat, exec_elf, f"--base={user_base:#x}"])

    # 1c. the x86-64 user C library and /INIT.ELF, the first ring-3 program
    #     built with it. Same pipeline as the 32-bit userland: compile C and
    #     the NASM stubs into a fixed-base PE32+, then repack it as ELF64.
    #     The program is stored on the FAT12 volume rather than linked into
    #     the kernel, so the boot test exercises the disk and ELF loaders.
    lib64_dir = os.path.join(ROOT, "user", "lib64")
    lib64_sources = sorted(glob.glob(os.path.join(lib64_dir, "*.c")))
    init64_source = os.path.join(ROOT, "user", "tests", "init64.c")
    init64_pe = os.path.join(BUILD, "init64.exe")
    init64_elf = os.path.join(BUILD, "INIT.ELF")
    lib64_objects = []
    for source in sorted(glob.glob(os.path.join(lib64_dir, "*.asm"))):
        obj = os.path.join(BUILD, "u64asm_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run([nasm, "-I" + os.path.join(ROOT, "include"), "-f", "win64",
             source, "-o", obj])
        lib64_objects.append(obj)
    for source in lib64_sources + [init64_source]:
        obj = os.path.join(BUILD, "u64lib_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Os", "/W3",
                  "/I" + os.path.join(ROOT, "include"), "/Fo" + obj, source])
        lib64_objects.append(obj)
    run(linker + ["/nologo", "/MACHINE:X64", "/SUBSYSTEM:CONSOLE",
                  "/ENTRY:_start", f"/BASE:{user_base:#x}", "/FIXED",
                  "/DYNAMICBASE:NO", "/NXCOMPAT:NO", "/ALIGN:4096",
                  "/FILEALIGN:512", "/NODEFAULTLIB", "/OUT:" + init64_pe]
        + lib64_objects)
    run([py, os.path.join(ROOT, "tools", "pe2elf64.py"),
         init64_pe, init64_elf])
    log("x86-64 user libc built, /INIT.ELF packed as ELF64")

    run(linker + ["/nologo", "/MACHINE:X64", "/SUBSYSTEM:NATIVE",
                  "/ENTRY:entry64", "/BASE:0x10000", "/ALIGN:16", "/FIXED",
                  "/MAP:" + os.path.join(BUILD, "kernel64.map"),
                  "/NODEFAULTLIB", "/OUT:" + kernel_pe,
                  entry_obj, isr_obj, ap_trampoline64_obj,
                  user_image_obj] + objects)
    pe = subprocess.run([py, os.path.join(ROOT, "tools", "pe2bin.py"),
                         kernel_pe, kernel_bin],
                        capture_output=True, text=True)
    print(pe.stdout, end="")
    if pe.returncode != 0:
        print(pe.stderr, end="")
        sys.exit(1)
    m = re.search(r"^ENTRY=(0x[0-9A-Fa-f]+)\s*$", pe.stdout, re.M)
    if not m:
        sys.exit("pe2bin.py did not report ENTRY=")
    entry = m.group(1)
    log(f"x64 kernel entry: {entry}")

    # 3. the same two boot stages, aimed at the 64-bit entry
    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    kernel_sectors = (os.path.getsize(kernel_bin) + 511) // 512
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}",
         f"-DKERNEL_SECTORS={kernel_sectors}", stage2_asm, "-o", stage2_bin])
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         stage1_bin, stage2_bin, kernel_bin, str(kernel_sectors), img,
         stage1_asm, stage2_asm])

    # 4. a fresh FAT12 volume for the disk filesystem test. The kernel
    #    creates /TMP and writes NOTE.TXT during the run, so the seed files
    #    are rebuilt every time rather than reused.
    build_x64_fs(py, fs_img, exec_elf, x64_module_image, x64_romfs_image,
                 init64_elf)

    # 5. boot it when asked. qemu-system-i386 refuses long mode without a
    #    64-bit CPU model, so every x64 boot uses the x86_64 binary;
    #    writethrough keeps the guest's disk writes in the host file when
    #    QEMU is killed.
    qemu = find_tool("qemu-system-x86_64",
                     [r"C:\Program Files\qemu\qemu-system-x86_64.exe"], "QEMU")
    base = [qemu, "-m", str(memory_mb), "-cpu", "qemu64",
            "-smp", str(smp_cpus),
            "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-drive", f"file={fs_img},format=raw,if=ide,"
                      "index=0,media=disk,cache=writethrough",
            "-boot", "order=a", "-no-reboot"]
    if mode == "build":
        log("x64 build ready: haku64.img + build/haku64_fs.img (not booted)")
        return
    if mode == "run":
        log("Booting Haku x86-64 in QEMU (close the window to return)...")
        subprocess.run(base)
        return

    if os.path.exists(serial):
        os.remove(serial)
    # The human monitor types the key the ring-3 console read blocks on and
    # captures the screen: the framebuffer console only proves itself in the
    # pixels, and the read test only completes when a real scancode arrives
    # through IRQ1. Port 5555 belongs to the 32-bit headless run.
    mon_port = 5556
    ppm = os.path.join(BUILD, "screen64.ppm")
    boot_timeout = float(os.environ.get("HAKU_X64_TIMEOUT", "60"))
    proc = subprocess.Popen(base + ["-display", "none",
                                    "-serial", f"file:{serial}",
                                    "-monitor",
                                    f"tcp:127.0.0.1:{mon_port},"
                                    "server=on,wait=off"])
    if not wait_for_serial(serial, "HAKU64-KEY-READY", boot_timeout):
        log("warning: the ring-3 program never asked for a keystroke")
    else:
        monitor_cmd(mon_port, ["sendkey x"])
    reached = wait_for_serial(serial, "HAKU64-SELFTEST-OK", boot_timeout)
    monitor_cmd(mon_port, [f"screendump {ppm}"])
    proc.kill()
    proc.wait()
    with open(serial, errors="replace") as f:
        content = f.read()
    log("--- serial log (x64) ---")
    log(content)
    required = ["HAKU64-BOOT-OK", "HAKU64-E820-OK", "HAKU64-PAGING-OK",
                "HAKU64-PERCPU-OK", "HAKU64-FRAME-OK", "HAKU64-HEAP-OK",
                "HAKU64-TERMINAL-OK: console=",
                "HAKU64-KBD-OK", "HAKU64-KEY-OK",
                "HAKU64-GDT-OK",
                "HAKU64-IDT-OK", "HAKU64-EXC-OK", "HAKU64-TIMER-OK",
                f"HAKU64-SMP-OK: cpus={smp_cpus}",
                "HAKU64-SMP-AP-OK cpu=1",
                "HAKU64-SMP-LOCK-OK",
                "HAKU64-SMP-AP-GDT-OK",
                "HAKU64-VM-OK", "HAKU64-PROC-OK",
                "HAKU64-SMP-SCHED-OK", "HAKU64-SMP-TLB-OK",
                "HAKU64-USER-OK",
                "HAKU64-SYSCALL-OK", "HAKU64-ELF-OK", "HAKU64-ELF-RUN-OK",
                "HAKU64-ELF-REJECT-OK", "HAKU64-ABI-OK", "HAKU64-NX-OK",
                "HAKU64-RTC-OK", "HAKU64-SYSCALL64-OK", "HAKU64-FS-OK",
                "HAKU64-VFS-OK", "HAKU64-INITRAMFS-OK", "HAKU64-DEVFS-OK",
                "HAKU64-PROCFS-OK",
                "HAKU64-USER-DEV-OK", "HAKU64-USER-INITRAMFS-OK",
                "HAKU64-USER-PROCFS-OK",
                "HAKU64-USER-TLS-OK",
                "HAKU64-USER-THREAD-OK",
                "HAKU64-USER-THREAD-JOIN-OK",
                "HAKU64-USER-PTHREAD-OK",
                "HAKU64-USER-DETACH-OK",
                "HAKU64-USER-ROBUST-OK",
                "HAKU64-USER-PIPE-OK",
                "HAKU64-USER-PIPE-SIGPIPE-OK",
                "HAKU64-USER-PIPE-BLOCK-OK",
                "HAKU64-USER-SIGNAL-OK",
                "HAKU64-USER-STOP-OK",
                "HAKU64-USER-TTY-OK",
                "HAKU64-USER-TTY-SIGNAL-OK",
                "HAKU64-USER-ERRNO-THREAD-OK",
                "HAKU64-USER-THREAD-EXEC-OK",
                "HAKU64-USER-THREAD-EXEC-IMAGE-OK",
                "HAKU64-FILE-OK", "HAKU64-DISK-OK", "HAKU64-FORK-OK",
                "HAKU64-COW-OK",
                "HAKU64-DUP-OK", "HAKU64-EXEC-OK", "HAKU64-EXEC-ARGS",
                "HAKU64-THREAD-OK", "HAKU64-PORT-OK",
                "HAKU64-MODULE-OK", "HAKU64-MODULE-LIB-OK",
                "HAKU64-MODULE-LOAD-OK", "HAKU64-MODULE-REJECT-OK",
                "HAKU64-MODULE-DEV-OK",
                "HAKU64-MODULE-FS-OK", "HAKU64-MODULE-FS-RELEASE-OK",
                "HAKU64-USER-MODULE-OK",
                "HAKU64-MODULE-UNLOAD-OK", "HAKU64-LIBC-OK",
                "HAKU64-SELFTEST-OK"]
    if smp_cpus == 1:
        # A uniprocessor run reports the two SMP scheduling tests as skipped,
        # and the AP markers cannot appear at all.
        required = [m for m in required if not m.startswith("HAKU64-SMP")]
    missing = [marker for marker in required if marker not in content]
    if "HAKU64-FAIL" in content:
        for line in content.splitlines():
            if "HAKU64-FAIL" in line:
                sys.exit(f"X64 CHECK FAILED: {line.strip()}")
    if "HAKU64-FB-OK" in content:
        # The console must have painted glyphs, not just reported the mode. The
        # foreground colour is the proof: the screen is a plain text grid, so a
        # few hundred pixels of it can only come from the font.
        if not os.path.exists(ppm):
            missing.append("framebuffer active but no screendump captured")
        elif not ppm_count_color(ppm, (0xD0, 0xD0, 0xD0), 200):
            missing.append("no console text in the screendump")
    elif "HAKU64-FB-SKIP" not in content:
        missing.append("no framebuffer status line in the serial log")
    if missing or not reached:
        sys.exit("X64 CHECK FAILED: missing markers: " + ", ".join(missing))

    # The run created /disk/NOTE.TXT and flushed it through the ATA cache, so
    # a layout check of the host-side image proves the write reached the
    # volume, not just the kernel's page cache. The bytes are compared too:
    # a valid layout that spells the wrong marker would still be a bug.
    check = subprocess.run([py, os.path.join(ROOT, "tools", "lsfat12.py"),
                            "--check", fs_img], capture_output=True, text=True)
    print(check.stdout, end="")
    if check.returncode != 0:
        print(check.stderr, end="")
        sys.exit("X64 CHECK FAILED: the disk image is not valid after the run")
    note = subprocess.run([py, os.path.join(ROOT, "tools", "lsfat12.py"),
                           "--cat", "NOTE.TXT", fs_img], capture_output=True)
    if note.returncode != 0 or note.stdout != b"HAKU64-DISK-OK\r\n":
        sys.exit("X64 CHECK FAILED: /disk/NOTE.TXT did not reach the volume")
    log("X64 CHECK PASSED: " + ", ".join(required))


def run(cmd):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def ppm_count_color(path, rgb, minimum):
    """True when a binary PPM (P6) holds at least `minimum` pixels of an RGB
    triple. Used to prove the console painted glyphs rather than just
    reporting the mode: one character is already dozens of pixels, so a small
    threshold separates "text drawn" from "a stray pixel"."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return False
    fields = []
    index = 2
    while len(fields) < 3 and index < len(data):
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while index < len(data) and data[index:index + 1] != b"\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    if len(fields) < 3:
        return False
    width, height, _maxval = fields
    pixels = data[index + 1:]
    red, green, blue = rgb
    count = 0
    for offset in range(0, min(len(pixels), width * height * 3) - 2, 3):
        if (pixels[offset] == red and pixels[offset + 1] == green
                and pixels[offset + 2] == blue):
            count += 1
            if count >= minimum:
                return True
    return False


def main():
    nasm = find_tool("nasm", [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), r"bin\NASM\nasm.exe"),
        r"C:\Program Files\NASM\nasm.exe",
    ], "NASM  [winget install NASM.NASM]")
    py = sys.executable
    log(f"NASM: {nasm}")

    raw_args = sys.argv[1:]
    positionals = [a for a in raw_args if not a.startswith("-")]
    mode = positionals[0] if positionals else "build"

    # x86-64 is the primary image. --arch=i386 selects the original
    # 32-bit pipeline, which still owns the shell, the user C library and the
    # probes; those modes force it because their userland does not exist on
    # the 64-bit side yet.
    arch = "x64"
    arch_explicit = False

    if mode == "inspect":
        # Read-only forensics: needs no toolchain, never builds anything.
        rest = [a for a in raw_args if a != "inspect"]
        if not any(a.lower().endswith(".img") for a in rest):
            rest = [os.path.join(BUILD, "haku_fs.img")] + rest
        rc = subprocess.run([sys.executable,
                             os.path.join(ROOT, "tools", "lsfat12.py"),
                             ] + rest).returncode
        sys.exit(rc)

    init_override = None
    expectations = []
    memory_mb = 512
    smp_cpus = 2
    smp_explicit = False
    busybox_path = None
    boot_command = None
    boot_check = False
    for flag in raw_args:
        if flag == "--fresh":
            continue
        if flag == "--check":
            boot_check = True
            continue
        if flag.startswith("--busybox="):
            busybox_path = flag.split("=", 1)[1]
            continue
        if flag.startswith("--cmd="):
            boot_command = flag.split("=", 1)[1]
            continue
        if flag.startswith("--init="):
            init_override = flag.split("=", 1)[1]
            continue
        if flag.startswith("--expect="):
            expectations.append(flag.split("=", 1)[1])
            continue
        if flag.startswith("--arch="):
            value = flag.split("=", 1)[1]
            if value not in ("x64", "i386"):
                sys.exit("--arch takes x64 or i386")
            arch = value
            arch_explicit = True
            continue
        if flag.startswith("--memory="):
            # The kernel sizes its direct map and frame pool from E820, so
            # the guest RAM is what it actually gets to use.
            value = flag.split("=", 1)[1]
            if not value.isdigit() or not 16 <= int(value) <= 4096:
                sys.exit("--memory=MB takes 16..4096 "
                         "(the kernel clips its direct map at 1020 MiB)")
            memory_mb = int(value)
            continue
        if flag.startswith("--smp="):
            # The per-CPU scheduler is validated at the default two CPUs; the
            # task table has sixteen slots and each CPU costs an idle task, so
            # more than eight would not fit the kernel's own limit.
            value = flag.split("=", 1)[1]
            if not value.isdigit() or not 1 <= int(value) <= 8:
                sys.exit("--smp=N takes 1..8 (the kernel's CPU limit)")
            smp_cpus = int(value)
            smp_explicit = True
            continue
        if flag.startswith("-"):
            sys.exit(f"unknown flag: {flag} (use --arch=x64|i386, --fresh, "
                     "--memory=MB, --smp=N, --init=PATH, --expect=MARKER, "
                     "--busybox=PATH, --cmd=TEXT, --check)")
    fresh = "--fresh" in raw_args
    if len(positionals) > 1:
        sys.exit(f"unexpected argument: {positionals[1]}")
    if mode == "probe":
        if not init_override:
            sys.exit("probe needs --init=PATH: the ELF to run as /init")
        if not os.path.isfile(init_override):
            sys.exit(f"no such file: {init_override}")
        if not expectations:
            sys.exit("probe needs at least one --expect=MARKER")
    elif mode == "shell":
        if boot_command is None:
            boot_command = (SHELL_CHECK_SCRIPT if boot_check
                            else SHELL_INTERACTIVE_SCRIPT)
        elif init_override or expectations:
            sys.exit("--init and --expect do not apply to `shell`")
    elif mode == "rootfs":
        if boot_check and boot_command is None:
            boot_command = ROOTFS_CHECK_SCRIPT
        if init_override or expectations:
            sys.exit("--init and --expect do not apply to `rootfs`")
    elif init_override or expectations:
        sys.exit("--init and --expect only apply to `probe`")
    if boot_check and mode not in ("shell", "rootfs"):
        sys.exit("--check only applies to `shell` and `rootfs`")

    if mode == "x64":
        build_x64(nasm, py, memory_mb, smp_cpus, "headless")
        return

    if mode in ("probe", "shell", "rootfs"):
        if arch_explicit and arch != "i386":
            sys.exit("`probe`, `shell` and `rootfs` run the 32-bit kernel; "
                     "use --arch=i386")
        arch = "i386"
    if mode in ("build", "run", "headless") and arch == "x64":
        build_x64(nasm, py, memory_mb, smp_cpus, mode)
        return
    if smp_explicit and arch == "i386" and smp_cpus > 4:
        sys.exit("--smp=N takes 1..4 for the 32-bit image: its task table "
                 "has four slots and the boot test needs one per AP")

    cc, linker, toolchain = detect_toolchain()
    qemu = find_tool("qemu-system-i386",
                     [r"C:\Program Files\qemu\qemu-system-i386.exe"], "QEMU")
    log(f"CC:   {' '.join(cc)}")
    log(f"LINK: {' '.join(linker)}")
    log(f"toolchain: {toolchain}")

    # clang-cl inherits MSVC's /arch:SSE2 default and uses SSE2 for
    # constant-size memory operations; MSVC keeps integer code scalar but
    # may still emit SSE2 for other patterns. Haku never sets CR4.OSFXSR, so
    # an SSE instruction faults with #UD at ring 0 and ring 3, and the kernel
    # does not save XMM state across a task switch either. Build i386 objects
    # for a CPU without vector registers, on both toolchains.
    no_sse = (["/arch:IA32"] if toolchain.startswith("MSVC")
              else ["-mno-sse", "-mno-sse2", "-mno-mmx"])

    kernel_dir = os.path.join(ROOT, "kernel")
    arch_dir = os.path.join(kernel_dir, "arch", "x86")
    include_dir = os.path.join(ROOT, "include")
    verify_abi_headers(include_dir)
    c_files = sorted(glob.glob(os.path.join(kernel_dir, "**", "*.c"),
                                recursive=True))
    # The 64-bit sources live under kernel/arch/x86_64 and are built by
    # `build.py x64`; a 32-bit object file and an x64 one cannot share a
    # link line, so keep them out of this glob.
    c_files = [p for p in c_files
               if os.path.join("arch", "x86_64") + os.sep not in p]
    # kernel/module/lib is libhakumod, the runtime a *module* links against,
    # and kernel/module/demo holds the example modules themselves. Both are
    # compiled per module further down, never into the kernel image.
    module_lib_dir = os.path.join(kernel_dir, "module", "lib")
    module_demo_dir = os.path.join(kernel_dir, "module", "demo")
    c_files = [p for p in c_files
               if not p.startswith(module_lib_dir + os.sep)
               and not p.startswith(module_demo_dir + os.sep)]
    if not c_files:
        sys.exit("no kernel/**/*.c files found")
    entry_asm = os.path.join(arch_dir, "kernel", "entry.asm")
    isr_asm = os.path.join(arch_dir, "kernel", "isr.asm")
    ap_trampoline_asm = os.path.join(arch_dir, "kernel", "ap_trampoline.asm")
    user_asm = os.path.join(arch_dir, "user", "user.asm")
    exec_target_asm = os.path.join(arch_dir, "user", "exec_target.asm")
    child_asm = os.path.join(arch_dir, "user", "child.asm")
    vfork_asm = os.path.join(arch_dir, "user", "vfork_target.asm")
    fault_asm = os.path.join(arch_dir, "user", "fault.asm")
    boot_dir = os.path.join(arch_dir, "boot")
    stage1_asm = os.path.join(boot_dir, "stage1.asm")
    stage2_asm = os.path.join(boot_dir, "stage2.asm")
    # Every kernel header directory is an include path so flat
    # #include "foo.h" directives keep working across subdirectories
    # (Linux-style ccflags; header names are unique across the tree).
    kernel_header_dirs = [
        os.path.join(arch_dir, "kernel"),
        os.path.join(arch_dir, "mm"),
        os.path.join(arch_dir, "elf"),
        os.path.join(kernel_dir, "drivers"),
        os.path.join(kernel_dir, "fs", "fat12"),
        os.path.join(kernel_dir, "fs", "vfs"),
        os.path.join(kernel_dir, "fs", "test"),
        os.path.join(kernel_dir, "fs", "ramfs"),
        os.path.join(kernel_dir, "fs", "devfs"),
        os.path.join(kernel_dir, "fs", "procfs"),
        os.path.join(kernel_dir, "fs", "initramfs"),
        os.path.join(kernel_dir, "kernel"),
        os.path.join(kernel_dir, "module"),
    ]
    fat12_script = os.path.join(ROOT, "tools", "mkfat12.py")
    cpio_script = os.path.join(ROOT, "tools", "mkcpio.py")
    entry_obj = os.path.join(BUILD, "entry.obj")
    isr_obj = os.path.join(BUILD, "isr.obj")
    ap_trampoline_obj = os.path.join(BUILD, "ap_trampoline.obj")
    user_elf = os.path.join(BUILD, "user.elf")
    exec_target_elf = os.path.join(BUILD, "exec_target.elf")
    child_elf = os.path.join(BUILD, "child.elf")
    vfork_elf = os.path.join(BUILD, "vfork_target.elf")
    fault_elf = os.path.join(BUILD, "fault.elf")
    user_lib_dir = os.path.join(ROOT, "user", "lib")
    user_lib_sources = sorted(glob.glob(os.path.join(user_lib_dir, "*.c")))
    user_test_source = os.path.join(ROOT, "user", "tests", "libtest.c")
    user_start_asm = os.path.join(user_lib_dir, "crt0.asm")
    user_syscall_asm = os.path.join(user_lib_dir, "syscall.asm")
    libtest_pe = os.path.join(BUILD, "libtest.exe")
    libtest_elf = os.path.join(BUILD, "lib.elf")
    pe2elf_script = os.path.join(ROOT, "tools", "pe2elf.py")
    filesystem_image = os.path.join(BUILD, "haku_fs.img")
    spare_image = os.path.join(BUILD, "ata_spare.img")
    kernel_pe = os.path.join(BUILD, "kernel.pe")
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    stage1_bin = os.path.join(BUILD, "stage1.bin")
    stage2_bin = os.path.join(BUILD, "stage2.bin")
    img = os.path.join(ROOT, "haku.img")

    # 1. User ELFs are always rebuilt (build artifacts), but the FAT12 data
    #    disk is user data: an existing image is reused untouched so files
    #    from previous QEMU sessions survive, and mkfat12 runs only for an
    #    initial image or an explicit --fresh. headless always recreates —
    #    its fault-injection tests require pristine, deterministic state.
    #    The shell and rootfs images are generated artifacts instead, so
    #    they are rebuilt below once their userland exists.
    nasm_include = include_dir + os.sep
    run([nasm, "-I" + nasm_include, "-f", "bin", user_asm, "-o", user_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", exec_target_asm,
         "-o", exec_target_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", child_asm, "-o", child_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", vfork_asm, "-o", vfork_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", fault_asm, "-o", fault_elf])

    # Modules are packed into the kernel's .hkm container on the host, so
    # the kernel never learns an object format. hello and romfs are C,
    # linked by the packer with libhakumod, the freestanding runtime a
    # module links against; haku_demo stays a NASM ELF32 object and keeps
    # that host path alive. /Gy gives every function its own COMDAT so the
    # packer can drop library helpers a module never calls.
    module_pack_script = os.path.join(ROOT, "tools", "mkhakumod.py")
    module_c_flags = (["/nologo", "/c", "/GS-", "/Zl", "/Os", "/Gy", "/W3"]
                      + no_sse + ["/I" + include_dir])
    lib_object = os.path.join(BUILD, "modlib_module_lib.obj")
    run(cc + module_c_flags +
        ["/Fo" + lib_object, os.path.join(module_lib_dir, "module_lib.c")])
    runtime_object = os.path.join(BUILD, "modlib_runtime.obj")
    run([nasm, "-f", "win32", os.path.join(module_lib_dir, "runtime.asm"),
         "-o", runtime_object])
    module_runtime = [lib_object, runtime_object]
    log("libhakumod built (module_lib.obj + runtime.obj)")

    module_specs = [
        ("haku_demo.asm", "MODULE.HKM", "HAKU-DEMO"),
        ("hello.c", "HELLO.HKM", "HAKU-HELLO"),
        ("romfs.c", "ROMFS.HKM", "HAKU-ROMFS"),
    ]
    module_sources = []
    module_archives = []
    for source_name, image_name, module_name in module_specs:
        module_source = os.path.join(kernel_dir, "module", "demo",
                                     source_name)
        stem = os.path.splitext(source_name)[0]
        objects = []
        if source_name.endswith(".c"):
            module_object = os.path.join(BUILD, "mod_" + stem + ".obj")
            run(cc + module_c_flags + ["/Fo" + module_object, module_source])
            objects.append(module_object)
            objects += module_runtime
        else:
            module_object = os.path.join(BUILD, stem + ".o")
            run([nasm, "-f", "elf32", module_source, "-o", module_object])
            objects.append(module_object)
        module_image = os.path.join(BUILD, image_name)
        run([py, module_pack_script] + objects +
            [module_image, "--name", module_name])
        module_sources.append(module_source)
        module_archives.append(f"{image_name}={module_image}")

    # The user C library is linked into a fixed-base PE image, then repacked
    # as ELF32 so the existing loader can map RX/R/RW sections.
    user_objects = []
    for source in (user_start_asm, user_syscall_asm):
        obj = os.path.join(BUILD, "uasm_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run([nasm, "-I" + include_dir, "-f", "win32", source, "-o", obj])
        user_objects.append(obj)
    for source in user_lib_sources:
        obj = os.path.join(BUILD, "ulib_" +
                           os.path.splitext(os.path.basename(source))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/O2", "/Oi-", "/W3"]
            + no_sse + ["/I" + include_dir, "/Fo" + obj, source])
        user_objects.append(obj)
    user_test_obj = os.path.join(BUILD, "ulib_libtest.obj")
    run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Od", "/Oi-", "/W3"]
        + no_sse + ["/I" + include_dir, "/Fo" + user_test_obj,
                    user_test_source])
    library_objects = user_objects
    run(linker + ["/nologo", "/MACHINE:X86", "/SUBSYSTEM:CONSOLE",
                  "/ENTRY:start", "/BASE:0x40000000", "/FIXED",
                  "/DYNAMICBASE:NO", "/NXCOMPAT:NO", "/SAFESEH:NO",
                  "/ALIGN:4096", "/FILEALIGN:512", "/NODEFAULTLIB",
                  "/OUT:" + libtest_pe] + library_objects + [user_test_obj])
    run([py, pe2elf_script, libtest_pe, libtest_elf])

    # The shell and rootfs targets boot their own data disks. Both are
    # generated on every run (unlike haku_fs.img, which is user data) and
    # both are built here because they need the userland above: the shell
    # image carries the boot ELF, the rootfs image carries /INIT.
    if mode == "shell":
        filesystem_image = build_shell_image(py, user_elf,
                                             shell_busybox(busybox_path),
                                             boot_command)
    elif mode == "rootfs":
        rootfs_init_source = os.path.join(ROOT, "user", "rootfs", "init.c")
        rootfs_init_obj = os.path.join(BUILD, "rootfs_init.obj")
        rootfs_init_pe = os.path.join(BUILD, "rootfs_init.exe")
        rootfs_init_elf = os.path.join(BUILD, "INIT")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Od", "/Oi-", "/W3"]
            + no_sse + ["/I" + include_dir, "/Fo" + rootfs_init_obj,
                        rootfs_init_source])
        run(linker + ["/nologo", "/MACHINE:X86", "/SUBSYSTEM:CONSOLE",
                      "/ENTRY:start", "/BASE:0x40000000", "/FIXED",
                      "/DYNAMICBASE:NO", "/NXCOMPAT:NO", "/SAFESEH:NO",
                      "/ALIGN:4096", "/FILEALIGN:512", "/NODEFAULTLIB",
                      "/OUT:" + rootfs_init_pe]
            + library_objects + [rootfs_init_obj])
        run([py, pe2elf_script, rootfs_init_pe, rootfs_init_elf])
        log("rootfs /INIT built and packed as ELF32")
        filesystem_image = build_rootfs_image(py, rootfs_init_elf,
                                              shell_busybox(busybox_path),
                                              boot_command)

    # The initramfs holds the userland image the kernel unpacks into its RAM
    # filesystem at boot: /init is the first program, and the helper
    # programs stay reachable under their old names so the existing tests
    # exercise the RAM filesystem instead of the data disk.
    initramfs_image = os.path.join(BUILD, "initrd.cpio")
    run([py, cpio_script, initramfs_image,
         f"init={init_override or user_elf}", f"HAKU.ELF={user_elf}",
         f"NEXT.ELF={exec_target_elf}", f"CHILD.ELF={child_elf}",
         f"VFORK.ELF={vfork_elf}", f"FAULT.ELF={fault_elf}",
         f"LIB.ELF={libtest_elf}", *module_archives,
         "--dir=dev", "--dir=proc", "--dir=disk"])

    if mode in ("shell", "rootfs"):
        # Built above from the generated tree; never treated as user data.
        pass
    elif mode in ("headless", "probe") or fresh or \
            not os.path.isfile(filesystem_image):
        if mode in ("headless", "probe") and os.path.isfile(filesystem_image):
            log(f"{mode}: recreating the data image "
                "(the run mutates the disk)")
        run([py, fat12_script, user_elf, filesystem_image,
             f"NEXT.ELF={exec_target_elf}", f"CHILD.ELF={child_elf}",
             f"FAULT.ELF={fault_elf}", f"LIB.ELF={libtest_elf}",
             f"VFORK.ELF={vfork_elf}", f"INITRD={initramfs_image}",
             *module_archives])
        log(f"FAT12 program disk ready "
            f"({os.path.getsize(filesystem_image)} bytes)")
    else:
        log(f"reusing data image {filesystem_image} "
            f"({os.path.getsize(filesystem_image)} bytes, "
            f"user files preserved; --fresh recreates it)")
        source_mtime = max(os.path.getmtime(user_asm),
                           os.path.getmtime(exec_target_asm),
                           os.path.getmtime(child_asm),
                           os.path.getmtime(vfork_asm),
                           os.path.getmtime(fault_asm),
                           max(os.path.getmtime(source)
                               for source in module_sources),
                           os.path.getmtime(user_test_source),
                           max(os.path.getmtime(source)
                               for source in user_lib_sources))
        if source_mtime > os.path.getmtime(filesystem_image):
            log(f"WARNING: {os.path.basename(user_asm)} is newer than the data image - "
                "test ELFs on disk may be stale; rerun with --fresh")

    # 2. kernel C files -> COFF objects
    c_objs = []
    for src in c_files:
        obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Os", "/W3"]
            + no_sse + ["/I" + include_dir]
            + ["/I" + d for d in kernel_header_dirs]
            + ["/Fo" + obj, src])
        c_objs.append(obj)
    log(f"{len(c_objs)} C objects built")

    # 3. asm stubs -> COFF (entry linked FIRST so the image starts there)
    run([nasm, "-I" + nasm_include, "-f", "win32", entry_asm, "-o", entry_obj])
    run([nasm, "-I" + nasm_include, "-f", "win32", isr_asm, "-o", isr_obj])
    run([nasm, "-I" + nasm_include, "-f", "win32", ap_trampoline_asm,
         "-o", ap_trampoline_obj])
    log("entry.obj + isr.obj + ap_trampoline.obj built")

    # 4. link a fixed-base PE (/ENTRY:entry — link prepends _ for x86;
    #    /ALIGN:16 packs sections; /FIXED drops .reloc — we always load
    #    at the linked base, so runtime relocation is meaningless)
    run(linker + ["/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/FIXED", "/NODEFAULTLIB",
         "/MAP:" + os.path.join(BUILD, "kernel.map"),
         "/OUT:" + kernel_pe, entry_obj, isr_obj, ap_trampoline_obj] + c_objs)
    log("kernel.pe linked")

    # 4. PE -> flat binary, learn the entry address
    pe = subprocess.run([py, os.path.join(ROOT, "tools", "pe2bin.py"),
                         kernel_pe, kernel_bin],
                        capture_output=True, text=True)
    print(pe.stdout, end="")
    if pe.returncode != 0:
        print(pe.stderr, end="")
        sys.exit(1)
    m = re.search(r"^ENTRY=(0x[0-9A-Fa-f]+)\s*$", pe.stdout, re.M)
    if not m:
        sys.exit("pe2bin.py did not report ENTRY=")
    entry = m.group(1)
    log(f"kernel entry: {entry}")

    # 5. boot stages (stage2 gets the real entry address)
    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    kernel_sectors = (os.path.getsize(kernel_bin) + 511) // 512
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}",
         f"-DKERNEL_SECTORS={kernel_sectors}", stage2_asm, "-o", stage2_bin])

    # 6. floppy image (all size/signature/sector checks live in mkimg.py)
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         stage1_bin, stage2_bin, kernel_bin, str(kernel_sectors), img,
         stage1_asm, stage2_asm])

    with open(spare_image, "wb") as f:
        f.truncate(1474560)

    disk = [qemu, "-m", str(memory_mb),
            "-smp", str(smp_cpus),
            "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-drive", f"file={filesystem_image},format=raw,if=ide,index=0,media=disk",
            "-drive", f"file={spare_image},format=raw,if=ide,index=2,media=disk",
            "-boot", "order=a", "-no-reboot"]
    # CPU exception trace: shows every #GP/#PF/triple-fault with EIP.
    trace = ["-d", "int,cpu_reset", "-D", os.path.join(BUILD, "qemu.log")]
    if mode == "run":
        log("Booting Haku in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode in ("shell", "rootfs") and not boot_check:
        if mode == "shell":
            log("Booting the Haku shell in QEMU (close the window to return)...")
            log("busybox ash is pid 1: type a command at the prompt")
        else:
            log("Booting the Haku rootfs in QEMU "
                "(close the window to return)...")
            log("/INIT starts /bin/busybox as pid 1; type exit to stop")
        subprocess.run(disk)
    elif mode in ("shell", "rootfs"):
        name = "shell" if mode == "shell" else "rootfs"
        done_marker, expected = (("SHELL-DONE", SHELL_EXPECTED)
                                 if mode == "shell"
                                 else ("ROOTFS-DONE", ROOTFS_EXPECTED))
        serial = os.path.join(BUILD, "serial.log")
        if os.path.exists(serial):
            os.remove(serial)
        proc = subprocess.Popen(disk + ["-display", "none",
                                        "-serial", f"file:{serial}"])
        reached = wait_for_serial(
            serial, done_marker,
            float(os.environ.get("HAKU_SHELL_TIMEOUT" if mode == "shell"
                                 else "HAKU_ROOTFS_TIMEOUT", "300")))
        proc.kill()
        proc.wait()
        with open(serial, errors="replace") as f:
            content = f.read()
        log(f"--- serial log ({name}) ---")
        log(content)
        missing = [marker for marker in expected if marker not in content]
        applets = None
        if mode == "shell":
            # The applet count is the one thing a marker cannot express: it
            # proves that /bin was filled and that a directory listing came
            # back through getdents, so a boot that reaches the prompt but
            # cannot run anything still fails here.
            applets = re.search(r"APPS:(\d+)", content)
            if not applets or int(applets.group(1)) < 100:
                missing.append("APPS:<count >= 100>")
        if "PANIC" in content or "FAIL" in content:
            for line in content.splitlines():
                if "PANIC" in line or "FAIL" in line:
                    log("  " + line.strip())
                    missing.append("no PANIC/FAIL")
        if mode == "shell":
            if "can't access tty" in content or \
                    "job control turned off" in content:
                missing.append("no tty warning")
            if "can't set tty process group" in content:
                missing.append("no process-group error")
        if missing or not reached:
            sys.exit(f"{name.upper()} CHECK FAILED: missing "
                     + ", ".join(missing))
        if mode == "shell":
            log(f"SHELL CHECK PASSED: busybox ash ran as pid 1, "
                f"{applets.group(1)} applets resolved through $PATH, "
                f"uname and /proc answered, job control enabled, "
                f"a foreground pipeline forked and ran")
        else:
            log("ROOTFS CHECK PASSED: /INIT ran as pid 1 on the FAT12 root, "
                "/etc/motd and /etc/rc were read, busybox parsed the script, "
                "applets ran, and /proc and / listed")
    elif mode in ("headless", "probe"):
        serial = os.path.join(BUILD, "serial.log")
        shot = os.path.join(BUILD, "screen.png")
        ppm = os.path.join(BUILD, "screen.ppm")
        if os.path.exists(serial):
            os.remove(serial)
        # Human monitor on TCP (for screendump now, sendkey later).
        MON_PORT = 5555
        proc = subprocess.Popen(disk + trace + ["-display", "none",
                                                 "-serial", f"file:{serial}",
                                        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server=on,wait=off"])
        try:
            # The initial test process blocks once in read(0, ..., 1).
            # Waiting for the marker that the kernel prints at that moment
            # is deterministic: a fixed sleep lost the keystroke whenever
            # boot ran slower than the guess (a bigger guest RAM, a slower
            # CI runner), which showed up as a whole run of missing USER-*
            # markers. A probe does not read the console, so it gets no
            # keystroke.
            if mode == "headless":
                if not wait_for_serial(
                        serial, "SCHED-WAITQ-OK",
                        float(os.environ.get("HAKU_BOOT_DELAY", "15"))):
                    log("warning: pid 1 never parked on the console read")
                monitor_cmd(MON_PORT, ["sendkey x"])
            proc.wait(timeout=float(
                os.environ.get("HAKU_HEADLESS_TIMEOUT", "30")))
        except subprocess.TimeoutExpired:
            # Still alive: interrogate the CPU state, grab a screenshot, stop it.
            regs = monitor_cmd(MON_PORT, ["info registers", f"screendump {shot}",
                                              f"screendump {ppm}",
                                              "xp /3wx 0x500",
                                              "xp /64wx 0x11240",
                                              "xp /64wx 0x11980"])
            with open(os.path.join(BUILD, "regs.txt"), "wb") as f:
                f.write(regs)
            for line in regs.decode("utf-8", "replace").splitlines():
                if line.startswith(("EIP", "EAX", "CS ", "CR0", "EFL")):
                    log("  " + line.strip())
            if os.path.exists(shot):
                log(f"screenshot: {shot}")
            proc.kill()
            proc.wait()
        time.sleep(0.5)
        if not os.path.exists(serial):
            sys.exit("no serial output captured")
        with open(serial, errors="replace") as f:
            content = f.read()
        log("--- serial log ---")
        log(content)
        # Commit-by-commit boot proof: every marker below must be present.
        # `probe` boots a program built elsewhere (a musl binary) and only
        # asserts the boot path plus the markers the caller named.
        # Per-CPU stage: one block per CPU, two CPUs scheduling two tasks,
        # and a TLB shootdown that reaches every other CPU. A uniprocessor
        # run reports the two scheduling tests as skipped and has no AP
        # markers at all, so the list is built from the CPU count.
        smp_markers = ["PERCPU-OK", "SMP-PERCPU-OK"]
        if smp_cpus >= 2:
            smp_markers += [f"SMP-OK cpus={smp_cpus}",
                            "SMP-AP-OK cpu=1",  # trampoline, then per-CPU GDT
                            "SMP-LOCK-OK",      # one lock across CPUs
                            "SMP-AP-GDT-OK",    # AP loaded its own TSS/IDT
                            "SMP-SCHED-OK",     # an AP ran its own task
                            "SMP-TLB-OK"]       # shootdown reached every CPU
        else:
            smp_markers += ["SMP-OK cpus=1", "SMP-LOCK-OK",
                            "SMP-SCHED-SKIP", "SMP-TLB-SKIP"]
        headless_expected = [
            "HAKU-BOOT-OK",  # kernel_main reached
            "IDT-OK",        # 48 IDT gates loaded
            "EXC-03-BP",     # INT3 self-test dispatched through the IDT
            "INT3-RESUMED",  # handler returned, execution resumed
            "KLOG-LEVELS-OK",# leveled logging gates DEBUG at the INFO threshold
            "PAGING-OK",     # 8 MiB identity map, CR0.PG on, R/W probe passed
            "HEAP-OK",       # kmalloc/kfree/krealloc self-test passed
            "E820-OK",       # stage2 INT 15h map handoff verified
            "FRAME-OK",      # frame_alloc/free self-test passed
            "MEM-OK",        # direct map and frame pool sized from E820
            *smp_markers,
            "GDT-OK",        # kernel/user selectors and ring-0 TSS stack loaded
            "ELF-REJECT-OK", # malformed images and W+X segments were rejected
            "ELF-LOAD-OK",   # valid ELF32 PT_LOAD segments loaded to fresh CR3
            "USERMODE-READY",# user stack mapped in its own address space
            "ATA-OK primary-master LBA28",
            "ATA-SCAN-OK devices=2",
            "secondary-master LBA28", # spare disk on the second IDE channel
            "ATA-RECOVERY-OK",   # software reset + reread after a recovery
            "ATA-MULTI-OK",     # second channel device answers sector reads
            "ATA-CACHE-OK",     # repeated sector read served from the cache
            "FAT12-MOUNT-OK",
            "INITRAMFS-UNPACK-OK",
            "INITRAMFS-ROOT: ramfs is /, disk is /disk, /init is pid 1",
            "INIT-READ-OK name=init",
            "USERMODE-ENTER",# iret entered CPL 3
            "SCHED-READY tasks=1 kernel-stack=per-task",
            "SCHED-START pid=1",
            "SCHED-TIMER-OK",
            "SCHED-WAITQ-OK", # blocked read left the run queue; idle ran instead
            "USERMODE-OK",   # ELF32 user image executed POSIX-style syscalls
            "USER-SYSCALL-CHECKS-OK", # EFAULT/EBADF/EINVAL/ENOSYS + flag/fd/whence paths passed
            "USER-READ-OK",  # blocking read copied keyboard input to user memory
            "USER-FILE-IO-OK", # FAT12 create/write/read/seek/close passed
            "USER-FILE-OPS-OK", # unlink/rename/stat/fstat, open-file removed/renamed
            "USER-DUP-OK",   # dup/dup2 share one open file description
            "STDIO-OK",      # standard descriptors 0/1/2 installed on the console
            "USER-STDIO-OK", # stdio redirect via dup2, close, and restore
            "SPAWN-CHILD-RAN",
            "USER-FORK-OK",   # fork/wait4/exit_group round trip
            "USER-COW-OK",    # forked pages are private once either side writes
            "USER-VFORK-OK",  # vfork shares the space; parent wakes on exit/exec
            "VFORK-EXEC-RAN",
            "USER-WAITPID-OK child status=42 reaped",
            "USER-FD-INHERIT-OK", # spawn inherited fd 3; child exit closed it
            "USER-TIME-OK",  # gettimeofday reads ticks; nanosleep blocks until wake
            "USER-MEM-OK",   # brk grows/shrinks; mmap/munmap allocate and release pages
            "USER-DIR-OK",   # subdirectories, paths, and "."/".." resolution
            "USER-USAGE-OK", # getusage reports per-process memory and CPU counters
            "USER-PROT-OK",  # RX/read-only stores, unmapped reads, and freed pages fault
            "CRASH-REPORT-OK", # faulted user frame dumped with registers and CRs
            "EXECVE-OK pid=1", # exec replaced the image and preserved descriptors
            "USER-EXEC-TARGET-OK", # argv/envp stack and image replacement passed
            "USER-SIGNAL-OK", # kill/tkill/tgkill/sigaction/sigprocmask delivery
            "USER-MMAP-OK",  # mmap2/mprotect/mremap/madvise/set_thread_area
            "USER-FUTEX-OK", # futex wait/wake with a timeout
            "USER-THREAD-OK", # clone shares space/files, distinct tid, same pid
            "USER-PTHREAD-OK", # pthread create/join, mutex, condition variable
            "USER-DETACH-OK",  # a detached pthread frees its own pages
            "USER-ROBUST-OK",  # robust mutex EOWNERDEAD/consistent/poison
            "USER-ERRNO-THREAD-OK", # errno is per thread
            "USER-FS-ABI-OK", # openat/getdents64/chdir/pipe/poll/fcntl/termios
            "USER-ERRNO-OK", # error matrix: bad fd/pointer/path/argument
            "USER-INITRAMFS-OK", # ramfs root, long names, symlinks, devfs, procfs
            "SCHED-CONCURRENT-OK", # two tasks doing disk I/O at once
            "USER-LIB-OK",   # C library syscalls/strings/malloc/file I/O
            "USER-MODULE-OK", # init_module/delete_module loaded the demo module
            "HAKU-MODULE-OK", # the module's relocated ring-0 init really ran
            "HAKU-MODULE-EXIT", # delete_module called the module's exit hook
            "MODULE-LOAD-OK name=HAKU-DEMO",
            "MODULE-UNLOAD-OK name=HAKU-DEMO",
            "USER-MODDEV-OK",  # a module registered /dev/hello and it read back
            "HELLO-MODULE-OK",
            "HELLO-LIB-OK",    # the C module's runtime helpers computed correctly
            "USER-ROMFS-OK",   # a module mounted a read-only filesystem at /rom
            "ROMFS-MODULE-OK",
            "USER-EXIT-OK status=0 pid=1",
            "SCHED-REAP-OK",
            "FS-ERRORS-OK",  # missing/invalid/exists/badfd map to distinct errno
            "FS-EROFS-OK",   # read-only mount rejects every mutating path
            "FS-FILE-EDGE-OK", # empty files, exact-sector, span, partial writes
            "FS-CORRUPT-OK", # cut/bad/reserved/cyclic chains + data-full ENOSPC
            "FS-ATA-OK",     # ATA bounds/null transfers rejected
            "FS-STAT-OK",    # stat/fstat metadata: mode/size/nlink/blocks
            "FS-ROLLBACK-OK", # failed writes roll back size/clusters
            "FS-ENOSPC-OK",  # full root directory refused with ENOSPC
            "PIC-OK",        # 8259 remapped to 0x20/0x28
            "PIT-OK",        # 100 Hz timer programmed
            "KBD-OK",        # PS/2 keyboard IRQ1 handler installed
            "TERMINAL-OK",   # terminal/display abstraction selected a backend
            "TERMINAL-CSI-OK", # ESC[6n consumed and answered on stdin
            "T+1s",          # hardware timer IRQs arriving
            "T+2s",
            "T+3s",
        ]
        if mode == "probe":
            expected = ["HAKU-BOOT-OK", "INITRAMFS-UNPACK-OK",
                        "INIT-READ-OK name=init", "USERMODE-ENTER"] + expectations
        else:
            expected = headless_expected
        missing = [m for m in expected if m not in content]
        if "EXECVE-FAIL" in content:
            missing.append("successful execve did not return an error")
        if "KERNEL PANIC" in content:
            missing.append("kernel panicked during the headless run")
        # The framebuffer console proves itself on the screen: when the
        # graphics mode is live the log's own glyphs must show up in the
        # captured pixels, in the renderer's foreground colour.
        if "FB-OK" in content:
            if not os.path.exists(ppm):
                missing.append("framebuffer active but no screendump captured")
            elif not ppm_count_color(ppm, (0xD0, 0xD0, 0xD0), 200):
                missing.append("no console text in the screendump")
        elif "FB-SKIP" not in content:
            missing.append("no framebuffer status line in the serial log")
        unexpected = [m for m in ("USER-SYSCALL-FAIL",
                                     "FS-ERROR-FAIL",
                                     "FS-EDGE-FAIL",
                                     "FS-CORRUPT-FAIL",
                                     "FS-ATA-FAIL",
                                     "FS-ROLLBACK-FAIL",
                                     "FS-ENOSPC-FAIL",
                                     "MUSL-FAIL",
                                     "ELF-LOAD-FAIL",
                                     "ELF-DISK-FAIL",
                                     "INITRAMFS-FAIL") if m in content]
        if not missing and not unexpected:
            log(("PROBE CHECK PASSED: " if mode == "probe"
                 else "HEADLESS CHECK PASSED: ") + ", ".join(expected))
            if mode == "probe":
                return
            # The guest just ran fault-injection restores and a root-fill
            # against this image: prove on the host side that mirrors,
            # chains, and allocation state are still consistent.
            log("--- post-run filesystem inspection ---")
            chk = subprocess.run(
                [py, os.path.join(ROOT, "tools", "lsfat12.py"),
                 "--check", filesystem_image],
                capture_output=True, text=True)
            print(chk.stdout, end="")
            if chk.returncode != 0:
                print(chk.stderr, end="")
                sys.exit("HEADLESS CHECK FAILED: "
                         "data image inconsistent after run")
        else:
            failures = []
            if missing:
                failures.append("missing markers: " + ", ".join(missing))
            if unexpected:
                failures.append("unexpected markers: " + ", ".join(unexpected))
            log("hint: inspect the disk with "
                "`python build.py inspect [--check] [IMAGE.img]`")
            sys.exit("HEADLESS CHECK FAILED: " + "; ".join(failures))
    elif mode != "build":
        sys.exit(f"unknown mode: {mode} "
                 f"(use: build | run [--fresh] | headless | shell | rootfs | "
                 f"probe | x64 | inspect [...]; --arch=i386 for the 32-bit "
                 f"kernel)")


if __name__ == "__main__":
    main()
