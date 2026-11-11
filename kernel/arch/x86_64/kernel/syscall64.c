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
#define SYS64_ENOENT 2ull
#define SYS64_EIO    5ull
#define SYS64_E2BIG  7ull
#define SYS64_ENOEXEC 8ull
#define SYS64_EBADF  9ull
#define SYS64_ECHILD 10ull
#define SYS64_ENOMEM 12ull
#define SYS64_EACCES 13ull
#define SYS64_EFAULT 14ull
#define SYS64_EAGAIN 11ull
#define SYS64_EINVAL 22ull
#define SYS64_ENOSPC 28ull
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

/* write(fd, buffer, count). The console descriptors (0..2 until redirected,
 * and any descriptor dup()ed from them) go to COM1; a file descriptor from
 * fs64.c writes into the page cache, which is flushed to the FAT12 volume
 * when the file is closed. A bad page after some bytes were written is a
 * short write, which is what write() is allowed to do. */
static u64 sys64_write(u64 fd, u64 buffer, u64 count) {
    int id = sched64_current(), file, access;
    u64 space, written = 0;

    if (!fs64_fd_console(id, (int)fd)) {
        u8 chunk[64];
        u64 pos, done = 0;

        file = fs64_fd_file(id, (int)fd);
        if (file < 0) return (u64)0 - SYS64_EBADF;
        access = fs64_fd_access(id, (int)fd);
        if (access == 0) return (u64)0 - SYS64_EBADF;   /* O_RDONLY */
        if (count == 0) return 0;
        if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

        pos = fs64_fd_pos(id, (int)fd);
        space = current_space();
        while (done < count) {
            u64 n = count - done;

            if (n > sizeof(chunk)) n = sizeof(chunk);
            if (!copy_from_user64(space, chunk, buffer + done, n)) {
                if (done) break;
                return (u64)0 - SYS64_EFAULT;
            }
            n = fs64_write(file, pos + done, chunk, n);
            if (n == 0) {
                if (done == 0) return (u64)0 - SYS64_ENOSPC;
                break;
            }
            done += n;
        }
        fs64_fd_seek(id, (int)fd, pos + done);
        return done;
    }

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

/* writev(fd, iovec, count). Each segment goes through write(), so console
 * and file descriptors both work. Only the first SYS64_IOV_MAX entries are
 * consumed; a partial write stops the walk like write() does. */
static u64 sys64_writev(u64 fd, u64 iov, u64 count) {
    u64 space, total = 0;
    u8 entry[SYS64_IOV_SIZE];

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

/* mmap(address, length, prot, flags, fd, offset): anonymous or file-backed.
 * Without MAP_FIXED the address must be 0 and the range comes from the
 * task's bump pointer; MAP_FIXED takes a page-aligned address inside the
 * anonymous window and refuses a range that is already mapped instead of
 * replacing it, so a fixed mapping can never silently discard the program
 * or its stack. A file-backed mapping needs an open descriptor and a
 * page-aligned offset inside the file: MAP_SHARED maps the file's own
 * frames (writes are visible to read), MAP_PRIVATE copies them at map time
 * because there is no fork and therefore no COW. Mapping past the end of
 * the file is -EINVAL rather than the SIGBUS a Linux process would see. */
static u64 sys64_mmap(u64 address, u64 length, u64 prot, u64 flags, u64 fd,
                      u64 offset) {
    int id = sched64_current();
    u64 space, pages, base, kind, end, file_size;
    int writable, fixed, anon, file = -1, owned = 1, shared;

    if (length == 0) return (u64)0 - SYS64_EINVAL;
    if (prot & ~(u64)(K64_PROT_READ | K64_PROT_WRITE | K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if ((prot & K64_PROT_WRITE) && (prot & K64_PROT_EXEC))
        return (u64)0 - SYS64_EINVAL;
    if (length > (u64)K64_USER_MMAP_PAGES_MAX * K64_PAGE_SIZE)
        return (u64)0 - SYS64_ENOMEM;

    kind = flags & ~(u64)(K64_MAP_PRIVATE | K64_MAP_SHARED | K64_MAP_FIXED |
                          K64_MAP_ANONYMOUS);
    if (kind != 0) return (u64)0 - SYS64_EINVAL;
    if (((flags & K64_MAP_PRIVATE) != 0) == ((flags & K64_MAP_SHARED) != 0))
        return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    anon = (flags & K64_MAP_ANONYMOUS) != 0;
    shared = (flags & K64_MAP_SHARED) != 0;
    if (anon) {
        if (fd != ~0ull || offset != 0) return (u64)0 - SYS64_EINVAL;
    } else {
        if ((offset & (K64_PAGE_SIZE - 1)) != 0)
            return (u64)0 - SYS64_EINVAL;
        file = fs64_fd_file(id, (int)fd);
        if (file < 0) return (u64)0 - SYS64_EBADF;
        file_size = fs64_pages(file) * K64_PAGE_SIZE;
        if (offset >= file_size) return (u64)0 - SYS64_EINVAL;
        if (length > file_size - offset) return (u64)0 - SYS64_EINVAL;
        /* A writable shared mapping writes the file, so the descriptor has
         * to be open for writing; a private copy does not touch the file. */
        if (shared && (prot & K64_PROT_WRITE) &&
            fs64_fd_access(id, (int)fd) == 0)
            return (u64)0 - SYS64_EACCES;
        /* Shared pages are the file's; private pages are copies we own and
         * therefore have to free again on munmap. */
        owned = !shared;
    }

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

    if (!task64_map_record(id, base, pages, owned, shared))
        return (u64)0 - SYS64_ENOMEM;

    writable = (prot & K64_PROT_WRITE) != 0;
    for (u64 i = 0; i < pages; i++) {
        u64 virt = base + i * K64_PAGE_SIZE;
        u64 frame;

        if (anon || owned) {
            frame = frame64_alloc();
            if (!frame) goto fail;
            memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
            if (!anon)
                fs64_read(file, offset + i * K64_PAGE_SIZE,
                          phys_to_virt64(frame), K64_PAGE_SIZE);
        } else {
            frame = fs64_page(file, offset / K64_PAGE_SIZE + i);
            if (!frame) goto fail;
        }
        if (!paging64_map_space(space, virt, frame, writable, 1,
                                (prot & K64_PROT_EXEC) != 0)) {
            if (anon || owned) frame64_free(frame);
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
        if (phys && task64_map_owned(id, virt)) frame64_free(phys);
    }
    task64_map_drop(id, base, pages * K64_PAGE_SIZE);
    return (u64)0 - SYS64_ENOMEM;
}

/* munmap(address, length). The address must be page aligned; a page that
 * is already absent is skipped, so unmapping twice is harmless. The mapping
 * records decide whether a frame is ours to free: a MAP_SHARED file mapping
 * aliases frames the file still owns. */
static u64 sys64_munmap(u64 address, u64 length) {
    int id = sched64_current();
    u64 space, pages;

    if ((address & (K64_PAGE_SIZE - 1)) != 0) return (u64)0 - SYS64_EINVAL;
    if (length == 0) return (u64)0 - SYS64_EINVAL;
    if (address < K64_USER_BASE || address >= K64_USER_END)
        return (u64)0 - SYS64_EINVAL;
    if (length > K64_USER_END - address) return (u64)0 - SYS64_EINVAL;

    pages = (length + K64_PAGE_SIZE - 1) / K64_PAGE_SIZE;
    space = current_space();
    for (u64 i = 0; i < pages; i++) {
        u64 virt = address + i * K64_PAGE_SIZE;
        u64 phys = paging64_unmap_space(space, virt);

        if (phys && task64_map_owned(id, virt)) frame64_free(phys);
    }
    task64_map_drop(id, address, pages * K64_PAGE_SIZE);
    return 0;
}

/* open(path, flags, mode): the FAT12 page cache. The path is copied out of
 * user memory one byte at a time -- a page-sized copy would fault on a short
 * string that ends near the end of a page -- into a bounded kernel buffer,
 * so nothing here can run off the end of a name. O_CREAT/O_EXCL/O_TRUNC/
 * O_APPEND and the three access modes are the accepted set. */
static u64 sys64_open(u64 path, u64 flags, u64 mode) {
    char name[K64_FS_PATH_MAX + 1];
    u64 space = current_space();
    int fd, error = 0;

    (void)mode;

    for (u64 i = 0; i <= (u64)K64_FS_PATH_MAX; i++) {
        if (!copy_from_user64(space, &name[i], path + i, 1))
            return (u64)0 - SYS64_EFAULT;
        if (name[i] == '\0') break;
        if (i == (u64)K64_FS_PATH_MAX) return (u64)0 - SYS64_EINVAL;
    }

    fd = fs64_open_fd(sched64_current(), name, flags, &error);
    if (fd < 0) return (u64)error;
    return (u64)fd;
}

/* mkdir(path, mode): one directory, 8.3 name, parent must exist. */
static u64 sys64_mkdir(u64 path, u64 mode) {
    char name[K64_FS_PATH_MAX + 1];
    u64 space = current_space();

    (void)mode;
    for (u64 i = 0; i <= (u64)K64_FS_PATH_MAX; i++) {
        if (!copy_from_user64(space, &name[i], path + i, 1))
            return (u64)0 - SYS64_EFAULT;
        if (name[i] == '\0') break;
        if (i == (u64)K64_FS_PATH_MAX) return (u64)0 - SYS64_EINVAL;
    }
    return (u64)fs64_create_dir(name);
}

/* read(fd, buffer, count): file descriptors only. The data is staged in a
 * small kernel buffer and copied out page by page, so a bad pointer after
 * some bytes is a short read rather than a fault. */
static u64 sys64_read(u64 fd, u64 buffer, u64 count) {
    int id = sched64_current(), file;
    u64 space, pos, size, done = 0;
    u8 chunk[64];

    file = fs64_fd_file(id, (int)fd);
    if (file < 0) return (u64)0 - SYS64_EBADF;
    if (fs64_fd_access(id, (int)fd) == 1) return (u64)0 - SYS64_EBADF;
    if (count == 0) return 0;
    if (count > SYS64_MAX_WRITE) count = SYS64_MAX_WRITE;

    pos = fs64_fd_pos(id, (int)fd);
    size = fs64_size(file);
    if (pos >= size) return 0;
    if (count > size - pos) count = size - pos;

    space = current_space();
    while (done < count) {
        u64 n = count - done;

        if (n > sizeof(chunk)) n = sizeof(chunk);
        n = fs64_read(file, pos + done, chunk, n);
        if (n == 0) break;
        if (!copy_to_user64(space, buffer + done, chunk, n)) {
            if (done) break;
            return (u64)0 - SYS64_EFAULT;
        }
        done += n;
    }
    fs64_fd_seek(id, (int)fd, pos + done);
    return done;
}

/* close(fd): -EBADF for anything that is not an open descriptor. Closing a
 * console descriptor that was never duplicated is a no-op (the implicit
 * console has no description to release). */
static u64 sys64_close(u64 fd) {
    int id = sched64_current();

    if (fs64_fd_file(id, (int)fd) < 0 && !fs64_fd_console(id, (int)fd))
        return (u64)0 - SYS64_EBADF;
    fs64_fd_close(id, (int)fd);
    return 0;
}

/* dup(fd): the lowest free descriptor at or above 3 with one description
 * shared with fd, so both follow the same file offset. 0..2 are the console
 * and therefore never handed out by dup(); dup2 can still target them. */
static u64 sys64_dup(u64 fd) {
    int result = fs64_fd_dup(sched64_current(), (int)fd, 3);

    return (u64)(s64)result;
}

/* dup2(fd, target): point `target` at fd's description, closing whatever it
 * held. This is how a shell redirects stdout onto a file. */
static u64 sys64_dup2(u64 fd, u64 target) {
    int result = fs64_fd_dup2(sched64_current(), (int)fd, (int)target);

    return (u64)(s64)result;
}

/* execve's argv/envp arrive as a user array of user string pointers. Both are
 * copied into kernel buffers before the address space is replaced; the caps
 * keep the copy off the kernel stack's deep end and are reported as E2BIG. */
#define SYS64_EXEC_VEC_MAX   8u
#define SYS64_EXEC_ARG_MAX   128u
#define SYS64_EXEC_BYTES_MAX 768u
#define SYS64_EXEC_WORDS_MAX 64u

#define SYS64_AT_NULL   0ull
#define SYS64_AT_PAGESZ 6ull
#define SYS64_AT_ENTRY  9ull

typedef struct {
    u32 count;
    u32 length;
    u32 offsets[SYS64_EXEC_VEC_MAX];
    u32 lengths[SYS64_EXEC_VEC_MAX];   /* including the terminator */
    char strings[SYS64_EXEC_BYTES_MAX];
} exec_vector64_t;

/* Copy one NUL-terminated string out of user memory in page-sized chunks.
 * Returns the length including the terminator when it fit in `max` bytes, 0
 * when it did not, and -1 when the pointer is unreadable. One page-table
 * walk per page, not one per byte. */
static int copy_user_string64(u64 space, u64 address, char *dst, u32 max) {
    u32 done = 0;

    while (done < max) {
        u64 page = K64_PAGE_SIZE - ((address + done) & (K64_PAGE_SIZE - 1));
        u32 chunk = max - done;

        if (page < chunk) chunk = (u32)page;
        if (!copy_from_user64(space, dst + done, address + done, chunk))
            return -1;
        for (u32 i = 0; i < chunk; i++)
            if (dst[done + i] == '\0') return (int)(done + i + 1);
        done += chunk;
    }
    return 0;
}

/* Capture one vector. `user_vector` 0 is an empty vector, and a missing
 * terminating NULL past the maximum is E2BIG, not a silent truncation. */
static int capture_vector64(u64 space, u64 user_vector,
                            exec_vector64_t *out) {
    out->count = 0;
    out->length = 0;
    if (!user_vector) return 0;

    for (u32 i = 0; i < SYS64_EXEC_VEC_MAX; i++) {
        u64 entry;
        u32 room;
        int length;
        if (!copy_from_user64(space, &entry, user_vector + i * 8ull, 8))
            return SYS64_EFAULT;
        if (entry == 0) return 0;
        if (out->length >= SYS64_EXEC_BYTES_MAX) return SYS64_E2BIG;

        room = SYS64_EXEC_BYTES_MAX - out->length;
        if (room > SYS64_EXEC_ARG_MAX) room = SYS64_EXEC_ARG_MAX;
        length = copy_user_string64(space, entry,
                                    &out->strings[out->length], room);
        if (length < 0) return SYS64_EFAULT;
        if (length == 0) return SYS64_E2BIG;

        out->offsets[i] = out->length;
        out->lengths[i] = (u32)length;
        out->length += (u32)length;
        out->count++;
    }
    return SYS64_E2BIG;
}

/* Copy one captured vector onto the new stack, growing downwards. The
 * strings keep their byte order and only the word block below is aligned. */
static int place_vector64(u64 space, u64 bottom, const exec_vector64_t *v,
                          u64 *sp, u64 *addresses) {
    for (u32 i = 0; i < v->count; i++) {
        u64 length = v->lengths[i];

        if (*sp < bottom + length) return SYS64_E2BIG;
        *sp -= length;
        if (!copy_to_user64(space, *sp, &v->strings[v->offsets[i]], length))
            return SYS64_EIO;
        addresses[i] = *sp;
    }
    return 0;
}

/* Lay out argc/argv/NULL/envp/NULL and a small auxiliary vector on the stack
 * elf64_load allocated. AT_PAGESZ and AT_ENTRY are what a static startup stub
 * asks for; there is no AT_PHDR because the loader does not expose it yet. */
static int build_exec_stack64(loaded_elf64_t *loaded,
                              const exec_vector64_t *argv,
                              const exec_vector64_t *envp) {
    u64 argv_addresses[SYS64_EXEC_VEC_MAX];
    u64 envp_addresses[SYS64_EXEC_VEC_MAX];
    u64 words[SYS64_EXEC_WORDS_MAX];
    u64 count = 0, size, sp = loaded->stack_top;
    int error;

    error = place_vector64(loaded->space, loaded->stack_bottom, argv, &sp,
                           argv_addresses);
    if (error) return error;
    error = place_vector64(loaded->space, loaded->stack_bottom, envp, &sp,
                           envp_addresses);
    if (error) return error;

    words[count++] = argv->count;
    for (u32 i = 0; i < argv->count; i++) words[count++] = argv_addresses[i];
    words[count++] = 0;
    for (u32 i = 0; i < envp->count; i++) words[count++] = envp_addresses[i];
    words[count++] = 0;
    words[count++] = SYS64_AT_PAGESZ;
    words[count++] = K64_PAGE_SIZE;
    words[count++] = SYS64_AT_ENTRY;
    words[count++] = loaded->entry;
    words[count++] = SYS64_AT_NULL;
    words[count++] = 0;

    size = count * 8ull;
    if (sp < loaded->stack_bottom + size) return SYS64_E2BIG;
    sp = (sp - size) & ~15ull;
    if (sp < loaded->stack_bottom) return SYS64_E2BIG;
    if (!copy_to_user64(loaded->space, sp, words, size)) return SYS64_EIO;
    loaded->stack_top = sp;
    return 0;
}

/* execve(path, argv, envp): replace the caller's image with an ELF64 file
 * from the FAT12 volume. argv/envp strings are copied into kernel buffers
 * first, because the old address space is gone once the new one is current;
 * then the whole file is read into the heap (the page cache caps a file at
 * 32 pages), elf64_load validates and maps it, its stack gets argc/argv/
 * envp and a three-entry auxiliary vector, and task64_exec releases the old
 * space. Descriptors survive execve because there is no O_CLOEXEC. On
 * failure nothing has changed and the caller continues normally. */
static regs64_t *sys64_execve(regs64_t *frame) {
    char name[K64_FS_PATH_MAX + 1];
    exec_vector64_t argv, envp;
    loaded_elf64_t loaded;
    u64 space = current_space();
    u64 size;
    u8 *buffer;
    int error, id = sched64_current(), file;

    for (u64 i = 0; i <= (u64)K64_FS_PATH_MAX; i++) {
        if (!copy_from_user64(space, &name[i], frame->rdi + i, 1)) {
            frame->rax = (u64)0 - SYS64_EFAULT;
            return frame;
        }
        if (name[i] == '\0') break;
        if (i == (u64)K64_FS_PATH_MAX) {
            frame->rax = (u64)0 - SYS64_EINVAL;
            return frame;
        }
    }

    error = capture_vector64(space, frame->rsi, &argv);
    if (error == 0) error = capture_vector64(space, frame->rdx, &envp);
    if (error) {
        frame->rax = (u64)0 - (u64)error;
        return frame;
    }

    file = fs64_lookup(name);
    if (file < 0) {
        frame->rax = (u64)0 - SYS64_ENOENT;
        return frame;
    }
    size = fs64_size(file);
    if (size == 0 || size > (u64)0x20000) {
        fs64_release(file);
        frame->rax = (u64)0 - (size == 0 ? SYS64_ENOEXEC : SYS64_ENOMEM);
        return frame;
    }

    buffer = (u8 *)kmalloc64(size);
    if (!buffer) {
        fs64_release(file);
        frame->rax = (u64)0 - SYS64_ENOMEM;
        return frame;
    }
    if (fs64_read(file, 0, buffer, size) != size) {
        kfree64(buffer);
        fs64_release(file);
        frame->rax = (u64)0 - SYS64_EIO;
        return frame;
    }
    fs64_release(file);

    if (!elf64_load(buffer, size, &loaded)) {
        kfree64(buffer);
        frame->rax = (u64)0 - SYS64_ENOEXEC;
        return frame;
    }
    kfree64(buffer);

    error = build_exec_stack64(&loaded, &argv, &envp);
    if (error) {
        elf64_unload(&loaded);
        frame->rax = (u64)0 - (u64)error;
        return frame;
    }

    if (!task64_exec(id, loaded.space)) {
        elf64_unload(&loaded);
        frame->rax = (u64)0 - SYS64_ENOMEM;
        return frame;
    }
    paging64_switch_space(loaded.space);

    /* Nothing of the old image may leak into the new one: clear the general
     * registers and enter at the new entry with the new stack. */
    frame->rax = 0;
    frame->rbx = 0;
    frame->rcx = 0;
    frame->rdx = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;
    frame->r8 = 0;
    frame->r9 = 0;
    frame->r10 = 0;
    frame->r11 = 0;
    frame->r12 = 0;
    frame->r13 = 0;
    frame->r14 = 0;
    frame->r15 = 0;
    frame->rip = loaded.entry;
    frame->rsp = loaded.stack_top;
    return frame;
}

/* lseek(fd, offset, whence). The offset is a signed byte count, so a
 * negative result is -EINVAL instead of a huge unsigned position. */
static u64 sys64_lseek(u64 fd, u64 offset, u64 whence) {
    int id = sched64_current();
    int file = fs64_fd_file(id, (int)fd);
    s64 delta = (s64)offset, pos;

    if (file < 0) return (u64)0 - SYS64_EBADF;
    switch (whence) {
    case K64_SEEK_SET: pos = 0; break;
    case K64_SEEK_CUR: pos = (s64)fs64_fd_pos(id, (int)fd); break;
    case K64_SEEK_END: pos = (s64)fs64_size(file); break;
    default: return (u64)0 - SYS64_EINVAL;
    }
    if (delta > 0 && pos > (s64)0x7FF0000000000000ull - delta)
        return (u64)0 - SYS64_EINVAL;
    pos += delta;
    if (pos < 0) return (u64)0 - SYS64_EINVAL;

    fs64_fd_seek(id, (int)fd, (u64)pos);
    return (u64)pos;
}

/* getpid(): the thread group's pid, which is task index + 1, so a process
 * never sees pid 0 and a thread sees the same value as its creator. */
static u64 sys64_getpid(void) {
    int id = sched64_tgid(sched64_current());

    return id < 0 ? 1 : (u64)(id + 1);
}

/* getppid(): the parent process's pid, or 0 for a task the boot context
 * created. A thread answers with its group leader's parent, like Linux. */
static u64 sys64_getppid(void) {
    int leader = sched64_tgid(sched64_current());
    int parent = leader < 0 ? -1 : task64_parent(leader);

    if (parent >= 0) parent = sched64_tgid(parent);
    return parent < 0 ? 0 : (u64)(parent + 1);
}

/* gettid(): this task's own id, distinct from getpid() inside a thread
 * group and equal to it for a single-threaded process. */
static u64 sys64_gettid(void) {
    int id = sched64_current();

    return id < 0 ? 1 : (u64)(id + 1);
}

/* clone(flags, child_stack, parent_tid, child_tid, tls): CLONE_VM is what
 * makes this a thread, because the child then runs in the caller's address
 * space with its own stack. Without CLONE_VM the call degrades to a fork, so
 * a caller that asks for a process still gets one. The unsupported flags are
 * refused instead of ignored: a task must not believe it got TLS or a
 * clear-on-exit child TID when the kernel does not implement them. */
static regs64_t *sys64_clone(regs64_t *frame) {
    u64 flags = frame->rdi;
    u64 parent_tid = frame->rdx;
    u64 space = current_space();

    if (flags & ~(u64)K64_CLONE_SUPPORTED) {
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }
    if ((flags & K64_CLONE_PARENT_SETTID) &&
        !user_page64(space, parent_tid, 0, 1)) {
        frame->rax = (u64)0 - SYS64_EFAULT;
        return frame;
    }

    frame = sched64_clone(frame, flags, frame->rsi);
    if ((s64)frame->rax > 0 && (flags & K64_CLONE_PARENT_SETTID)) {
        u32 pid = (u32)frame->rax;

        /* The parent's space is still current: clone() never switched. */
        if (!copy_to_user64(space, parent_tid, &pid, 4))
            frame->rax = (u64)0 - SYS64_EFAULT;
    }
    return frame;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3): only WAIT and WAKE exist,
 * and WAIT has no timeout, so `timeout` is never read. FUTEX_PRIVATE_FLAG is
 * accepted because the address check is always against one address space.
 * WAIT compares the word once; a different value means the wake happened
 * first and is reported as -EAGAIN rather than sleeping through it. A real
 * wait returns the next task's frame from the scheduler, so the caller's
 * frame is never resumed twice. */
static regs64_t *sys64_futex(regs64_t *frame) {
    u64 uaddr = frame->rdi;
    u64 op = frame->rsi;
    u32 word;

    if (op & ~(u64)(K64_FUTEX_OP_MASK | K64_FUTEX_PRIVATE_FLAG)) {
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }

    switch (op & K64_FUTEX_OP_MASK) {
    case K64_FUTEX_WAKE:
        frame->rax = (u64)sched64_futex_wake(sched64_current(), uaddr,
                                             (int)frame->rdx);
        return frame;

    case K64_FUTEX_WAIT:
        if (!copy_from_user64(current_space(), &word, uaddr, 4)) {
            frame->rax = (u64)0 - SYS64_EFAULT;
            return frame;
        }
        if (word != (u32)frame->rdx) {
            frame->rax = (u64)0 - SYS64_EAGAIN;
            return frame;
        }
        return sched64_futex_wait(frame, uaddr);

    default:
        frame->rax = (u64)0 - SYS64_EINVAL;
        return frame;
    }
}

/* wait4(pid, status, options): block until one child is DONE, write its
 * status, and reap it. Waiters sleep with interrupts enabled: the timer
 * preempts them, the child runs to completion, and the loop sees DONE on
 * the way back in. WNOHANG makes it a poll instead. */
static u64 sys64_wait4(u64 pid, u64 status, u64 options) {
    int id = sched64_current();
    u64 space = current_space();
    u64 code = 0;
    s64 want = (s64)pid;
    int result;

    if (options & ~(u64)K64_WNOHANG) return (u64)0 - SYS64_EINVAL;
    if (want < -1 || want == 0) return (u64)0 - SYS64_EINVAL;
    if (status != 0 && !user_page64(space, status, 0, 1))
        return (u64)0 - SYS64_EFAULT;

    for (;;) {
        result = task64_reap(id, (int)want, &code);
        if (result >= 0) break;
        if (result == -(int)SYS64_ECHILD) return (u64)0 - SYS64_ECHILD;
        if (options & K64_WNOHANG) return 0;
        sti64();
        cpu_halt64();
        cli64();
    }

    if (status != 0) {
        u32 value = (u32)code;

        if (!copy_to_user64(space, status, &value, 4))
            return (u64)0 - SYS64_EFAULT;
    }
    return (u64)result;
}

/* kill(pid, signal): pid 1 and up is a task index + 1. Only the default
 * action (terminate) exists; the caller's own pid is handled in the
 * dispatcher because it must not resume its frame. */
static u64 sys64_kill(u64 pid, u64 signal) {
    s64 want = (s64)pid;

    if (want <= 0) return (u64)0 - SYS64_EINVAL;
    return (u64)task64_signal((int)want - 1, (u32)signal);
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

/* nanosleep(request, remaining): sleep for at least the requested interval.
 * The interval is rounded up to whole timer ticks and the task waits with
 * interrupts enabled, exactly like wait4's loop, so other READY tasks run
 * while it waits; there is no sleep queue yet, so the waiter stays the
 * current task and is simply resumed. `remaining` is never written because
 * no signal can interrupt the sleep. */
static u64 sys64_nanosleep(u64 request, u64 remaining) {
    u64 ts[2], hz, ticks, wait, deadline;

    (void)remaining;

    if (!copy_from_user64(current_space(), ts, request, SYS64_TIMESPEC))
        return (u64)0 - SYS64_EFAULT;
    if ((s64)ts[0] < 0 || (s64)ts[1] < 0 || ts[1] >= 1000000000ull)
        return (u64)0 - SYS64_EINVAL;
    if (ts[0] > 1000000ull) return (u64)0 - SYS64_EINVAL;
    hz = timer64_hz();
    if (hz == 0) return (u64)0 - SYS64_EINVAL;

    /* Round up: sleeping less than asked would be a lie. */
    wait = ts[0] * hz + (ts[1] * hz + 999999999ull) / 1000000000ull;
    ticks = timer64_ticks();
    deadline = ticks + wait;
    while (timer64_ticks() < deadline) {
        sti64();
        cpu_halt64();
        cli64();
    }
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
    case K64_SYS_READ:
        result = sys64_read(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_OPEN:
        result = sys64_open(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_CLOSE:
        result = sys64_close(frame->rdi);
        break;

    case K64_SYS_LSEEK:
        result = sys64_lseek(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_DUP:
        result = sys64_dup(frame->rdi);
        break;

    case K64_SYS_DUP2:
        result = sys64_dup2(frame->rdi, frame->rsi);
        break;

    case K64_SYS_NANOSLEEP:
        result = sys64_nanosleep(frame->rdi, frame->rsi);
        break;

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

    case K64_SYS_MKDIR:
        result = sys64_mkdir(frame->rdi, frame->rsi);
        break;

    case K64_SYS_FORK:
        return sched64_fork(frame);

    case K64_SYS_CLONE:
        return sys64_clone(frame);

    case K64_SYS_FUTEX:
        return sys64_futex(frame);

    case K64_SYS_EXECVE:
        return sys64_execve(frame);

    case K64_SYS_WAIT4:
        result = sys64_wait4(frame->rdi, frame->rsi, frame->rdx);
        break;

    case K64_SYS_KILL:
        /* kill() of the caller ends it instead of returning: the frame the
         * handler was given must never be resumed. */
        if (frame->rdi == sys64_getpid() && frame->rsi != 0)
            return sched64_terminate(frame, (u32)frame->rsi);
        result = sys64_kill(frame->rdi, frame->rsi);
        break;

    case K64_SYS_GETPID:
        result = sys64_getpid();
        break;

    case K64_SYS_GETTID:
        result = sys64_gettid();
        break;

    case K64_SYS_GETPPID:
        result = sys64_getppid();
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
