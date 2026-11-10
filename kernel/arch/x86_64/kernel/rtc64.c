/* Haku x86-64 real-time clock.
 *
 * CLOCK_REALTIME needs a wall-clock anchor, and the only one this port can
 * read without a filesystem is the CMOS RTC behind ports 0x70/0x71. The
 * kernel reads it once during boot and keeps the resulting Unix epoch;
 * clock_gettime adds the monotonic tick count to it.
 *
 * The chip reports BCD or binary fields and 12- or 24-hour hours depending
 * on status register B, and an update may be in progress while it is read.
 * The reader waits for the update-in-progress bit to clear, takes one
 * snapshot, and rejects anything outside sane calendar ranges instead of
 * inventing a time. QEMU (and every PC this port targets) provides a
 * century register at 0x32; when it is missing the year is derived from
 * the two-digit value.
 */
#include "haku64.h"

#define CMOS_ADDR 0x70u
#define CMOS_DATA 0x71u

#define CMOS_SECONDS  0x00u
#define CMOS_MINUTES  0x02u
#define CMOS_HOURS    0x04u
#define CMOS_DAY      0x07u
#define CMOS_MONTH    0x08u
#define CMOS_YEAR     0x09u
#define CMOS_STATUS_A 0x0Au
#define CMOS_STATUS_B 0x0Bu
#define CMOS_CENTURY  0x32u

static u64 boot_epoch;
static u64 boot_ticks;

static u8 cmos_read(u8 reg) {
    /* Bit 7 disables NMI while the index is latched; the PIC owns the only
     * interrupt this port enables, so the bit is harmless either way. */
    outb64(CMOS_ADDR, (u8)(0x80u | reg));
    return inb64(CMOS_DATA);
}

static int cmos_wait_ready(void) {
    for (u32 i = 0; i < 1000000u; i++)
        if (!(cmos_read(CMOS_STATUS_A) & 0x80u)) return 1;
    return 0;
}

/* BCD byte to 0..99, or -1 when the nibbles are not a valid decimal. */
static int cmos_bcd(u8 value) {
    if ((value & 0x0Fu) > 9u || (value >> 4) > 9u) return -1;
    return (int)(value & 0x0Fu) + (int)(value >> 4) * 10;
}

static int cmos_field(u8 value, int bcd) {
    return bcd ? cmos_bcd(value) : (int)value;
}

/* Days since 1970-01-01 for a proleptic Gregorian date. This is Howard
 * Hinnant's civil-from-days inverse; it avoids a table and handles leap
 * years with the 400-year era. */
static s64 days_from_civil(int year, int month, int day) {
    s64 y = year;
    u32 m = (u32)month;
    u32 d = (u32)day;
    s64 era;
    u32 yoe, doy, doe;

    y -= m <= 2u;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (u32)(y - era * 400);
    doy = (153u * (m + (m > 2u ? (u32)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (s64)doe - 719468;
}

int rtc64_init(void) {
    u8 status_b, sec_raw, min_raw, hour_raw, day_raw, mon_raw, year_raw;
    u8 century_raw;
    int bcd, hour24, sec, min, hour, day, mon, year, century;

    boot_epoch = 0;
    if (!cmos_wait_ready()) return 0;

    status_b = cmos_read(CMOS_STATUS_B);
    bcd = (status_b & 0x04u) == 0;
    hour24 = (status_b & 0x02u) != 0;

    sec_raw = cmos_read(CMOS_SECONDS);
    min_raw = cmos_read(CMOS_MINUTES);
    hour_raw = cmos_read(CMOS_HOURS);
    day_raw = cmos_read(CMOS_DAY);
    mon_raw = cmos_read(CMOS_MONTH);
    year_raw = cmos_read(CMOS_YEAR);
    century_raw = cmos_read(CMOS_CENTURY);

    sec = cmos_field((u8)(sec_raw & 0x7Fu), bcd);
    min = cmos_field((u8)(min_raw & 0x7Fu), bcd);
    hour = cmos_field((u8)(hour_raw & 0x7Fu), bcd);
    day = cmos_field(day_raw, bcd);
    mon = cmos_field(mon_raw, bcd);
    year = cmos_field(year_raw, bcd);
    century = cmos_field(century_raw, bcd);

    if (!hour24) {
        if (hour == 12) hour = 0;
        if (hour_raw & 0x80u) hour += 12;
    }
    if (century < 19 || century > 99) century = year < 70 ? 20 : 19;
    year += century * 100;

    if (sec < 0 || sec > 59 || min < 0 || min > 59 ||
        hour < 0 || hour > 23 || day < 1 || day > 31 ||
        mon < 1 || mon > 12 || year < 1970 || year > 2200)
        return 0;

    s64 days = days_from_civil(year, mon, day);
    if (days < 0) return 0;
    boot_epoch = (u64)days * 86400ull + (u64)hour * 3600ull +
                 (u64)min * 60ull + (u64)sec;
    /* The snapshot is taken after the timer is running, so the elapsed
     * time is measured from here rather than from tick zero. */
    boot_ticks = timer64_ticks();
    return boot_epoch != 0;
}

u64 rtc64_epoch(void) {
    return boot_epoch;
}

/* Wall-clock seconds at `ticks` of a `hz` timer. Returns 0 when the RTC
 * could not be read, which is the caller's signal to fall back to
 * CLOCK_MONOTONIC. */
u64 rtc64_realtime(u64 ticks, u64 hz) {
    if (boot_epoch == 0 || hz == 0) return 0;
    if (ticks < boot_ticks) ticks = boot_ticks;
    return boot_epoch + (ticks - boot_ticks) / hz;
}
