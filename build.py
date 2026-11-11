#!/usr/bin/env python3
"""Haku build — one straightforward script, no shell-quoting games.

Usage:
    python build.py                 build haku.img only (data image preserved)
    python build.py run             build + boot in a QEMU window
    python build.py run --fresh     recreate the FAT12 data image, then boot
    python build.py headless        fresh data image + boot headless, verify
    python build.py x64             build haku64.img (64-bit kernel) and boot it
    python build.py --memory=MB     guest RAM (default 512, max 1020)
    python build.py inspect ...     examine a FAT12 image (see --help there)

Pipeline:
    arch/x86/user/{user,child,fault,exec_target}.asm --nasm--> ELF files
        --mkfat12--> haku_fs.img (fresh only)
    kernel/**/*.c --cl--> kernel objects
    + arch/x86/kernel/{entry,isr}.asm --nasm--> entry objects
    kernel objects + entry objects --link--> kernel.pe --pe2bin--> kernel.bin
    arch/x86/boot/stage1.asm --nasm--> stage1.bin (512 B)
    arch/x86/boot/stage2.asm --nasm (kernel entry + size from PE)--> stage2.bin
    stage1 + stage2 + kernel.bin --mkimg--> haku.img (boot floppy)
    QEMU boots haku.img and attaches haku_fs.img as primary-master ATA disk,
    plus a generated spare disk on the secondary channel for device scanning

`python build.py x64` is a separate pipeline: entry64.asm, isr64.asm and the
kernel/arch/x86_64 C sources are compiled for x86-64, linked as a PE32+
image, and booted from the same stage1/stage2 floppy as haku64.img. It also
builds a fresh FAT12 seed disk (build/haku64_fs.img) with the shared ATA
driver and filesystem sources, attaches it as the primary-master IDE disk,
and verifies the volume after QEMU exits. It shares nothing with the i386
link line, so neither target can break the other.

The data image (build/haku_fs.img) is user data: an existing image is
reused as-is so files from previous QEMU sessions survive the build, and
it is recreated only when missing or with --fresh. headless always starts
from a fresh image — its fault-injection tests need pristine state — and
verifies the disk again after QEMU exits (tools/lsfat12.py --check).

All tools are invoked with argument lists (never through cmd.exe), so paths
with spaces just work. The C compiler and linker are the only host-specific
piece: Windows uses VS2022's 32-bit cl.exe + link.exe, while other platforms
use clang-cl + lld-link. Both accept the identical MSVC-style switches
(/c, /Fo, /ENTRY, /ALIGN, /NODEFAULTLIB ...) and both emit 32-bit COFF/PE, so
only tool discovery differs.
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


def find_msvc():
    """Find an x86-hosted cl.exe from any VS 2022+ installation.

    CI images may expose cl through PATH (developer prompt) or only through
    the Visual Studio directories, so check both. Prefer an x86 target
    toolset: an x64 cl.exe cannot produce the 32-bit COFF objects we link.
    """
    found = shutil.which("cl")
    if found and re.search(r"[\\/]Hostx(?:64|86)[\\/]x86[\\/]cl\.exe$",
                           found, re.IGNORECASE):
        return found
    patterns = [
        r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC"
        r"\*\bin\Hostx64\x86\cl.exe",
        r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC"
        r"\*\bin\Hostx64\x86\cl.exe",
    ]
    cands = []
    for pattern in patterns:
        cands += glob.glob(pattern)

    def version(path):
        match = re.search(r"[\\/]MSVC[\\/]([\d.]+)[\\/]bin[\\/]", path)
        return tuple(int(part) for part in match.group(1).split(".")) \
            if match else ()

    if not cands:
        sys.exit('x86 cl.exe not found: install VS2022 '
                 '"Desktop development with C++"')
    return max(cands, key=version)


def _latest(paths):
    """Pick the highest-numbered tool; distros ship clang-cl-19, lld-link-19."""
    def version(path):
        nums = re.findall(r"\d+", os.path.basename(path))
        return int(nums[-1]) if nums else -1
    return max(paths, key=version)


def find_clang_cl():
    """Locate a cl-driver clang: clang-cl, a versioned one, or clang in cl mode."""
    direct = shutil.which("clang-cl")
    if direct:
        return [direct]
    cands = []
    for d in ("/usr/lib/llvm-*/bin", "/usr/bin", "/usr/local/bin"):
        cands += glob.glob(os.path.join(d, "clang-cl-*"))
        cands += glob.glob(os.path.join(d, "clang-cl"))
    if cands:
        return [_latest(cands)]
    clang = shutil.which("clang")
    if clang:
        return [clang, "--driver-mode=cl"]
    sys.exit("clang-cl not found: install Clang (e.g. apt install clang lld)")


def find_lld_link():
    """Locate lld-link (or a versioned lld-link-19)."""
    direct = shutil.which("lld-link")
    if direct:
        return [direct]
    cands = []
    for d in ("/usr/lib/llvm-*/bin", "/usr/bin", "/usr/local/bin"):
        cands += glob.glob(os.path.join(d, "lld-link-*"))
        cands += glob.glob(os.path.join(d, "lld-link"))
    if not cands:
        sys.exit("lld-link not found: install LLVM lld (e.g. apt install lld)")
    return [_latest(cands)]


def detect_toolchain():
    """Return (cc, linker, label): a compiler+linker that share MSVC switches.

    Windows keeps VS2022's 32-bit cl.exe + link.exe. Everywhere else MSVC is
    unavailable, so clang-cl + lld-link drive the same /c, /Fo, /ENTRY, /ALIGN
    command lines targeting i686 COFF, keeping NASM win32 objects, the linked
    PE, and pe2bin's output identical to a Windows build.
    """
    if os.name == "nt":
        cl = find_msvc()
        link = os.path.join(os.path.dirname(cl), "link.exe")
        if not os.path.isfile(link):
            sys.exit(f"link.exe not found next to cl.exe: {cl}")
        return [cl], [link], "MSVC cl.exe/link.exe"
    cc = find_clang_cl() + ["--target=i686-pc-windows-msvc"]
    return cc, find_lld_link(), "clang-cl/lld-link (i686-pc-windows-msvc)"


def find_x64_toolchain():
    """Return (cc, linker, label) for x86-64, or exit with a clear reason.

    Windows keeps the x64 tools next to the 32-bit ones
    (bin\\Hostx64\\x64), so the same VS install covers both. Elsewhere
    clang-cl is retargeted at x86_64-pc-windows-msvc, mirroring the i686
    path, so NASM win64 objects, the PE32+ image, and pe2bin line up.
    """
    if os.name == "nt":
        cl = find_msvc()                       # ...\bin\Hostx64\x86\cl.exe
        x64 = os.path.join(os.path.dirname(os.path.dirname(cl)), "x64")
        cl64 = os.path.join(x64, "cl.exe")
        link64 = os.path.join(x64, "link.exe")
        if not (os.path.isfile(cl64) and os.path.isfile(link64)):
            sys.exit("x64 MSVC tools not found; install "
                     "'Desktop development with C++' for x64 in the "
                     "Visual Studio Installer")
        return [cl64], [link64], "MSVC cl.exe/link.exe (x64)"
    cc = find_clang_cl() + ["--target=x86_64-pc-windows-msvc"]
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


def build_x64_fs(py, image, exec_elf):
    """Seed the x86-64 data disk: three files the kernel reads at boot.

    HELLO.TXT carries the 16-byte marker the ELF64 self-test prints,
    PATTERN.BIN is three pages so a file mapping and a multi-page read both
    have data on every page, and USER.ELF is the program the test execs from
    the disk (the kernel image does not contain it, so running it proves the
    exec path). The pattern is the same (offset*37+11) & 0xFF formula
    boot64.c uses to verify it. The image is rebuilt on every x64 run because
    the kernel also creates /TMP and writes NOTE.TXT into it.
    """
    hello = b"HAKU64-FILE-OK\r\n"
    pattern = bytes(((i * 37 + 11) & 0xFF) for i in range(3 * 4096))
    hello_path = os.path.join(BUILD, "haku64_hello.txt")
    pattern_path = os.path.join(BUILD, "haku64_pattern.bin")
    with open(hello_path, "wb") as f:
        f.write(hello)
    with open(pattern_path, "wb") as f:
        f.write(pattern)

    run([py, os.path.join(ROOT, "tools", "mkfat12.py"), "--empty", image,
         f"HELLO.TXT={hello_path}", f"PATTERN.BIN={pattern_path}",
         f"USER.ELF={exec_elf}"])
    log(f"x64 FAT12 seed disk ready ({os.path.getsize(image)} bytes)")


def build_x64(nasm, py, memory_mb):
    """Build and boot haku64.img: the same floppy, with a 64-bit kernel.

    The entry path, memory subsystem, interrupts, per-process address
    spaces, the scheduler, ring-3 entry, the six-argument syscall ABI, the
    ELF64 loader and a FAT12 disk (the same ATA driver and filesystem the
    32-bit kernel uses, behind a small compatibility layer) are all 64-bit.
    The 32-bit kernel is untouched and remains the default image.
    """
    cc, linker, label = find_x64_toolchain()
    log(f"x64 toolchain: {label}")

    arch32 = os.path.join(ROOT, "kernel", "arch", "x86")
    arch64 = os.path.join(ROOT, "kernel", "arch", "x86_64")
    entry_asm = os.path.join(arch64, "boot", "entry64.asm")
    isr_asm = os.path.join(arch64, "kernel", "isr64.asm")
    user_src = os.path.join(arch64, "user", "ring3_elf64.asm")
    user_image_asm = os.path.join(arch64, "user", "user64_image.asm")
    stage1_asm = os.path.join(arch32, "boot", "stage1.asm")
    stage2_asm = os.path.join(arch32, "boot", "stage2.asm")

    entry_obj = os.path.join(BUILD, "entry64.obj")
    isr_obj = os.path.join(BUILD, "isr64.obj")
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
                    os.path.join(ROOT, "kernel", "drivers"),
                    os.path.join(ROOT, "kernel", "fs", "fat12")]
    sources = sorted(glob.glob(os.path.join(arch64, "**", "*.c"),
                               recursive=True))
    sources += [os.path.join(ROOT, "kernel", "drivers", "ata.c"),
                os.path.join(ROOT, "kernel", "fs", "fat12", "fat12.c")]
    if not sources:
        sys.exit("no kernel/arch/x86_64/**/*.c files found")
    run([nasm, "-f", "win64", entry_asm, "-o", entry_obj])
    run([nasm, "-f", "win64", isr_asm, "-o", isr_obj])
    objects = []
    for src in sources:
        obj = os.path.join(BUILD, "x64_" +
                           os.path.splitext(os.path.basename(src))[0] + ".obj")
        run(cc + ["/nologo", "/c", "/GS-", "/Zl", "/Os", "/W3"]
            + ["/I" + d for d in include_dirs]
            + ["/Fo" + obj, src])
        objects.append(obj)
    log(f"{len(objects)} x64 C objects built")

    # 2. the ring-3 test program: flat binary -> static ELF64 -> embedded
    #    .rdata object. mkelf64.py also refuses a program too large for the
    #    one-page code segment the layout promises.
    run([nasm, "-I" + os.path.join(ROOT, "include"), "-f", "bin",
         f"-DUSER64_BASE={user_base:#x}", user_src, "-o", user_flat])
    run([py, os.path.join(ROOT, "tools", "mkelf64.py"),
         user_flat, user_elf, f"--base={user_base:#x}"])
    embedded = user_elf.replace("/", "\\")
    run([nasm, "-f", "win64", f"-DUSER64_IMAGE={embedded}",
         user_image_asm, "-o", user_image_obj])

    # 2b. the program stored on the disk and run by execve(). Same layout,
    #     same base: it never coexists with the image above because it runs
    #     in a space of its own.
    run([nasm, "-I" + os.path.join(ROOT, "include"), "-f", "bin",
         exec_src, "-o", exec_flat])
    run([py, os.path.join(ROOT, "tools", "mkelf64.py"),
         exec_flat, exec_elf, f"--base={user_base:#x}"])

    run(linker + ["/nologo", "/MACHINE:X64", "/SUBSYSTEM:NATIVE",
                  "/ENTRY:entry64", "/BASE:0x10000", "/ALIGN:16", "/FIXED",
                  "/NODEFAULTLIB", "/OUT:" + kernel_pe,
                  entry_obj, isr_obj, user_image_obj] + objects)
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
    build_x64_fs(py, fs_img, exec_elf)

    # 5. boot it. qemu-system-i386 refuses long mode without a 64-bit CPU
    #    model, so this mode uses the x86_64 binary. writethrough keeps the
    #    guest's disk writes in the host file when QEMU is killed.
    qemu = find_tool("qemu-system-x86_64",
                     [r"C:\Program Files\qemu\qemu-system-x86_64.exe"], "QEMU")
    if os.path.exists(serial):
        os.remove(serial)
    proc = subprocess.Popen([qemu, "-m", str(memory_mb), "-cpu", "qemu64",
                             "-drive", f"file={img},format=raw,if=floppy,index=0",
                             "-drive", f"file={fs_img},format=raw,if=ide,"
                                       "index=0,media=disk,cache=writethrough",
                             "-boot", "order=a", "-no-reboot",
                             "-display", "none", "-serial", f"file:{serial}"])
    reached = wait_for_serial(serial, "HAKU64-SELFTEST-OK", 30.0)
    proc.kill()
    proc.wait()
    with open(serial, errors="replace") as f:
        content = f.read()
    log("--- serial log (x64) ---")
    log(content)
    required = ["HAKU64-BOOT-OK", "HAKU64-E820-OK", "HAKU64-PAGING-OK",
                "HAKU64-FRAME-OK", "HAKU64-HEAP-OK", "HAKU64-GDT-OK",
                "HAKU64-IDT-OK", "HAKU64-EXC-OK", "HAKU64-TIMER-OK",
                "HAKU64-VM-OK", "HAKU64-PROC-OK", "HAKU64-USER-OK",
                "HAKU64-SYSCALL-OK", "HAKU64-ELF-OK", "HAKU64-ELF-RUN-OK",
                "HAKU64-ELF-REJECT-OK", "HAKU64-ABI-OK", "HAKU64-NX-OK",
                "HAKU64-RTC-OK", "HAKU64-SYSCALL64-OK", "HAKU64-FS-OK",
                "HAKU64-FILE-OK", "HAKU64-DISK-OK", "HAKU64-FORK-OK",
                "HAKU64-DUP-OK", "HAKU64-EXEC-OK", "HAKU64-SELFTEST-OK"]
    missing = [marker for marker in required if marker not in content]
    if "HAKU64-FAIL" in content:
        for line in content.splitlines():
            if "HAKU64-FAIL" in line:
                sys.exit(f"X64 CHECK FAILED: {line.strip()}")
    if missing or not reached:
        sys.exit("X64 CHECK FAILED: missing markers: " + ", ".join(missing))

    # The run created /TMP/NOTE.TXT and flushed it through the ATA cache, so
    # a layout check of the host-side image proves the write reached the
    # volume, not just the kernel's page cache.
    check = subprocess.run([py, os.path.join(ROOT, "tools", "lsfat12.py"),
                            "--check", fs_img], capture_output=True, text=True)
    print(check.stdout, end="")
    if check.returncode != 0:
        print(check.stderr, end="")
        sys.exit("X64 CHECK FAILED: the disk image is not valid after the run")
    log("X64 CHECK PASSED: " + ", ".join(required))


def run(cmd):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def ppm_has_color(path, rgb):
    """True when a binary PPM (P6) contains the given RGB triple."""
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
    for offset in range(0, min(len(pixels), width * height * 3) - 2, 3):
        if (pixels[offset] == red and pixels[offset + 1] == green
                and pixels[offset + 2] == blue):
            return True
    return False


def main():
    nasm = find_tool("nasm", [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), r"bin\NASM\nasm.exe"),
        r"C:\Program Files\NASM\nasm.exe",
    ], "NASM  [winget install NASM.NASM]")
    cc, linker, toolchain = detect_toolchain()
    py = sys.executable
    qemu = find_tool("qemu-system-i386",
                     [r"C:\Program Files\qemu\qemu-system-i386.exe"], "QEMU")
    log(f"NASM: {nasm}")
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

    raw_args = sys.argv[1:]
    positionals = [a for a in raw_args if not a.startswith("-")]
    mode = positionals[0] if positionals else "build"

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
    busybox_path = None
    shell_command = None
    shell_check = False
    for flag in raw_args:
        if flag == "--fresh":
            continue
        if flag == "--check":
            shell_check = True
            continue
        if flag.startswith("--busybox="):
            busybox_path = flag.split("=", 1)[1]
            continue
        if flag.startswith("--cmd="):
            shell_command = flag.split("=", 1)[1]
            continue
        if flag.startswith("--init="):
            init_override = flag.split("=", 1)[1]
            continue
        if flag.startswith("--expect="):
            expectations.append(flag.split("=", 1)[1])
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
        if flag.startswith("-"):
            sys.exit(f"unknown flag: {flag} (use --fresh, --memory=MB, "
                     "--init=PATH, --expect=MARKER, --busybox=PATH, "
                     "--cmd=TEXT, --check)")
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
        if shell_command is None:
            shell_command = (SHELL_CHECK_SCRIPT if shell_check
                             else SHELL_INTERACTIVE_SCRIPT)
        elif init_override or expectations:
            sys.exit("--init and --expect do not apply to `shell`")
        if shell_check and init_override:
            sys.exit("--init does not apply to `shell`")
    elif init_override or expectations:
        sys.exit("--init and --expect only apply to `probe`")
    if shell_check and mode != "shell":
        sys.exit("--check only applies to `shell`")

    if mode == "x64":
        build_x64(nasm, py, memory_mb)
        return

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
    if not c_files:
        sys.exit("no kernel/**/*.c files found")
    entry_asm = os.path.join(arch_dir, "kernel", "entry.asm")
    isr_asm = os.path.join(arch_dir, "kernel", "isr.asm")
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
    ]
    fat12_script = os.path.join(ROOT, "tools", "mkfat12.py")
    cpio_script = os.path.join(ROOT, "tools", "mkcpio.py")
    entry_obj = os.path.join(BUILD, "entry.obj")
    isr_obj = os.path.join(BUILD, "isr.obj")
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

    # The shell target boots a different data disk: busybox plus, for the
    # scripted check, an INIT.CMD. Keeping it separate means the test image
    # and the shell image never overwrite each other.
    if mode == "shell":
        filesystem_image = build_shell_image(py, user_elf,
                                             shell_busybox(busybox_path),
                                             shell_command)

    # 1. User ELFs are always rebuilt (build artifacts), but the FAT12 data
    #    disk is user data: an existing image is reused untouched so files
    #    from previous QEMU sessions survive, and mkfat12 runs only for an
    #    initial image or an explicit --fresh. headless always recreates —
    #    its fault-injection tests require pristine, deterministic state.
    nasm_include = include_dir + os.sep
    run([nasm, "-I" + nasm_include, "-f", "bin", user_asm, "-o", user_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", exec_target_asm,
         "-o", exec_target_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", child_asm, "-o", child_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", vfork_asm, "-o", vfork_elf])
    run([nasm, "-I" + nasm_include, "-f", "bin", fault_asm, "-o", fault_elf])

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
    user_objects.append(user_test_obj)
    run(linker + ["/nologo", "/MACHINE:X86", "/SUBSYSTEM:CONSOLE",
                  "/ENTRY:start", "/BASE:0x40000000", "/FIXED",
                  "/DYNAMICBASE:NO", "/NXCOMPAT:NO", "/SAFESEH:NO",
                  "/ALIGN:4096", "/FILEALIGN:512", "/NODEFAULTLIB",
                  "/OUT:" + libtest_pe] + user_objects)
    run([py, pe2elf_script, libtest_pe, libtest_elf])

    # The initramfs holds the userland image the kernel unpacks into its RAM
    # filesystem at boot: /init is the first program, and the helper
    # programs stay reachable under their old names so the existing tests
    # exercise the RAM filesystem instead of the data disk.
    initramfs_image = os.path.join(BUILD, "initrd.cpio")
    run([py, cpio_script, initramfs_image,
         f"init={init_override or user_elf}", f"HAKU.ELF={user_elf}",
         f"NEXT.ELF={exec_target_elf}", f"CHILD.ELF={child_elf}",
         f"VFORK.ELF={vfork_elf}", f"FAULT.ELF={fault_elf}",
         f"LIB.ELF={libtest_elf}", "--dir=dev", "--dir=proc",
         "--dir=disk"])

    if mode in ("headless", "probe") or fresh or \
            not os.path.isfile(filesystem_image):
        if mode in ("headless", "probe") and os.path.isfile(filesystem_image):
            log(f"{mode}: recreating the data image "
                "(the run mutates the disk)")
        run([py, fat12_script, user_elf, filesystem_image,
             f"NEXT.ELF={exec_target_elf}", f"CHILD.ELF={child_elf}",
             f"FAULT.ELF={fault_elf}", f"LIB.ELF={libtest_elf}",
             f"VFORK.ELF={vfork_elf}", f"INITRD={initramfs_image}"])
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
    log("entry.obj + isr.obj built")

    # 4. link a fixed-base PE (/ENTRY:entry — link prepends _ for x86;
    #    /ALIGN:16 packs sections; /FIXED drops .reloc — we always load
    #    at the linked base, so runtime relocation is meaningless)
    run(linker + ["/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/FIXED", "/NODEFAULTLIB",
         "/MAP:" + os.path.join(BUILD, "kernel.map"),
         "/OUT:" + kernel_pe, entry_obj, isr_obj] + c_objs)
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
            "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-drive", f"file={filesystem_image},format=raw,if=ide,index=0,media=disk",
            "-drive", f"file={spare_image},format=raw,if=ide,index=2,media=disk",
            "-boot", "order=a", "-no-reboot"]
    # CPU exception trace: shows every #GP/#PF/triple-fault with EIP.
    trace = ["-d", "int,cpu_reset", "-D", os.path.join(BUILD, "qemu.log")]
    if mode == "run":
        log("Booting Haku in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode == "shell" and not shell_check:
        log("Booting the Haku shell in QEMU (close the window to return)...")
        log("busybox ash is pid 1: type a command at the prompt")
        subprocess.run(disk)
    elif mode == "shell":
        serial = os.path.join(BUILD, "serial.log")
        if os.path.exists(serial):
            os.remove(serial)
        proc = subprocess.Popen(disk + ["-display", "none",
                                        "-serial", f"file:{serial}"])
        reached = wait_for_serial(serial, "SHELL-DONE",
                                  float(os.environ.get("HAKU_SHELL_TIMEOUT",
                                                       "300")))
        proc.kill()
        proc.wait()
        with open(serial, errors="replace") as f:
            content = f.read()
        log("--- serial log (shell) ---")
        log(content)
        missing = [marker for marker in SHELL_EXPECTED if marker not in content]
        # The applet count is the one thing a marker cannot express: it proves
        # that /bin was filled and that a directory listing came back through
        # getdents, so a boot that reaches the prompt but cannot run anything
        # still fails here.
        applets = re.search(r"APPS:(\d+)", content)
        if not applets or int(applets.group(1)) < 100:
            missing.append("APPS:<count >= 100>")
        if "PANIC" in content or "FAIL" in content:
            for line in content.splitlines():
                if "PANIC" in line or "FAIL" in line:
                    log("  " + line.strip())
                    missing.append("no PANIC/FAIL")
        if "can't access tty" in content or "job control turned off" in content:
            missing.append("no tty warning")
        if "can't set tty process group" in content:
            missing.append("no process-group error")
        if missing or not reached:
            sys.exit("SHELL CHECK FAILED: missing " + ", ".join(missing))
        log(f"SHELL CHECK PASSED: busybox ash ran as pid 1, "
            f"{applets.group(1)} applets resolved through $PATH, "
            f"uname and /proc answered, job control enabled, "
            f"a foreground pipeline forked and ran")
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
            "USER-FS-ABI-OK", # openat/getdents64/chdir/pipe/poll/fcntl/termios
            "USER-ERRNO-OK", # error matrix: bad fd/pointer/path/argument
            "USER-INITRAMFS-OK", # ramfs root, long names, symlinks, devfs, procfs
            "SCHED-CONCURRENT-OK", # two tasks doing disk I/O at once
            "USER-LIB-OK",   # C library syscalls/strings/malloc/file I/O
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
        # graphics mode is live the signature block the driver draws must
        # show up in the captured framebuffer.
        if "FB-OK" in content:
            if not os.path.exists(ppm):
                missing.append("framebuffer active but no screendump captured")
            elif not ppm_has_color(ppm, (0x20, 0xA0, 0xFF)):
                missing.append("framebuffer signature color missing from the"
                               " screendump")
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
                 f"(use: build | run [--fresh] | headless | inspect [...])")


if __name__ == "__main__":
    main()
