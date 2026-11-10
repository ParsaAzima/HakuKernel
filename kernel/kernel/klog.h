/* Haku klog: Linux-dmesg-style boot log with severity levels.
 *
 * Every line opens with "[    S.UUUUUU] " where the stamp is seconds since
 * boot from the 100 Hz PIT (0.000000 until pit_init runs, exactly like
 * early Linux messages before the clocksource is up). All output goes to
 * both the active display and COM1, so the screen and the headless serial
 * log agree.
 *
 * Plain klog_write()/klog_prefix() are the unconditional marker path used
 * by boot self-tests. klog_log()/klog_begin() add a severity tag and honour
 * the configured threshold (INFO by default); panic output bypasses the
 * threshold and is always printed.
 *
 * The headless markers (HAKU-BOOT-OK, IDT-OK, ...) live inside these lines
 * as subsystem tags; build.py greps for them as substrings.
 */
#ifndef HAKU_KLOG_H
#define HAKU_KLOG_H

#include "cpu.h"

typedef enum {
    KLOG_DEBUG = 0,
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR,
    KLOG_CRIT,
    KLOG_NONE /* threshold only: silence all leveled output */
} klog_level_t;

void klog_prefix(void);                      /* "[    0.000000] " */
void klog_putc(char c);                      /* VGA + serial */
void klog_write(const char *s);              /* VGA + serial */
void klog_dec(uint32_t v);                   /* decimal, both */
void klog_hex(uint32_t v);                   /* 0x + 8 digits, both */
void klog_hex_raw(uint32_t v);               /* 8 digits, no prefix, both */

/* Leveled logging. A message is printed when its level is at or above the
 * threshold; klog_begin returns 0 when it was suppressed. */
void klog_set_level(klog_level_t level);
/* 0 sends subsequent lines to COM1 only; 1 (the default) mirrors them to
 * the active display as well. */
void klog_mirror_display(int enabled);
klog_level_t klog_get_level(void);
const char *klog_level_name(klog_level_t level);
int klog_level_enabled(klog_level_t level);
int klog_begin(klog_level_t level);          /* stamp + "LEVEL: " */
void klog_log(klog_level_t level, const char *message);
/* Verify DEBUG is filtered at INFO and enabled after lowering the level. */
void klog_self_test(void);

#endif
