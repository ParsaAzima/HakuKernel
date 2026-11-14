# Project layout

[Back to documentation index](README.md)

Where the sources, build outputs, and generated artifacts live.

```
Haku/
  kernel/
    arch/x86/
      boot/             stage1.asm + stage2.asm: 16-bit boot sector and CHS
                        kernel loader (up to 448 KiB) → A20 → GDT → protected mode
      kernel/           entry.asm + isr.asm: 32-bit entry, interrupt stubs,
                        GDT/TSS load, ring-3 transition + syscalls
                        cpu/gdt/idt/pic/pit/kbd: port I/O, descriptor tables,
                        PIC/PIT, and the PS/2 keyboard driver
                        smp.c: ACPI MADT, local APIC, AP bring-up, LAPIC
                        timer calibration and TLB shootdown
                        percpu.c: one GS-addressed block per CPU
      mm/               paging.c/.h: i386 page tables (identity + sized direct map + user)
                        frame.c/.h: E820-backed physical page inventory
      elf/              elf.c/.h: validated ELF32 PT_LOAD loader + user spaces
      user/             user.asm, child.asm, fault.asm, exec_target.asm,
                        vfork_target.asm: ELF32 test programs (syscall,
                        spawn/waitpid, protection faults, execve/vfork)
    arch/x86_64/          primary target (build.py default); arch/x86 is
                          built with --arch=i386
      boot/             entry64.asm: PAE + EFER.LME + PML4/PDPT/PD, far jump
                        into a 64-bit code segment, then call into C
      kernel/           boot64.c: bring-up sequence (E820 → physmap → frames
                        → heap → console → GDT/TSS → IDT/PIC → timer → spaces
                        → tasks → ring 3 → self-tests);
                        haku64.h: shared types, layout, asm declarations;
                        serial64.c: COM1 output + the mem*/strlen helpers;
                        bootmem64.c: bump allocator for early tables;
                        gdt64.c: GDT, TSS and the IST stacks;
                        idt64.c: 256 gates + exception/IRQ/syscall dispatch;
                        isr64.asm: stubs, CLI/STI/LIDT/LGDT/LTR, port I/O,
                        CR2/CR3, SSE enable, FXSAVE/FXRSTOR, exception
                        probes; pic64.c: 8259 remap, masks, EOI;
                        timer64.c: 100 Hz PIT tick counter;
                        task64.c: tasks, per-task kernel stacks, round-robin
                        preemptive scheduler and the frame-pointer switch;
                        syscall64.c: SYSCALL/SYSRET plus the DPL-3 INT 0x80
                        fallback and the ported call table;
                        fs64.c: FAT12 page cache in front of the ATA disk;
                        rtc64.c: CMOS wall clock;
                        smp64.c: ACPI MADT, local APIC and AP bring-up;
                        module64.c: .hkm version 2 loader, exports and slots;
                        console64.c: serial+display console tee, status bar
                        and console read;
                        fb64.c: maps the VBE aperture into K64_FB_VIRT;
                        kbd64.c: PS/2 set-1 keyboard IRQ1 decoder and queue
      elf/              elf64.c: validated static ELF64 PT_LOAD loader that
                        maps segments into a per-process space
      user/             ring3_elf64.asm: ELF64 test program (write, BSS store,
                        exit); user64_image.asm: embeds the built image
      mm/               paging64.c: four-level paging, physmap and
                        per-process address spaces;
                        frame64.c: E820 frame allocator (no 1 GiB ceiling);
                        heap64.c: 8 MiB free-list kernel heap
    drivers/            ata.c/.h: primary/secondary ATA (master/slave) LBA28/LBA48
                        polling PIO with reset/retry recovery and cache flush
                        terminal.c/.h: text-I/O layer (output + input ring)
                        display.c/.h: shared display backend API
                        vga_text.c/.h: VGA text backend (fallback)
                        framebuffer.c/.h: VBE probe/mapping glue (32-bit port)
                        fbcon.c/.h: shared VBE text renderer, scrolling and
                        status bar (both ports)
                        font.h, font_terminus.c: bitmap font descriptor and the
                        generated Terminus 8x16 table
                        fbhandover.h: the stage-2 VBE record both ports read
                        serial.c/.h: COM1 output backend
    fs/
      fat12/            fat12.c/.h: writable FAT12 files, subdirectories, paths
                        fat12_vfs.c/.h: FAT12 vfs_fs_ops backend + errno mapping
      ramfs/            ramfs.c/.h: in-memory root (long names, modes, links)
      devfs/            devfs.c/.h: /dev/null, zero, urandom, console, tty
      procfs/           procfs.c/.h: generated /proc/version, uptime, meminfo
      initramfs/        cpio.c/.h: cpio "newc" unpacker into the VFS
      vfs/              vfs.c/.h: fs-independent descriptors, mount table
      test/             fs_test.c/.h: kernel-side filesystem self-tests
    kernel/             kernel_main.c: kernel startup
                        scheduler.c/.h: scheduler + per-process usage counters
                        sync.c/.h: mutex/spinlock/IRQ-save locks + rules
                        usermode.c/.h: user mappings, syscall ABI, brk/mmap/munmap
                        getusage
                        klog.c/.h: timestamped, leveled terminal logging
                        panic.c/.h: fatal handler + halted state message
                        crash.c/.h: register/MMU/stack crash reports
                        heap.c/.h: kernel kmalloc/kfree allocator
    module/             haku_module.h: .hkm container layout (version 1 = i386,
                                        2 = x86-64), limits, loader API and
                                        the kernel exports a module may import
                        module.c: 32-bit validation, relocation, export table,
                                  slots, capability ownership and teardown
                        lib/module_lib.c: libhakumod's string and 64-bit helpers
                        lib/runtime.asm: i386 stack probes + shift helpers
                        lib/runtime64.asm: x64 __chkstk stack probes
                        demo/haku_demo.asm: ring-0 demo module (MODULE.HKM)
                        demo/hello.c: C /dev/hello character device (HELLO.HKM)
                        demo/romfs.c: C read-only /rom filesystem (ROMFS.HKM)
                        demo/x64demo.c: x86-64 demo module (X64MOD.HKM)
  user/
    lib/              errno.c, fsabi.c, memory.c, mman.c, signal.c, stdio.c,
                      string.c, unistd.c, crt0.asm, syscall.asm: user C library
                      (32-bit ABI)
    lib64/            errno64.c, memory64.c, mman64.c, stdio64.c, string64.c,
                      unistd64.c, crt64.asm, syscall64.asm, haku_stdarg.h:
                      x86-64 user C library (Microsoft x64 ABI)
    tests/            libtest.c: C library self-test installed as LIB.ELF
                      init64.c: x86-64 C library self-test installed as
                      INIT.ELF and started by the kernel
                      musl_probe.c: portable probe built by the CI musl job
  include/haku/         abi.h + abi.inc + abi64.h + abi64.inc: shared
                        user/kernel ABI constants (the 64-bit pair carries the
                        x86-64 numbers and layouts)
                        types/errno/string/stdlib/unistd/fcntl/stat/stdio/
                        signal/mman/dirent/poll/termios/resource.h: user
                        library headers
                        module.h: the C interface a kernel module is written
                        against (services, device callbacks, fs ops table) on
                        both architectures; capability calls answer -ENOSYS
                        on x86-64 until a 64-bit VFS exists
                        plus haku.h
  tools/pe2bin.py      PE image → flat binary (+ reports ENTRY= for the bootloader)
  tools/mkimg.py       boot floppy size/signature checks
  tools/mkfat12.py     FAT12 image builder for the user ELF
  tools/mkcpio.py      cpio "newc" initramfs builder (INITRD on the FAT volume)
  tools/pe2elf.py      fixed-base PE32 to ELF32 (RX/R/RW) for user C
  tools/pe2elf64.py    fixed-base PE32+ to ELF64 (RX/R/RW) for the 64-bit user
                      C library and /INIT.ELF
  tools/mkelf64.py     flat x86-64 binary → static ELF64 (RX code + RW BSS) for
                      the 64-bit loader test
  tools/mkhakumod.py   relocatable objects (NASM ELF32 or COFF i386/amd64) →
                       packed .hkm module; links C objects, folds COMDAT, drops
                       unreachable sections
  .github/workflows/  ci.yml: hosted x86-64 + i386 headless build/test, artifacts
                      release.yml: tag-driven bootable image release
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (stage*.bin, *.obj/pe, haku64.img, ...)
  haku64.img           primary bootable floppy image (x86-64)
  haku.img             bootable floppy image (i386, --arch=i386)
  build/haku64_fs.img  1.44 MB FAT12 ATA disk for the x64 kernel
  build/haku_fs.img    1.44 MB FAT12 ATA disk image with HAKU/NEXT/LIB ELFs
                      and the INITRD initramfs archive
  build/ata_spare.img  1.44 MB blank ATA disk on the secondary channel (scan test)
```
