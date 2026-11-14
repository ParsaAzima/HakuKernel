/* Haku x86-64 PS/2 keyboard: IRQ1, set-1 scancodes, a small queue.
 *
 * Set-1 scancodes are what the BIOS leaves the 8042 in after POST. Bit 7 is
 * set on "break" (key-up), the rest is "make" (key-down). Shift and caps-lock
 * are tracked, translation goes through two US-layout tables into the ring
 * that console read() drains.
 *
 * The 32-bit port hands each character straight to the terminal layer because
 * it owns a line editor. The 64-bit console has no terminal layer yet, so the
 * queue is the interface: read() pops, and when it is empty the caller parks
 * on K64_CONSOLE_WAIT_ADDR with -EAGAIN. IRQ1 then pushes and wakes, so an
 * idle console costs no CPU.
 *
 * Everything that touches the ring or the modifier state takes the same
 * interrupt-safe lock, because IRQ1 and a system call both reach it. The ring
 * drops the newest byte when full: losing one keystroke is better than
 * overwriting one the reader has not seen yet.
 */
#include "haku64.h"
#include "sync.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_STATUS_OBF  0x01u   /* output buffer full */

static const char scancode_map[0x40] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',   /* 0x00-0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  0x08, '\t',  /* 0x08-0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',   /* 0x10-0x17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',   /* 0x18-0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',   /* 0x20-0x27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',   /* 0x28-0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',   /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,     /* 0x38-0x3F */
};

static const char scancode_map_shift[0x40] = {
    0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',   /* 0x00-0x07 */
    '&',  '*',  '(',  ')',  '_',  '+',  0x08, '\t',  /* 0x08-0x0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',   /* 0x10-0x17 */
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',   /* 0x18-0x1F */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',   /* 0x20-0x27 */
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',   /* 0x28-0x2F */
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',   /* 0x30-0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,     /* 0x38-0x3F */
};

static u8 queue[K64_KBD_QUEUE];
static u32 queue_head;                 /* next byte to hand out */
static u32 queue_tail;                 /* next free slot */
static volatile int shift_down;
static volatile int caps_lock;
static spinlock_t kbd_lock = SPINLOCK_INIT;

/* Translate one scancode. Only the caller's lock makes this safe: it touches
 * the modifier state. Returns 1 when a byte was produced. */
static int kbd_decode(u8 sc, char *out) {
    int break_code;
    u8 index;

    /* Extended prefix (arrows, keypad, right ctrl/alt). The 32-bit driver
     * ignores it too; the four arrow keys therefore do nothing yet. */
    if (sc == 0xE0) return 0;

    break_code = (sc & 0x80) != 0;
    index = (u8)(sc & 0x7F);

    switch (index) {
    case 0x2A: /* left shift  */
    case 0x36: /* right shift */
        shift_down = !break_code;
        return 0;
    case 0x3A: /* caps lock toggles on the make code only */
        if (!break_code) caps_lock = !caps_lock;
        return 0;
    default:
        break;
    }
    if (break_code) return 0;
    /* F-keys, the numeric keypad and the media keys live above 0x40 and have
     * no US-layout character; bound the index rather than reading past the
     * table. */
    if (index >= 0x40) return 0;

    *out = (shift_down ^ caps_lock) ? scancode_map_shift[index]
                                    : scancode_map[index];
    return *out != 0;
}

/* Queue one decoded byte. The lock is interrupt-safe: IRQ1 and a system call
 * can both be here. */
static void kbd_push(char c) {
    u32 next;
    irqflags_t flags = spin_lock_irqsave(&kbd_lock);

    next = (queue_tail + 1u) % K64_KBD_QUEUE;
    if (next != queue_head) {
        queue[queue_tail] = (u8)c;
        queue_tail = next;
    }
    spin_unlock_irqrestore(&kbd_lock, flags);
}

int kbd64_read(void) {
    irqflags_t flags = spin_lock_irqsave(&kbd_lock);
    int ch = -1;

    if (queue_head != queue_tail) {
        ch = (int)queue[queue_head];
        queue_head = (queue_head + 1u) % K64_KBD_QUEUE;
    }
    spin_unlock_irqrestore(&kbd_lock, flags);
    return ch;
}

u32 kbd64_pending(void) {
    irqflags_t flags = spin_lock_irqsave(&kbd_lock);
    u32 count = (queue_tail + K64_KBD_QUEUE - queue_head) % K64_KBD_QUEUE;

    spin_unlock_irqrestore(&kbd_lock, flags);
    return count;
}

/* IRQ1: one byte is waiting in the controller's output buffer. */
void kbd64_irq(void) {
    char ch;

    if (kbd_decode(inb64(KBD_DATA_PORT), &ch)) {
        kbd_push(ch);
        /* Wake a reader parked on the console. The task id is only run
         * through the "is this a valid slot" check; the match is on the
         * address, and the woken task re-checks the queue before trusting
         * the wakeup. */
        sched64_futex_wake(1, K64_CONSOLE_WAIT_ADDR, 1);
    }
}

/* The controller keeps a byte in the output buffer until someone reads it; an
 * unread byte left over from firmware keeps OBF set and the controller then
 * raises no further IRQ1. Drain the (tiny, bounded) backlog, then unmask the
 * line. Interrupts are still off here, so the first real key is queued after
 * the self-test has proved the decoder. */
void kbd64_init(void) {
    for (u32 i = 0; i < 16u; i++) {
        if (!(inb64(KBD_STATUS_PORT) & KBD_STATUS_OBF)) break;
        (void)inb64(KBD_DATA_PORT);
    }
    pic64_clear_mask(1);
}

/* Decode-only self-test with synthesised scancodes, so the check does not
 * depend on the headless run typing anything: shift+a gives 'A', shift up
 * gives 'a', Enter and Backspace are queued, the 0xE0 prefix and a keypad
 * scancode produce nothing, and the queue drains in order. */
int kbd64_self_test(void) {
    static const u8 scancodes[] = {
        0x2A, 0x1E, 0xAA, 0x1E, 0x1C, 0x0E, 0xE0, 0x47, 0x3B
    };
    static const char expected[] = { 'A', 'a', '\n', '\b' };
    int produced = 0;
    int ok = 1;
    char ch;

    while (kbd64_read() >= 0) { }
    for (u32 i = 0; i < sizeof(scancodes); i++) {
        if (!kbd_decode(scancodes[i], &ch)) continue;
        kbd_push(ch);
        produced++;
    }
    if (produced != (int)sizeof(expected)) ok = 0;
    for (u32 i = 0; i < sizeof(expected) && ok; i++)
        if (kbd64_read() != (int)(u8)expected[i]) ok = 0;
    if (kbd64_read() >= 0) ok = 0;

    /* A fresh scan starts with neither modifier held. */
    shift_down = 0;
    caps_lock = 0;

    if (!ok) return 0;
    k64_puts("HAKU64-KBD-OK: PS/2 set-1 decode and queue, irq1=");
    k64_dec(K64_IRQ_BASE + 1u);
    k64_puts("\r\n");
    return 1;
}
