/* Haku framebuffer (32-bit): discover the VBE handover and map the aperture.
 *
 * The rendering itself is shared with the 64-bit port and lives in
 * drivers/fbcon.c; the record stage 2 leaves behind is described in
 * drivers/fbhandover.h. When no usable linear framebuffer is found the
 * console keeps the VGA text buffer as its backend.
 */
#ifndef HAKU_FRAMEBUFFER_H
#define HAKU_FRAMEBUFFER_H

/* Map the aperture and start the shared console. Returns 1 when the
 * framebuffer console is live, 0 when the VGA text fallback stays active. */
int framebuffer_init(void);

#endif
