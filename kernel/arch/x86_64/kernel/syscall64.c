/* Haku x86-64 system calls: SYSCALL/SYSRET with an INT 0x80 fallback.
 *
 * SYSCALL is the fast path: LSTAR points at syscall64_entry in isr64.asm,
 * which switches to the task's kernel stack, builds the same regs64_t frame
 * every interrupt stub builds, and calls sys64_dispatch. It returns with
 * SYSRET unless the scheduler handed back a kernel frame, in which case the
 * shared IRETQ tail resumes it. The IDT keeps a DPL-3 gate at vector 0x80,
 * so code that only knows INT 0x80 still works and the error-path tests can
 * cover both entry points.
 *
 * Calling convention is the x86-64 Linux one:
 *   RAX  number
 *   RDI  first argument, RSI second, RDX third, R10 fourth, R8 fifth,
 *   R9   sixth
 *   RAX  result, or a negative errno
 * The handler edits the frame in place and the entry stub delivers the
 * result back to ring 3.
 *
 * Every user pointer is resolved through the page tables of the current
 * space before it is read or written. A ring-0 page fault is fatal on this
 * port (idt64.c halts), so a syscall cannot rely on the CPU to reject a bad
 * address; paging64_query_space answers "present? writable? user?" and the
 * copy loops below only touch physical frames through the physmap.
 */
#include "haku64.h"

#define SYS64_MAX_WRITE   4096ull
#define SYS64_IOV_MAX     8u
#define SYS64_IOV_SIZE    16ull
#define SYS64_TIMESPEC    16ull

#define SYS64_EPERM  1ull
#define SYS64_EBADF  9ull
#define SYS64_ENOMEM 12ull
#define SYS64_EFAULT 14ull
#define SYS64_EINVAL 22ull
#define SYS64_ENOSYS 38ull

/* RFLAGS bits SYSCALL clears (FMASK): TF, IF, DF and AC. Interrupts stay
 * off for the whole handler, exactly as with the interrupt gate. */
#define SYS64_FMASK (0x100ull | 0x200ull | 0x400ull | 0x40000ull)

void syscall64_init(void) {
    u64 star = ((u64)K64_STAR_BASE << 48) | ((u64)K64_KERNEL_CS << 32);

    syscall64_setup(star, (u64)syscall64_entry, SYS64_FMASK);
    k64_puts("HAKU64-SYSCALL64-SETUP: star=");
    k64_hex(star);
    k64_puts(" entry=");
    k64_hex((u64)syscall64_entry);
    k64_puts("\r\n");
}

static u64 syscall_count;
static u32 ring3_seen;
static u32 syscall_ok_seen;

static u64 current_space(void) {
    return read_cr3_64() & K64_PAGE_MASK;
}

/* Resolve one user address in `space`. Returns 0 unless the page is
 * mapped, belongs to the user half and (when asked) is writable. */
static int user_page64(u64 space, u64 address, u64 *phys, int need_write) {
    u64 page;
    int writable, user;

    if (address < K64_USER_BASE || address >= K64_USER_END) return 0;
    if (!paging64_query_space(space, address, &page, &writable, &user))
        return 0;
    if (!user) return 0;
    if (need_write && !writable) return 0;
    *phys = page;
    return 1;
}

/* Copy a kernel buffer into user memory one page at a time. */
static int copy_to_user64(u64 space, u64 address, const void *src,
                          u64 length) {
    const u8 *from = (const u8 *)src;

    while (length > 0) {
        u64 phys, offset, chunk;

        if (!user_page64(space, address, &phys, 1)) return 0;
        offset = address & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        memcpy((u8 *)phys_to_virt64(phys), from, chunk);
        address += chunk;
        from += chunk;
        length -= chunk;
    }
    return 1;
}

/* Copy from user memory one page at a time, for structures such as iovec
 * arrays that the kernel has to inspect. */
static int copy_from_user64(u64 space, void *dst, u64 address, u64 length) {
    u8 *to = (u8 *)dst;

    while (length > 0) {
        u64 phys, offset, chunk;

        if (!user_page64(space, address, &phys, 0)) return 0;
        offset = address & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        memcpy(to, phys_to_virt64(phys), chunk);
        address += chunk;
        to += chunk;
        length -= chunk;
    }
    return 1;
}

static void note_write(u64 count) {
    if (syscall_ok_seen || count == 0) return;
    syscall_ok_seen = 1;
    k64_puts("\r\nHAKU64-SYSCALL-OK: write n=");
    k64_dec(count);
    k64_puts("\r\n");
}

/* write(1 or 2, buffer, count). Output is raw bytes: the caller decides
 * about line endings. A bad page after some bytes were written is a short
 * write, which is what write() is allowed to do. */
static u64 sys64_write(u64 fd, u64 buffer, u64 count) {
    u64 space, written = 0;

    if (fd != 1 && fd != 2) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    space = current_space();
    while (written < count) {
        u64 phys, offset, chunk;

        if (!user_page64(space, buffer + written, &phys, 0)) {
            if (written) break;
            return (u64)0 - SYS64_EFAULT;
        }
        offset = (buffer + written) & (K64_PAGE_SIZE - 1);
        chunk = K64_PAGE_SIZE - offset;
        if (chunk > count - written) chunk = count - written;
        for (u64 i = 0; i < chunk; i++)
            k64_putc(*(const volatile char *)phys_to_virt64(phys + i));
        written += chunk;
    }
    return written;
}

/* writev(fd, iovec, count). Only the first SYS64_IOV_MAX entries are
 * consumed; a partial write stops the walk like write() does. */
static u64 sys64_writev(u64 fd, u64 iov, u64 count) {
    u64 space, total = 0;
    u8 entry[SYS64_IOV_SIZE];

    if (fd != 1 && fd != 2) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_IOV_MAX) count = SYS64_IOV_MAX;

    space = current_space();
    for (u64 i = 0; i < count; i++) {
        u64 base, length, n;

        if (!copy_from_user64(space, entry, iov + i * SYS64_IOV_SIZE,
                              SYS64_IOV_SIZE)) {
            if (total) break;
            return (u64)0 - SYS64_EFAULT;
        }
        memcpy(&base, entry, 8);
        memcpy(&length, entry + 8, 8);

        n = sys64_write(fd, base, length);
        if ((s64)n < 0) {
            if (total) break;
            return n;
        }
        total += n;
        if (n < length) break;
    }
    return total;
}

/* mmap(address, length, prot, flags, fd, offset): anonymous mappings only,
 * private or shared. Without MAP_FIXED the address must be 0 and the range
 * comes from the task's bump pointer. MAP_FIXED takes a page-aligned
 * address inside the anonymous window and refuses a range that is already
 * mapped instead of replacing it, so a fixed mapping can never silently
 * discard the program or its stack. A file descriptor or offset is still
 * -EINVAL: file-backed mappings need the 64-bit VFS. */
static u64 sys64_mmap(u64 address, u64 length, u64 prot, u64 flags, u64 fd,
                      u64 offset) {
    int id = sched64_current();
    u64 space, pages, base, kind, end;
    int writable, fixed;

    if (length == 0 || fd != ~0ull || offset != 0)
        return (u64)0 - SYS64_EINVAL;
    if (prot & ~(u64)(K64_PROT_READ | K64_PROT_WRITE | K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if ((prot & K64_PROT_WRITE) && (prot & K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if (length > (u64)K64_USER_MMAP_PAGES_MAX * K64_PAGE_SIZE)
        return (u64)0 - SYS64_ENOMEM;

    kind = flags & ~(u64)(K64_MAP_PRIVATE | K64_MAP_SHARED | K64_MAP_FIXED |
                          K64_MAP_ANONYMOUS);
    if (kind != 0) return (u64)0 - SYS64_EINVAL;
    if ((flags & K64_MAP_ANONYMOUS) == 0) return (u64)0 - SYS64_EINVAL;
    if (((flags & K64_MAP_PRIVATE) != 0) == ((flags & K64_MAP_SHARED) != 0))
        return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    fixed = (flags & K64_MAP_FIXED) != 0;
    space = sched64_space(id);

    if (fixed) {
        end = address + length;
        if (address == 0 || (address & (K64_PAGE_SIZE - 1)) != 0)
            return (u64)0 - SYS64_EINVAL;
        if (end < address || address < K64_USER_MMAP_BASE ||
            end > K64_USER_MMAP_END)
            return (u64)0 - SYS64_EINVAL;
        /* Refuse occupied ranges rather than unmapping them: the window is
         * where this port hands out anonymous pages, and replacing a
         * mapping here is not worth the bookkeeping yet. */
        for (u64 i = 0; i < pages; i++)
            if (paging64_query_space(space, address + i * K64_PAGE_SIZE,
                                     0, 0, 0))
                return (u64)0 - SYS64_EINVAL;
        base = address;
    } else {
        if (address != 0) return (u64)0 - SYS64_EINVAL;
        base = task64_mmap_reserve(id, pages);
        if (!base) return (u64)0 - SYS64_ENOMEM;
    }

    writable = (prot & K64_PROT_WRITE) != 0;
    for (u64 i = 0; i < pages; i++) {
        u64 virt = base + i * K64_PAGE_SIZE;
        u64 frame = frame64_alloc();

        if (!frame) goto fail;
        memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
        if (!paging64_map_space(space, virt, frame, writable, 1,
                                (prot & K64_PROT_EXEC) != 0)) {
            frame64_free(frame);
            goto fail;
        }
    }
    return base;

fail:
    /* Release the pages mapped so far. The bump pointer is not rewound;
     * the range stays reserved, which is simpler than tracking holes. */
    for (u64 i = 0; i < pages; i++) {
        u64 virt = base + i * K64_PAGE_SIZE;
        u64 phys;

        if (!paging64_query_space(space, virt, 0, 0, 0)) continue;
        phys = paging64_unmap_space(space, virt);
        if (phys) frame64_free(phys);
    }
    return (u64)0 - SYS64_ENOMEM;
}

/* munmap(address, length). The address must be page aligned; a page that
 * is already absent is skipped, so unmapping twice is harmless. */
static u64 sys64_munmap(u64 address, u64 length) {
    u64 space, pages;

    if ((address & (K64_PAGE_SIZE - 1)) != 0) return (u64)0 - SYS64_EINVAL;
    if (length == 0) return (u64)0 - SYS64_EINVAL;
    if (address < K64_USER_BASE || address >= K64_USER_END)
        return (u64)0 - SYS64_EINVAL;
    if (length > K64_USER_END - address) return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    space = current_space();
    for (u64 i = 0; i < pages; i++) {
        u64 phys = paging64_unmap_space(space, address + i * K64_PAGE_SIZE);

        if (phys) frame64_free(phys);
    }
    return 0;
}

/* getpid(): task index + 1, so a process never sees pid 0. */
static u64 sys64_getpid(void) {
    int id = sched64_current();

    return id < 0 ? 1 : (u64)(id + 1);
}

/* clock_gettime(clock_id, timespec). CLOCK_MONOTONIC counts timer ticks
 * from boot; CLOCK_REALTIME adds the RTC epoch sampled at boot. When the
 * RTC is unreadable the realtime clock degrades to the monotonic one, so
 * the call still succeeds. */
static u64 sys64_clock_gettime(u64 clock_id, u64 tp) {
    u64 ticks, hz, seconds, wall, timespec[2];

    if (clock_id != K64_CLOCK_REALTIME && clock_id != K64_CLOCK_MONOTONIC)
        return (u64)0 - SYS64_EINVAL;
    hz = timer64_hz();
    if (hz == 0) return (u64)0 - SYS64_EINVAL;

    ticks = timer64_ticks();
    seconds = ticks / hz;
    if (clock_id == K64_CLOCK_REALTIME) {
        wall = rtc64_realtime(ticks, hz);
        if (wall != 0) seconds = wall;
    }
    timespec[0] = seconds;
    timespec[1] = (ticks % hz) * (1000000000ull / hz);

    if (!copy_to_user64(current_space(), tp, timespec, SYS64_TIMESPEC))
        return (u64)0 - SYS64_EFAULT;
    return 0;
}

regs64_t *sys64_dispatch(regs64_t *frame) {
    u32 in_user = ((u32)frame->cs & 3u) == 3u;
    u32 wrote = 0;
    u64 result;

    syscall_count++;
    if (!in_user) {
        k64_puts("HAKU64-SYSCALL-FAIL: int 0x80 outside ring 3\r\n");
        frame->rax = (u64)0 - SYS64_EPERM;
        return frame;
    }

    if (!ring3_seen) {
        ring3_seen = 1;
        k64_puts("HAKU64-USER-OK: ring 3 entered cs=");
        k64_hex(frame->cs);
        k64_puts(" rip=");
        k64_hex(frame->rip);
        k64_puts("\r\n");
    }

    switch (frame->rax) {
    case K64_SYS_WRITE:
        result = sys64_write(frame->rdi, frame->rsi, frame->rdx);
        wrote = 1;
        break;

    case K64_SYS_WRITEV:
        result = sys64_writev(frame->rdi, frame->rsi, frame->rdx);
        wrote = 1;
        break;

    case K64_SYS_MMAP:
        result = sys64_mmap(frame->rdi, frame->rsi, frame->rdx,
                            frame->r10, frame->r8, frame->r9);
        break;

    case K64_SYS_MUNMAP:
        result = sys64_munmap(frame->rdi, frame->rsi);
        break;

    case K64_SYS_GETPID:
        result = sys64_getpid();
        break;

    case K64_SYS_CLOCK_GETTIME:
        result = sys64_clock_gettime(frame->rdi, frame->rsi);
        break;

    case K64_SYS_EXIT:
    case K64_SYS_EXIT_GROUP:
        /* exit_status is recorded by sched64_exit from RDI. */
        return sched64_exit(frame);

    default:
        result = (u64)0 - SYS64_ENOSYS;
        break;
    }

    frame->rax = result;
    if (wrote && (s64)result > 0)
        note_write(result);
    return frame;
}

u64 sys64_count(void) {
    return syscall_count;
}
