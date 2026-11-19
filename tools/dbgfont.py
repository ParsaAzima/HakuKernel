#!/usr/bin/env python3
"""Debug helper: dump guest physical memory and look for the VGA font."""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
PORT = 5601


def monitor(cmds, timeout=6):
    s = None
    for _ in range(30):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
            break
        except OSError:
            time.sleep(0.25)
    if s is None:
        return ""
    s.settimeout(timeout)
    time.sleep(0.3)
    try:
        s.recv(65536)
    except socket.TimeoutError:
        pass
    for c in cmds:
        s.sendall(c.encode() + b"\n")
        time.sleep(0.6)
        try:
            s.recv(65536)
        except socket.TimeoutError:
            pass
    s.close()
    return ""


def main():
    img = os.path.join(ROOT, "haku.img")
    out = os.path.join(BUILD, "rom.bin")
    disk = [r"C:\Program Files\qemu\qemu-system-i386.exe",
            "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-boot", "order=a", "-no-reboot", "-display", "none",
            "-serial", f"file:{os.path.join(BUILD, 'dbg-serial.log')}",
            "-monitor", f"tcp:127.0.0.1:{PORT},server=on,wait=off"]
    proc = subprocess.Popen(disk)
    time.sleep(2.0)
    try:
        monitor([f"pmemsave 0xC0000 0x20000 {out}"])
    finally:
        time.sleep(1.0)
        proc.kill()
        proc.wait()
    if not os.path.exists(out):
        print("no dump")
        return
    data = open(out, "rb").read()
    print(f"dumped {len(data)} bytes")
    # the classic 8x16 'A' glyph
    pattern = bytes([0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
                     0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00])
    idx = data.find(pattern)
    print("classic 8x16 'A' at:", hex(0xC0000 + idx) if idx >= 0 else "not found")
    # a blank space glyph plus non-blank neighbours
    hits = []
    for off in range(0, len(data) - 0x1000):
        if data[off + 0x20 * 16:off + 0x21 * 16] == b"\x00" * 16:
            a = data[off + 0x41 * 16:off + 0x42 * 16]
            z = data[off + 0x30 * 16:off + 0x31 * 16]
            if any(a) and any(z):
                hits.append(off)
    print("blank-space candidates:", len(hits))
    for off in hits[:6]:
        print(f"  0x{0xC0000 + off:05X} 'A'={data[off+0x41*16:off+0x41*16+8].hex()}")


if __name__ == "__main__":
    main()
