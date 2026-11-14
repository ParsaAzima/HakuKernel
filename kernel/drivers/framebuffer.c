/* Haku framebuffer glue (32-bit): find the VBE mode, map it, hand it over.
 *
 * stage 2 discovers a linear-framebuffer mode with the VBE BIOS calls and
 * leaves a handover record at physical 0x6FC0 (see boot/stage2.asm). This
 * file validates that record, keeps the aperture out of the frame pool, maps
 * it into the supervisor window and passes the result to the shared renderer
 * in drivers/fbcon.c. Nothing here draws pixels: the 64-bit port uses the
 * same renderer with a different mapping.
 */
#include "framebuffer.h"
#include "fbcon.h"
#include "fbhandover.h"
#include "font.h"
#include "frame.h"
#include "klog.h"
#include "paging.h"

int framebuffer_init(void) {
    const fb_handover_t *handover =
        (const fb_handover_t *)(uint32_t)FB_HANDOVER_PHYS;
    fbcon_mode_t screen;
    uint32_t virtual_base;

    if (fbcon_active()) return 1;
    if (handover->magic != FB_HANDOVER_MAGIC) return 0;
    if (handover->bpp != 32u && handover->bpp != 24u) return 0;
    if (!handover->width || !handover->height) return 0;
    if (handover->width > 4096u || handover->height > 4096u) return 0;
    if (!handover->physical_base || !handover->pitch) return 0;
    /* The window is one page-directory entry: 4 MiB of aperture. */
    if (!handover->size || handover->size > PAGING_FB_WINDOW_BYTES) return 0;

    /* The aperture must not overlap the tracked frame pool. */
    frame_reserve_range(handover->physical_base,
                        handover->physical_base + handover->size);

    virtual_base = paging_map_framebuffer(handover->physical_base,
                                           handover->size);
    if (!virtual_base) return 0;

    screen.base = (void *)virtual_base;
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

    if (!fbcon_init(&screen)) return 0;

    klog_prefix();
    klog_write("fb: ");
    klog_dec(fbcon_width());
    klog_write("x");
    klog_dec(fbcon_height());
    klog_write(" bpp=");
    klog_dec(handover->bpp);
    klog_write(" font=");
    klog_write(fbcon_font_name());
    klog_write(" grid=");
    klog_dec(fbcon_columns());
    klog_write("x");
    klog_dec(fbcon_rows());
    klog_write("\n");
    return 1;
}
