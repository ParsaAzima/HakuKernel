/* Haku x86-64 initramfs (cpio "newc" archive).
 *
 * The boot archive is data, not kernel code: the kernel reads /disk/INITRD,
 * unpacks it into the RAM filesystem mounted at "/", and every entry is
 * created through the ordinary descriptor path (open/write/close/mkdir), so
 * the same name-length, space and permission checks apply to the archive as
 * to a user program.
 */
#ifndef HAKU_INITRAMFS64_H
#define HAKU_INITRAMFS64_H

#include "haku64.h"

/* Unpack /disk/INITRD into the RAM root. Falls back to mounting the FAT12
 * volume at "/" when the archive is absent. Returns 1 on success. */
int initramfs64_init(void);

#endif
