/* Haku x86-64 ELF64 loader.
 *
 * The 32-bit loader in kernel/arch/x86/elf/elf.c sets the conventions this
 * one follows: validate the whole image before mapping anything, name the
 * first reason a file was refused (`elf64_last_error`), accept only static
 * ET_EXEC, require readable segments that are not both writable and
 * executable, and refuse segments whose offset does not line up with their
 * address. What changes is the page-table API: the 64-bit space is a PML4
 * created by paging64_create_space, and every leaf goes in through
 * paging64_map_space, which only accepts addresses in the user half.
 *
 * There is no AT_PHDR work here because there is no libc yet: the loaded
 * image gets a stack and its segments, nothing else. The struct still
 * records the writable page so the ring-3 self-test can check that the
 * program actually ran and wrote to its BSS.
 *
 * Frames mapped into a space are tracked by the loader for the duration of
 * a failed load. paging64_destroy_space frees tables only, so elf64_unload
 * walks the recorded image and stack ranges, translates each page back to
 * its frame, and frees those before dropping the tables.
 */
#include "haku64.h"

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

static const char *load_error = "no error";

const char *elf64_last_error(void) {
    return load_error;
}

static int reject(const char *why) {
    load_error = why;
    return 0;
}

static int in_image(u64 offset, u64 length, u64 image_size) {
    return offset <= image_size && length <= image_size - offset;
}

/* NULL when the segment can be mapped, otherwise the reason it cannot. */
static const char *segment_error(const elf64_phdr_t *phdr, u64 image_size,
                                 u64 *page_start, u64 *page_end) {
    u64 end;

    if (!(phdr->flags & PF_R))
        return "loadable segment is not readable";
    if ((phdr->flags & (PF_W | PF_X)) == (PF_W | PF_X))
        return "loadable segment is both writable and executable";
    if (phdr->memsz == 0 || phdr->filesz > phdr->memsz)
        return "loadable segment has an impossible size";
    if (phdr->vaddr < K64_USER_IMAGE_MIN || phdr->vaddr >= K64_USER_END ||
        phdr->memsz > K64_USER_END - phdr->vaddr)
        return "loadable segment is outside the user address window";
    end = phdr->vaddr + phdr->memsz;
    if (end > K64_USER_STACK_BOTTOM || end <= phdr->vaddr)
        return "loadable segment reaches into the user stack window";
    *page_start = phdr->vaddr & K64_PAGE_MASK;
    *page_end = (end + K64_PAGE_SIZE - 1) & K64_PAGE_MASK;
    if (*page_end <= *page_start)
        return "loadable segment maps no pages";
    if (!in_image(phdr->offset, phdr->filesz, image_size))
        return "loadable segment extends past the end of the file";
    if (phdr->align > 1u &&
        ((phdr->align & (phdr->align - 1u)) != 0 ||
         (phdr->vaddr & (phdr->align - 1u)) !=
         (phdr->offset & (phdr->align - 1u))))
        return "loadable segment offset is not aligned with its address";
    return 0;
}

/* Map one PT_LOAD segment: allocate a frame per page, zero it, copy the
 * file bytes that belong to that page, then map it read-execute or
 * read-write. On failure the frame allocated in this call is released; the
 * caller releases the ones already recorded. */
static int map_segment(const u8 *image, const elf64_phdr_t *phdr, u64 space,
                       u64 *frames, u32 *frame_count) {
    int writable = (phdr->flags & PF_W) != 0;
    int executable = (phdr->flags & PF_X) != 0;
    u64 page_start = phdr->vaddr & K64_PAGE_MASK;
    u64 page_end = (phdr->vaddr + phdr->memsz + K64_PAGE_SIZE - 1) &
                   K64_PAGE_MASK;

    for (u64 page = page_start; page < page_end; page += K64_PAGE_SIZE) {
        u64 frame = frame64_alloc();
        u64 consumed = page > phdr->vaddr ? page - phdr->vaddr : 0;
        u64 page_offset = page > phdr->vaddr ? 0 : phdr->vaddr - page;
        u64 amount = 0;

        if (!frame) return 0;
        memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
        if (consumed < phdr->filesz) {
            amount = phdr->filesz - consumed;
            if (amount > K64_PAGE_SIZE - page_offset)
                amount = K64_PAGE_SIZE - page_offset;
            memcpy((u8 *)phys_to_virt64(frame) + page_offset,
                   image + phdr->offset + consumed, amount);
        }
        if (*frame_count >= K64_ELF64_MAX_PAGES ||
            !paging64_map_space(space, page, frame, writable, 1, executable)) {
            frame64_free(frame);
            return 0;
        }
        frames[(*frame_count)++] = frame;
    }
    return 1;
}

int elf64_load(const u8 *image, u64 image_size, loaded_elf64_t *loaded) {
    elf64_header_t header;
    elf64_phdr_t phdrs[K64_ELF64_MAX_PHDRS];
    u64 starts[K64_ELF64_MAX_PHDRS];
    u64 ends[K64_ELF64_MAX_PHDRS];
    u64 frames[K64_ELF64_MAX_PAGES];
    u32 load_count = 0;
    u32 frame_count = 0;
    u32 page_total = 0;
    int entry_is_executable = 0;
    u64 writable_page = 0;
    u64 image_low = ~0ull;
    u64 image_high = 0;
    u64 space;

    if (!image || !loaded) return reject("no image buffer");
    if (image_size < sizeof(header))
        return reject("image is shorter than an ELF header");
    memcpy(&header, image, sizeof(header));
    if (header.ident[0] != 0x7Fu || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != ELFCLASS64 || header.ident[5] != ELFDATA2LSB ||
        header.ident[6] != 1u)
        return reject("image is not ELF64 little-endian");
    if (header.type != ET_EXEC || header.machine != EM_X86_64 ||
        header.version != 1u)
        return reject("image is not a 64-bit x86 executable");
    if (header.ehsize != sizeof(header) ||
        header.phentsize != sizeof(elf64_phdr_t) ||
        header.phnum == 0 || header.phnum > K64_ELF64_MAX_PHDRS)
        return reject("ELF header sizes or program-header count are not"
                      " usable");
    if (!in_image(header.phoff,
                  (u64)header.phnum * sizeof(elf64_phdr_t), image_size))
        return reject("program header table lies outside the image");

    memset(loaded, 0, sizeof(*loaded));
    for (u32 i = 0; i < header.phnum; i++) {
        elf64_phdr_t phdr;
        const char *why;

        memcpy(&phdr, image + header.phoff + (u64)i * sizeof(phdr),
               sizeof(phdr));
        if (phdr.type == PT_INTERP || phdr.type == PT_DYNAMIC)
            return reject("image needs a dynamic loader");
        if (phdr.type != PT_LOAD) continue;

        why = segment_error(&phdr, image_size, &starts[load_count],
                            &ends[load_count]);
        if (why) return reject(why);
        for (u32 j = 0; j < load_count; j++)
            if (starts[load_count] < ends[j] && starts[j] < ends[load_count])
                return reject("loadable segments overlap in memory");
        if ((phdr.flags & PF_X) && header.entry >= phdr.vaddr &&
            header.entry - phdr.vaddr < phdr.memsz)
            entry_is_executable = 1;
        if ((phdr.flags & PF_W) && !writable_page)
            writable_page = starts[load_count];
        page_total += (u32)((ends[load_count] - starts[load_count]) /
                            K64_PAGE_SIZE);
        if (page_total > K64_ELF64_MAX_PAGES)
            return reject("image needs too many pages");
        phdrs[load_count++] = phdr;
    }
    if (load_count == 0 || !entry_is_executable)
        return reject("entry point is not inside an executable segment");
    if (page_total + K64_USER_STACK_PAGES > K64_ELF64_MAX_PAGES)
        return reject("image leaves no room for the user stack");

    space = paging64_create_space();
    if (!space) return reject("not enough memory to map the image");

    for (u32 i = 0; i < load_count; i++) {
        if (!map_segment(image, &phdrs[i], space, frames, &frame_count))
            goto fail;
        if (starts[i] < image_low) image_low = starts[i];
        if (ends[i] > image_high) image_high = ends[i];
    }
    for (u64 page = K64_USER_STACK_BOTTOM; page < K64_USER_STACK_TOP;
         page += K64_PAGE_SIZE) {
        u64 frame = frame64_alloc();
        if (!frame) goto fail;
        memset(phys_to_virt64(frame), 0, K64_PAGE_SIZE);
        if (frame_count >= K64_ELF64_MAX_PAGES ||
            !paging64_map_space(space, page, frame, 1, 1, 0)) {
            frame64_free(frame);
            goto fail;
        }
        frames[frame_count++] = frame;
    }

    loaded->space = space;
    loaded->entry = header.entry;
    loaded->stack_top = K64_USER_STACK_TOP;
    loaded->stack_bottom = K64_USER_STACK_BOTTOM;
    loaded->image_low = image_low;
    loaded->image_high = image_high;
    loaded->writable = writable_page;
    loaded->pages = frame_count;
    return 1;

fail:
    for (u32 i = 0; i < frame_count; i++)
        frame64_free(frames[i]);
    paging64_destroy_space(space);
    return reject("not enough memory to map the image");
}

void elf64_unload(loaded_elf64_t *loaded) {
    if (!loaded || !loaded->space) return;

    for (u64 page = loaded->image_low; page < loaded->image_high;
         page += K64_PAGE_SIZE) {
        u64 phys = paging64_translate_space(loaded->space, page);
        if (phys) frame64_user_release(phys & K64_PAGE_MASK);
    }
    for (u64 page = loaded->stack_bottom; page < loaded->stack_top;
         page += K64_PAGE_SIZE) {
        u64 phys = paging64_translate_space(loaded->space, page);
        if (phys) frame64_user_release(phys & K64_PAGE_MASK);
    }
    paging64_destroy_space(loaded->space);
    memset(loaded, 0, sizeof(*loaded));
}
