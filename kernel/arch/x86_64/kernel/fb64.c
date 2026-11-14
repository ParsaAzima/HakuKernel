/* Haku x86-64 framebuffer glue: read the VBE handover, map it, hand it over.
 *
 * The mode discovery happens in boot/stage2.asm, which the two images share:
 * it leaves a record at physical 0x6FC0 (drivers/fbhandover.h). This file
 * owns the 64-bit half of using it -- keep the aperture out of the frame pool,
 * map it into K64_FB_VIRT and pass the description to the shared renderer in
 * drivers/fbcon.c. Nothing here draws: the 32-bit port runs the same renderer
 * over its own window, so both consoles are the same pixels.
 *
 * The record is reached through the physmap, not the identity map, because
 * only the first 1 GiB is identity mapped and the aperture itself is usually
 * placed above it (QEMU puts it at 0xFD000000).
 */
#include "haku64.h"
#include "fbcon.h"
#include "fbhandover.h"
#include "font.h"

/* A mode wider or taller than this is not something stage 2 selects, and the
 * renderer would need a shadow grid it does not have. */
#define FB64_MAX_WIDTH  4096u
#define FB64_MAX_HEIGHT 4096u

int fb64_init(void) {
    const fb_handover_t *handover =
        (const fb_handover_t *)phys_to_virt64(FB_HANDOVER_PHYS);
    fbcon_mode_t screen;

    if (fbcon_active()) return 1;
    if (handover->magic != FB_HANDOVER_MAGIC) return 0;
    if (handover->bpp != 32u && handover->bpp != 24u) return 0;
    if (!handover->width || !handover->height) return 0;
    if (handover->width > FB64_MAX_WIDTH ||
        handover->height > FB64_MAX_HEIGHT) return 0;
    if (!handover->physical_base || !handover->pitch) return 0;
    if (!handover->size || handover->size > K64_FB_WINDOW_BYTES) return 0;

    /* The aperture must not be handed out as a page frame: on firmware that
     * places it inside RAM, a task writing there would paint over the
     * console. */
    frame64_reserve_range(handover->physical_base,
                          handover->physical_base + handover->size);

    if (!paging64_map_framebuffer(handover->physical_base, handover->size))
        return 0;

    screen.base = (void *)K64_FB_VIRT;
    screen.pitch = handover->pitch;
    screen.width = handover->width;
    screen.height = handover->height;
    screen.bytes_per_pixel = handover->bpp / 8u;
    screen.red_size = handover->red & 0xFFu;
    screen.red_pos = (handover->red >> 8) & 0xFFu;
    screen.green_size = handover->green & 0xFFu;
    screen.green_pos = (handover->green >> 8) & 0xFFu;
    screen.blue_size = handover->blue & 0xFFu;
    screen.blue_pos = (handover->blue >> 8) & 0xFFu;
    screen.font = &terminus_8x16;

    return fbcon_init(&screen);
}
