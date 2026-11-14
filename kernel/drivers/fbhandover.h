/* The VBE handover record both ports read at boot.
 *
 * boot/stage2.asm uses the VBE BIOS calls (INT 10h AX=4F00h/4F01h/4F02h) to
 * pick a linear-framebuffer mode and writes this record at physical 0x6FC0
 * before it loads either kernel image. The two console glue files -- the
 * 32-bit kernel/drivers/framebuffer.c and the 64-bit fb64.c -- validate the
 * same bytes and hand the same description to the shared renderer, which is
 * what keeps the two screens identical.
 */
#ifndef HAKU_FBHANDOVER_H
#define HAKU_FBHANDOVER_H

#include "cpu.h"

/* Physical address and magic ('HFB1'). */
#define FB_HANDOVER_PHYS   0x6FC0u
#define FB_HANDOVER_MAGIC  0x31424648u

/* The record, in the order stage 2 writes it: dwords, then the channel masks
 * as size | position << 8, then the mode number and the aperture size. */
typedef struct {
    uint32_t magic;
    uint32_t physical_base;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    uint16_t mode;
    uint32_t size;        /* pitch * height */
} fb_handover_t;

#endif
