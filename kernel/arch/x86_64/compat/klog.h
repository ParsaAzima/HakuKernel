/* klog.h for the shared 32-bit ATA/FAT12 sources.
 *
 * The 32-bit kernel has a timestamped, severity-filtered log; the 64-bit
 * port writes the same messages straight to COM1 through k64_puts. Keeping
 * the names means ata.c and fat12.c do not need a second copy, and the
 * existing marker strings (ATA-SCAN-OK, FAT12-FAIL, ...) still show up in
 * the serial log unchanged.
 */
#ifndef HAKU64_COMPAT_KLOG_H
#define HAKU64_COMPAT_KLOG_H

#include "cpu.h"

typedef enum {
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR,
    KLOG_CRIT,
    KLOG_NONE
} klog_level_t;

void klog_prefix(void);
void klog_write(const char *s);
void klog_dec(uint32_t v);
void klog_hex(uint32_t v);
int klog_begin(klog_level_t level);
void klog_log(klog_level_t level, const char *message);

#endif
