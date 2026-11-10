/* Haku terminal: the text-I/O abstraction between userspace, the keyboard
 * driver, and the display backends.
 *
 * Output goes to the active display (display.h) and is mirrored to COM1 with
 * CRLF line endings. Input is pushed here by the keyboard IRQ; the terminal
 * owns the byte ring and the wait queue, so neither the PS/2 driver nor the
 * syscall layer knows about the other.
 */
#ifndef HAKU_TERMINAL_H
#define HAKU_TERMINAL_H

#include "cpu.h"

#define TERMINAL_BUF_SIZE 128u

/* Serial, display, and input state. Call once from the boot path. */
void terminal_init(void);

/* Output. Safe from task and interrupt context. */
void terminal_putc(char c);
void terminal_write(const char *s);
/* COM1 only. The timer heartbeat uses it once userland owns the display:
 * the serial boot log keeps its timeline while the screen stays the
 * shell's. */
void terminal_write_serial(const char *s);
void terminal_corner(const char *s);

/* Console device operations (VFS fd 0/1/2). read() returns after a newline
 * or count bytes; write() mirrors to the display and COM1. */
int32_t terminal_read(void *buffer, uint32_t count, uint32_t *out_count);
int32_t terminal_write_bytes(const void *buffer, uint32_t count,
                             uint32_t *out_count);

/* Input producer: called from the keyboard IRQ handler. */
void terminal_input_push(char c);
/* Non-blocking consume; -1 when the ring is empty. */
int terminal_input_poll(void);
/* 1 when a read would not block (poll/select readiness). */
int terminal_input_available(void);
/* Block until a character is available. */
char terminal_getchar(void);

/* Boot check for the control-code filter: write the cursor-position query,
 * then verify the reply landed on the input ring. Returns 1 on success. */
int terminal_self_test(void);

#endif
