#!/usr/bin/env python3
"""MyOS v0.1 build — one script, two toolchains.

Usage:
    python build.py            build myos.img only
    python build.py run        build + boot in a QEMU window
    python build.py headless   build + boot headless, verify via serial log

Toolchains (auto-detected from sys.platform):
  Windows  MSVC cl.exe + link.exe  →  kernel.pe  →  tools/pe2bin.py
  Linux    gcc -m32 + ld           →  kernel.elf →  objcopy -O binary

On Linux you need:  gcc-multilib, nasm, qemu-system-x86, python3.
On Windows:  VS2022 "Desktop development with C++", NASM, QEMU, Python 3.
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

IS_WINDOWS = sys.platform.startswith("win")


def log(msg):
    print(msg, flush=True)


def monitor_cmd(port, cmds, timeout=5):
    import socket
    out = []
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
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


def find_tool(name, fallbacks, label):
    found = shutil.which(name)
    if found:
        return found
    for path in fallbacks:
        if os.path.isfile(path):
            return path
    sys.exit(f"Tool not found: {label}")


def run(cmd, **kw):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


# ---------------------------------------------------------------------------
# Windows: MSVC
# ---------------------------------------------------------------------------
def find_msvc():
    pattern = (r"C:\Program Files\Microsoft Visual Studio\2022"
               r"\*\VC\Tools\MSVC\*\bin\Hostx64\x86\cl.exe")
    cands = sorted(glob.glob(pattern))
    if not cands:
        sys.exit('x86 cl.exe not found: install VS2022 "Desktop development with C++"')
    return cands[-1]


def build_windows(nasm, py, img, entry_obj, isr_obj, c_objs, kernel_pe,
                  kernel_bin, stage1_bin, stage2_bin, stage1_asm, stage2_asm):
    cl = find_msvc()
    link = os.path.join(os.path.dirname(cl), "link.exe")
    if not os.path.isfile(link):
        sys.exit(f"link.exe not found next to cl.exe: {cl}")
    log(f"CL:   {cl}")

    cflags = ["/nologo", "/c", "/GS-", "/Os", "/W3"]
    for src, obj in c_objs:
        run([cl] + cflags + ["/Fo" + obj, src])
    log(f"{len(c_objs)} C objects built")

    # asm stubs (pass -DWIN32 so symbol naming gets the '_' prefix)
    run([nasm, "-f", "win32", "-DWIN32", entry_obj.replace(".obj", ".asm").replace("entry", "entry")
         if False else "", "-o", entry_obj, "dummy"]) if False else None
    run([nasm, "-f", "win32", "-DWIN32",
         os.path.join(ROOT, "kernel", "entry.asm"), "-o", entry_obj])
    run([nasm, "-f", "win32", "-DWIN32",
         os.path.join(ROOT, "kernel", "isr.asm"), "-o", isr_obj])
    log("entry.obj + isr.obj built")

    run([link, "/NOLOGO", "/SUBSYSTEM:NATIVE", "/ENTRY:entry",
         "/BASE:0x10000", "/ALIGN:16", "/FIXED", "/NODEFAULTLIB",
         "/MAP:" + os.path.join(BUILD, "kernel.map"),
         "/OUT:" + kernel_pe, entry_obj, isr_obj] + [o for _, o in c_objs])
    log("kernel.pe linked")

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

    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}",
         stage2_asm, "-o", stage2_bin])


# ---------------------------------------------------------------------------
# Linux: gcc -m32 + ld + objcopy
# ---------------------------------------------------------------------------
def _gcc_m32_works():
    try:
        subprocess.run(["gcc", "-m32", "-x", "c", "-c", "-o", os.devnull, "-"],
                       input=b"int x;", check=True, capture_output=True)
        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False


def build_linux(nasm, py, img, entry_obj, isr_obj, c_objs, kernel_pe,
                kernel_bin, stage1_bin, stage2_bin, stage1_asm, stage2_asm):
    if _gcc_m32_works():
        cc = ["gcc", "-m32"]
    else:
        gcc2 = shutil.which("i686-linux-gnu-gcc")
        if not gcc2:
            sys.exit("no 32-bit C compiler found: apt install gcc-multilib "
                     "(or i686-linux-gnu-gcc)")
        cc = [gcc2]
    log(f"CC:   {' '.join(cc)}")

    cflags = ["-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector",
              "-fno-builtin", "-mno-sse", "-mno-mmx", "-mno-80387",
              "-Wall", "-Wno-unused", "-Os", "-c"]
    for src, obj in c_objs:
        run(cc + cflags + ["-o", obj, src])
    log(f"{len(c_objs)} C objects built")

    # ELF asm (no -DWIN32: GSYM expands to bare names)
    run([nasm, "-f", "elf32", os.path.join(ROOT, "kernel", "entry.asm"),
         "-o", entry_obj])
    run([nasm, "-f", "elf32", os.path.join(ROOT, "kernel", "isr.asm"),
         "-o", isr_obj])
    log("entry.o + isr.o built")

    kernel_elf = os.path.join(BUILD, "kernel.elf")
    run(cc + ["-nostdlib", "-static", "-no-pie",
              "-T", os.path.join(ROOT, "kernel", "linker.ld"),
              "-o", kernel_elf,
              entry_obj, isr_obj] + [o for _, o in c_objs])
    log("kernel.elf linked")

    # entry address from the ELF header
    out = subprocess.check_output(["readelf", "-h", kernel_elf]).decode()
    m = re.search(r"Entry point address:\s+(0x[0-9a-fA-F]+)", out)
    if not m:
        sys.exit("readelf did not report entry point")
    entry = m.group(1)
    log(f"kernel entry: {entry}")

    # flat binary; objcopy drops .bss (entry.asm zeroes it)
    run(["objcopy", "-O", "binary", kernel_elf, kernel_bin])

    run([nasm, "-f", "bin", stage1_asm, "-o", stage1_bin])
    run([nasm, "-f", "bin", f"-DKERNEL_ENTRY={entry}",
         stage2_asm, "-o", stage2_bin])


# ---------------------------------------------------------------------------
def main():
    nasm = find_tool("nasm", [
        os.path.join(os.environ.get("LOCALAPPDATA", ""), r"bin\NASM\nasm.exe"),
        r"C:\Program Files\NASM\nasm.exe",
        "/usr/bin/nasm",
    ], "NASM  [winget install NASM.NASM | apt install nasm]")
    py = sys.executable
    if IS_WINDOWS:
        qemu = find_tool("qemu-system-i386",
                         [r"C:\Program Files\qemu\qemu-system-i386.exe"], "QEMU")
    else:
        qemu = find_tool("qemu-system-i386", ["/usr/bin/qemu-system-i386"],
                         "QEMU  [apt install qemu-system-x86]")
    log(f"NASM: {nasm}")

    kernel_dir = os.path.join(ROOT, "kernel")
    c_files = sorted(glob.glob(os.path.join(kernel_dir, "*.c")))
    if not c_files:
        sys.exit("no kernel/*.c files found")

    entry_obj = os.path.join(BUILD, "entry.o" if not IS_WINDOWS else "entry.obj")
    isr_obj   = os.path.join(BUILD, "isr.o"   if not IS_WINDOWS else "isr.obj")
    kernel_pe = os.path.join(BUILD, "kernel.pe")
    kernel_bin = os.path.join(BUILD, "kernel.bin")
    stage1_bin = os.path.join(BUILD, "stage1.bin")
    stage2_bin = os.path.join(BUILD, "stage2.bin")
    stage1_asm = os.path.join(ROOT, "boot", "stage1.asm")
    stage2_asm = os.path.join(ROOT, "boot", "stage2.asm")
    img = os.path.join(ROOT, "myos.img")

    c_objs = []
    for src in c_files:
        ext = ".obj" if IS_WINDOWS else ".o"
        obj = os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ext)
        c_objs.append((src, obj))

    if IS_WINDOWS:
        build_windows(nasm, py, img, entry_obj, isr_obj, c_objs, kernel_pe,
                      kernel_bin, stage1_bin, stage2_bin, stage1_asm, stage2_asm)
    else:
        build_linux(nasm, py, img, entry_obj, isr_obj, c_objs, kernel_pe,
                    kernel_bin, stage1_bin, stage2_bin, stage1_asm, stage2_asm)

    # floppy image (all size/signature/sector checks live in mkimg.py)
    run([py, os.path.join(ROOT, "tools", "mkimg.py"),
         stage1_bin, stage2_bin, kernel_bin, img, stage1_asm, stage2_asm])

    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    disk = [qemu, "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-boot", "order=a", "-no-reboot"]
    trace = ["-d", "int,cpu_reset", "-D", os.path.join(BUILD, "qemu.log")]

    if mode == "run":
        log("Booting MyOS in QEMU (close the window to return)...")
        subprocess.run(disk)
    elif mode == "headless":
        serial = os.path.join(BUILD, "serial.log")
        shot = os.path.join(BUILD, "screen.png")
        if os.path.exists(serial):
            os.remove(serial)
        MON_PORT = 5555
        proc = subprocess.Popen(
            disk + trace + ["-display", "none",
                            "-serial", f"file:{serial}",
                            "-monitor",
                            f"tcp:127.0.0.1:{MON_PORT},server=on,wait=off"])
        try:
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            regs = monitor_cmd(MON_PORT, ["info registers",
                                          f"screendump {shot}",
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
        expected = [
            "MYOS-BOOT-OK", "IDT-OK", "EXC-03-BP", "INT3-RESUMED",
            "PAGING-OK", "HEAP-OK", "E820-OK", "FRAME-OK",
            "PIC-OK", "PIT-OK", "KBD-OK", "T+1s", "T+2s", "T+3s",
        ]
        missing = [m for m in expected if m not in content]
        if not missing:
            log("HEADLESS CHECK PASSED: " + ", ".join(expected))
        else:
            sys.exit("HEADLESS CHECK FAILED: missing markers: "
                     + ", ".join(missing))
    elif mode != "build":
        sys.exit(f"unknown mode: {mode} (use: build | run | headless)")


if __name__ == "__main__":
    main()
