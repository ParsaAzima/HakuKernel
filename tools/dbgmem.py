#!/usr/bin/env python3
"""Debug helper: boot haku.img and dump guest memory for comparison."""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
PORT = 5599


def monitor(cmds, timeout=5):
    s = None
    for _ in range(20):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
            break
        except OSError:
            time.sleep(0.25)
    if s is None:
        return "monitor unavailable (qemu exited?)"
    s.settimeout(timeout)
    time.sleep(0.3)
    out = b""
    try:
        out += s.recv(65536)
    except socket.TimeoutError:
        pass
    for c in cmds:
        s.sendall(c.encode() + b"\n")
        time.sleep(0.4)
        try:
            out += s.recv(65536)
        except socket.TimeoutError:
            pass
    s.close()
    return out.decode("utf-8", "replace")


def main():
    img = os.path.join(ROOT, "haku.img")
    disk = [r"C:\Program Files\qemu\qemu-system-i386.exe",
            "-drive", f"file={img},format=raw,if=floppy,index=0",
            "-drive", f"file={os.path.join(BUILD, 'haku_fs.img')},format=raw,if=ide,index=0,media=disk",
            "-boot", "order=a", "-no-reboot", "-display", "none",
            "-serial", f"file:{os.path.join(BUILD, 'dbg-serial.log')}",
            "-monitor", f"tcp:127.0.0.1:{PORT},server=on,wait=off"]
    proc = subprocess.Popen(disk)
    time.sleep(2.0)
    try:
        print(monitor(sys.argv[1:] or ["xp /16wx 0x10220"]))
    finally:
        proc.kill()
        proc.wait()


if __name__ == "__main__":
    main()
