# Storage and filesystems

[Back to documentation index](README.md)

ATA PIO, FAT12, the VFS layer, the boot namespace, and why the filesystem
is deliberately small.

## ATA PIO and FAT12

`kernel/drivers/ata.c` probes the legacy primary and secondary ATA channels
(`0x1F0`/`0x3F6` and `0x170`/`0x376`) at both master and slave positions,
ignores floating buses and ATAPI devices, and keeps every usable disk in a
device list. Devices are identified with IDENTIFY; LBA28 is used when
available and LBA48 addressing is the fallback. Identify, read, write, and
cache-flush commands use bounded polling PIO: a failed or timed-out command
is retried after a software reset (SRST) up to a fixed number of attempts,
and every write is followed by FLUSH CACHE before it is reported complete
(a drive that rejects the flush keeps the accepted data and only logs a
warning). PIO polling is retained deliberately: the initial FAT mount runs
before the PIC and PIT are initialized, while IRQ completion requires
remapping/unmasking IRQ14/15 and DMA completion adds PCI bus-master
programming and physical-buffer constraints. The synchronous path keeps boot
deterministic and headless-testable; AHCI, PCI native-mode controllers, and
ATAPI are not supported. The boot self-tests prove the recovery path
(`ATA-RECOVERY-OK`: a software reset followed by an identical sector reread)
and cross-channel I/O (`ATA-MULTI-OK`: the second device answers a sector
read), and the headless run checks the scan itself (`ATA-SCAN-OK devices=2`).
`kernel/fs/fat12/fat12.c` mounts a
FAT12 superfloppy from any detected ATA disk (the first disk that carries a
valid BPB wins, so the volume may live on the secondary channel or on a
slave) and supports creating, reading, writing, truncating and
seeking 8.3-named files anywhere in the tree, plus `mkdir`/`rmdir` of
single-cluster subdirectories with real `.`/`..` entries. Paths are
`/`-separated and resolved from the root; `.` and `..` are handled during
resolution. Cluster allocation, release, mirrored FAT updates and
volume/chain bounds checks are shared by files and directories.
`kernel/fs/vfs/vfs.c` supplies per-process descriptors and the user `open`, `close`,
`read`, `write`, and `lseek` interface through a filesystem-independent
operations vector (`vfs_fs_ops`: mount/open/close/read/write/seek/unlink/
rename/mkdir/rmdir/stat/lstat/fstat, each returning 1 or a `-VFS_E*` errno).
VFS never names a concrete filesystem; each backend registers the vector and
the VFS binds it to a directory path in a mount table. Path resolution picks
the longest matching prefix, so `/disk/HAKU.ELF` reaches the FAT12 backend as
`HAKU.ELF` while `/dev/null` reaches the device backend as `null`; the mount
root itself is `"."`, the same token the syscall layer hands the VFS after
canonicalizing `/`. A mount is read-only when it is created with
`VFS_MOUNT_RDONLY`, and VFS then returns `EROFS` for every mutating operation
instead of calling the backend, including writes through a descriptor that
was opened before the remount. A mount point cannot be unlinked, renamed, or
removed (`EBUSY`), a `rename` that would cross two mounts reports `EXDEV`, and
a mount point directory must exist in the parent filesystem so it shows up in
a listing. Disk I/O is serialized
against the preemptive scheduler; file contents and directory/FAT metadata
are written through to the ATA disk. The current VFS supports up to five
simultaneously open regular files per process (descriptors 3-7; `0`/`1`/`2`
are the console) and the `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`,
`O_EXCL`, `O_TRUNC`, and `O_APPEND` flags.

FAT12 remains intentionally small as a *data-disk* format: no partition
tables, long filenames, permissions, journaling, FAT16, or FAT32 (see the
next section for why). Directories are one cluster (16 entries) and do not
grow; moving a directory
across parents with `rename` leaves its `..` entry pointing at the old
parent, so directories should be renamed in place. The userland root is the
RAM filesystem described below, which has none of those limits. The QEMU
headless test creates `P1.TXT`, exercises `DIR1/T.TXT` with `./DIR1/../DIR1`
paths, and verifies multi-sector writes, reads, seeks, close, and reopen.

### Why FAT12 with 8.3 names, and not something bigger

Long filenames, partition tables, and FAT16/FAT32 are each a deliberate
non-goal for the data disk, not an oversight. The reasoning, recorded so the
decision does not have to be re-derived:

- **Long filenames (VFAT LFN).** LFN entries are a chain of hidden records
  with a checksum tying them to the 8.3 name, ordered in reverse and
  contiguous; every create, rename, unlink, and directory-growth path would
  have to maintain that chain, and a torn write leaves an orphaned fragment.
  Haku's userland root is moving to an in-memory filesystem with real long
  names and permission bits (see [the roadmap](../ROADMAP.md) milestone 7), which leaves FAT12
  as an interchange format for small data disks where 8.3 names are enough.
  Paying LFN complexity in the interchange format buys nothing the userland
  filesystem does not already provide.
- **Partition tables.** The kernel owns the whole disk image: the FAT12 volume
  starts at LBA 0. Adding an MBR would mean parsing a partition table, keeping
  a per-partition byte offset through every cluster calculation, and choosing a
  boot partition — for a disk no other operating system shares. If Haku ever
  has to coexist with another OS on one physical disk, this becomes worth
  doing, and the offset layer is the natural place to add it.
- **FAT16/FAT32.** The current images are 1.44 MB, where FAT12 is the correct
  encoding (a 12-bit entry per cluster). FAT16 becomes relevant only for data
  disks above roughly 16 MiB at 4 KiB clusters, and FAT32 brings long names,
  FSInfo, and mirroring rules with it. Both would be new backends with their
  own on-disk structures rather than a flag on the existing one.

Any of these can be revisited, but only together with interoperability tests:
before such a change lands, `mtools`/`fsck.fat` must be able to read, write,
and check the resulting image in CI, and `tools/lsfat12.py` must agree with
them. A filesystem that only Haku can read is not a filesystem Haku can
export data through, and the roadmap treats that gate as mandatory rather than
optional.

`tools/mkfat12.py` puts `HAKU.ELF`, the exec target `NEXT.ELF`, and the
spawn/wait child `CHILD.ELF` on a generated 1.44 MB FAT12 image, together with
the initramfs archive `INITRD` (no extension, because the volume only stores
8.3 names). QEMU
attaches this image as the primary-master IDE disk while the existing boot
floppy still starts the kernel; the build also attaches a generated
1.44 MB spare disk as secondary master so the headless run exercises
device scanning across both legacy channels. The kernel
loads the initial ELF at startup; `execve` uses VFS to load subsequent ELF
files into a fresh address space before replacing the current image.

### The boot namespace: initramfs, ramfs, `/dev`, and `/proc`

The root of the userland namespace is the RAM filesystem, not the FAT12
volume. At boot the kernel mounts `ramfs` on `/`, creates the `/disk` mount
point directory, mounts the FAT12 data disk there, reads `disk/INITRD`, and
unpacks the cpio archive into the RAM root. `/init` from that archive runs as
PID 1; the FAT volume stays reachable at `/disk`, and an image without an
archive still boots the old way with FAT12 as the root.

- **`ramfs`** (`kernel/fs/ramfs`) is the writable root: long names (48 bytes
  per component), permission bits, timestamps from the PIT, nested
  directories, and symbolic links with a hop limit of 8. Nodes come from a
  fixed 512-entry table and file data from the kernel heap (256 KiB per
  file, which is why the busybox shell loads its 1 MiB binary from the FAT12
  disk and not from an initramfs),
  so nothing survives a reboot — which is exactly what an initramfs needs.
  Unlinking a file that is still open keeps the data alive until the last
  descriptor closes, the same promise FAT12 makes.
- **`devfs`** (`kernel/fs/devfs`) is a fixed table of `/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/random`, `/dev/console`, and `/dev/tty`. These are
  ordinary descriptors on the normal read/write path; `seek` reports `ESPIPE`
  and the entries cannot be unlinked or renamed.
- **`procfs`** (`kernel/fs/procfs`) generates `/proc/version`, `/proc/uptime`,
  and `/proc/meminfo` when the file is opened, so a `cat` sees current values.
  It is read-only: writes report `EACCES`, and the entries cannot be removed.

The archive is cpio "newc" (`070701`), built by `tools/mkcpio.py` from the
same ELF images the FAT volume carries. It holds `/init`, the programs the
exec tests use, and empty `dev`, `proc`, and `disk` directories so the mount
points appear in a listing. `INITRAMFS-UNPACK-OK entries=… files=… bytes=…`
reports what was unpacked, and the ring-3 self-test proves the rest from user
mode (`USER-INITRAMFS-OK`): the root listing, a name longer than 8.3, a file
two directories deep, a symlink that `lstat` reports as a link while `stat`
and `open` follow it, `/dev/null` returning end of file, `/dev/zero`
returning zeros, `/proc/version` naming the kernel, and `/disk/HAKU.ELF`
starting with the ELF magic.

`lstat` is a real no-follow call: the backend's optional `lstat` operation
describes the last component itself (ramfs walks with `follow_last = 0`),
`AT_SYMLINK_NOFOLLOW` is honored by `fstatat64` and `statx`, and a backend
without links answers exactly like `stat`. `symlink` and `readlink` are
available to user mode; `readlink` truncates to the caller's buffer instead
of failing, like Linux.
