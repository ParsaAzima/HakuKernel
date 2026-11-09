#include "ata.h"
#include "klog.h"
#include "sync.h"

#define ATA_PRIMARY_IO       0x1F0u
#define ATA_PRIMARY_CTRL     0x3F6u
#define ATA_SECONDARY_IO     0x170u
#define ATA_SECONDARY_CTRL   0x376u

#define ATA_REG_DATA         0u
#define ATA_REG_ERROR        1u
#define ATA_REG_SECTOR_CNT   2u
#define ATA_REG_LBA_LOW      3u
#define ATA_REG_LBA_MID      4u
#define ATA_REG_LBA_HIGH     5u
#define ATA_REG_DRIVE        6u
#define ATA_REG_STATUS       7u
#define ATA_REG_COMMAND      7u

#define ATA_CMD_READ         0x20u
#define ATA_CMD_WRITE        0x30u
#define ATA_CMD_FLUSH        0xE7u
#define ATA_CMD_READ_EXT     0x24u
#define ATA_CMD_WRITE_EXT    0x34u
#define ATA_CMD_FLUSH_EXT    0xEAu
#define ATA_CMD_IDENTIFY     0xECu

#define ATA_SR_ERR           0x01u
#define ATA_SR_DRQ           0x08u
#define ATA_SR_DF            0x20u
#define ATA_SR_BSY           0x80u

#define ATA_CTRL_NIEN        0x02u
#define ATA_CTRL_SRST        0x04u

#define ATA_POLL_LIMIT       4000000u
#define ATA_RESET_LIMIT      1000000u
#define ATA_RETRIES          2u

#define ATA_WAIT_OK          0
#define ATA_WAIT_TIMEOUT     1
#define ATA_WAIT_ERROR       2
#define ATA_WAIT_ABSENT      3

#define ATA_DEVICE_MASTER    0u
#define ATA_DEVICE_SLAVE     1u
#define ATA_CHANNEL_PRIMARY  0u
#define ATA_CHANNEL_SECONDARY 1u
#define ATA_MAX_DEVICES      4u

/* Read cache. FAT12 walks the same metadata sectors over and over (both FAT
 * copies, the directory chain of every path component), so a small
 * direct-mapped cache removes most of the polling latency without adding a
 * background flusher: writes go straight to the disk and refresh the cached
 * copy, which keeps the cache transparent and crash-consistent with the
 * write-through model the filesystem already documents. */
#define ATA_CACHE_SLOTS      16u
#define ATA_SECTOR_SIZE      512u

typedef struct {
    uint32_t lba;
    uint8_t valid;
    uint8_t data[ATA_SECTOR_SIZE];
} ata_cache_slot_t;

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t present[2];
    uint8_t lba48[2];
    uint32_t sectors[2];
} ata_channel_t;

static ata_channel_t channels[2];
static int active_channel = -1;
static int active_device = -1;
static uint32_t active_sectors;
static uint32_t device_count;
static uint8_t device_slots[ATA_MAX_DEVICES];
static mutex_t ata_lock = MUTEX_INIT;
static uint8_t recovery_buffer_a[512];
static uint8_t recovery_buffer_b[512];
static ata_cache_slot_t sector_cache[ATA_CACHE_SLOTS];
static uint32_t cache_hits;
static uint32_t cache_misses;

static int ata_read_sectors_unlocked(uint32_t lba, uint8_t count, void *buffer);
static ata_channel_t *ata_active(void);

/* The cache belongs to whichever device is selected, so a device change
 * throws it away rather than risk serving another disk's sectors. */
static void cache_flush(void) {
    for (uint32_t i = 0; i < ATA_CACHE_SLOTS; i++)
        sector_cache[i].valid = 0;
}

static ata_cache_slot_t *cache_slot_for(uint32_t lba) {
    return &sector_cache[lba % ATA_CACHE_SLOTS];
}

/* One sector through the cache. The caller holds the disk mutex. */
static int cached_read_sector(uint32_t lba, void *buffer) {
    ata_cache_slot_t *slot = cache_slot_for(lba);
    if (slot->valid && slot->lba == lba) {
        memcpy(buffer, slot->data, ATA_SECTOR_SIZE);
        cache_hits++;
        return 1;
    }
    if (!ata_read_sectors_unlocked(lba, 1, slot->data)) {
        slot->valid = 0;
        return 0;
    }
    slot->lba = lba;
    slot->valid = 1;
    memcpy(buffer, slot->data, ATA_SECTOR_SIZE);
    cache_misses++;
    return 1;
}

/* A write owns the sector from now on: refresh the cached copy when it is
 * exactly one sector, otherwise drop every slot the write may have touched.
 * A failed write also drops them, because part of the transfer may have
 * reached the platter. */
static void cache_after_write(uint32_t lba, uint8_t count, const void *buffer,
                              int succeeded) {
    if (succeeded && count == 1u) {
        ata_cache_slot_t *slot = cache_slot_for(lba);
        slot->lba = lba;
        slot->valid = 1;
        memcpy(slot->data, buffer, ATA_SECTOR_SIZE);
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        cache_slot_for(lba + i)->valid = 0;
}

/* The bounds checks the public entry points share: an out-of-range request
 * must fail before it can reach the cache or the drive. */
static int ata_range_valid(uint32_t lba, uint32_t count) {
    ata_channel_t *channel = ata_active();
    if (!channel || !count || !active_sectors || lba >= active_sectors ||
        count > active_sectors - lba)
        return 0;
    if (!channel->lba48[active_device] &&
        (lba >= 0x10000000u || count > 0x10000000u - lba))
        return 0;
    return 1;
}

static const char *ata_device_name(int channel, int device) {
    if (channel == ATA_CHANNEL_PRIMARY)
        return device == ATA_DEVICE_MASTER ? "primary-master"
                                           : "primary-slave";
    return device == ATA_DEVICE_MASTER ? "secondary-master"
                                       : "secondary-slave";
}

static uint8_t ata_status(const ata_channel_t *channel) {
    return inb((uint16_t)(channel->io_base + ATA_REG_STATUS));
}

static void ata_delay_400ns(const ata_channel_t *channel) {
    (void)inb(channel->ctrl_base);
    (void)inb(channel->ctrl_base);
    (void)inb(channel->ctrl_base);
    (void)inb(channel->ctrl_base);
}

static void ata_reset_delay(void) {
    for (uint32_t i = 0; i < 20u; i++)
        io_wait();
}

static int ata_wait_not_busy(const ata_channel_t *channel, uint32_t limit) {
    for (uint32_t attempt = 0; attempt < limit; attempt++) {
        uint8_t status = ata_status(channel);
        if (status == 0 || status == 0xFFu)
            return ATA_WAIT_ABSENT;
        if (!(status & ATA_SR_BSY))
            return ATA_WAIT_OK;
    }
    return ATA_WAIT_TIMEOUT;
}

static int ata_wait_ready(const ata_channel_t *channel, uint8_t required,
                          uint32_t limit) {
    for (uint32_t attempt = 0; attempt < limit; attempt++) {
        uint8_t status = ata_status(channel);
        if (status == 0 || status == 0xFFu)
            return ATA_WAIT_ABSENT;
        if (status & ATA_SR_BSY)
            continue;
        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return ATA_WAIT_ERROR;
        if ((status & required) == required)
            return ATA_WAIT_OK;
    }
    return ATA_WAIT_TIMEOUT;
}

static const char *ata_wait_reason(int reason) {
    switch (reason) {
    case ATA_WAIT_TIMEOUT:
        return "timeout";
    case ATA_WAIT_ERROR:
        return "device-error";
    case ATA_WAIT_ABSENT:
        return "absent";
    default:
        return "unknown";
    }
}

static void ata_select(ata_channel_t *channel, uint8_t device, uint32_t lba,
                       int lba48) {
    uint8_t drive = lba48 ? 0x40u : 0xE0u;
    if (device == ATA_DEVICE_SLAVE)
        drive |= 0x10u;
    if (!lba48)
        drive |= (uint8_t)((lba >> 24) & 0x0Fu);
    outb((uint16_t)(channel->io_base + ATA_REG_DRIVE), drive);
    ata_delay_400ns(channel);
}

static void ata_software_reset(ata_channel_t *channel, uint8_t device) {
    outb(channel->ctrl_base, ATA_CTRL_NIEN | ATA_CTRL_SRST);
    ata_reset_delay();
    outb(channel->ctrl_base, ATA_CTRL_NIEN);
    ata_delay_400ns(channel);
    (void)ata_wait_not_busy(channel, ATA_RESET_LIMIT);
    ata_select(channel, device, 0, 0);
}

static void ata_program_lba(ata_channel_t *channel, uint8_t device,
                            uint32_t lba, int lba48, uint8_t command) {
    ata_select(channel, device, lba, lba48);
    if (lba48) {
        outb((uint16_t)(channel->io_base + ATA_REG_SECTOR_CNT), 0);
        outb((uint16_t)(channel->io_base + ATA_REG_LBA_LOW),
             (uint8_t)(lba >> 24));
        outb((uint16_t)(channel->io_base + ATA_REG_LBA_MID), 0);
        outb((uint16_t)(channel->io_base + ATA_REG_LBA_HIGH), 0);
    }
    outb((uint16_t)(channel->io_base + ATA_REG_SECTOR_CNT), 1);
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_LOW), (uint8_t)lba);
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_MID),
         (uint8_t)(lba >> 8));
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_HIGH),
         (uint8_t)(lba >> 16));
    outb((uint16_t)(channel->io_base + ATA_REG_COMMAND), command);
    ata_delay_400ns(channel);
}

static uint32_t ata_lba48_capacity(const uint16_t *words) {
    uint32_t low = (uint32_t)words[100] | ((uint32_t)words[101] << 16);
    uint32_t high = (uint32_t)words[102] | ((uint32_t)words[103] << 16);
    if (high)
        return 0xFFFFFFFFu;
    return low;
}

static int ata_identify(ata_channel_t *channel, uint8_t device,
                        uint16_t *words) {
    uint8_t status;
    int wait;

    ata_select(channel, device, 0, 0);
    status = ata_status(channel);
    if (status == 0 || status == 0xFFu)
        return 0;
    wait = ata_wait_not_busy(channel, ATA_POLL_LIMIT);
    if (wait == ATA_WAIT_ABSENT)
        return 0;
    if (wait != ATA_WAIT_OK)
        return -1;

    outb((uint16_t)(channel->io_base + ATA_REG_SECTOR_CNT), 0);
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_LOW), 0);
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_MID), 0);
    outb((uint16_t)(channel->io_base + ATA_REG_LBA_HIGH), 0);
    outb((uint16_t)(channel->io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    ata_delay_400ns(channel);

    status = ata_status(channel);
    if (status == 0 || status == 0xFFu)
        return 0;
    wait = ata_wait_ready(channel, ATA_SR_DRQ, ATA_POLL_LIMIT);
    if (wait == ATA_WAIT_ABSENT)
        return 0;
    if (wait != ATA_WAIT_OK)
        return -1;

    for (uint32_t i = 0; i < 256u; i++)
        words[i] = inw((uint16_t)(channel->io_base + ATA_REG_DATA));
    return 1;
}

static int ata_probe_device(ata_channel_t *channel, int channel_index,
                            uint8_t device) {
    uint16_t words[256];
    const char *name = ata_device_name(channel_index, device);
    uint32_t sectors = 0;
    int lba48 = 0;
    int result = ata_identify(channel, device, words);

    if (result == 0)
        return 0;
    if (result < 0) {
        if (klog_begin(KLOG_WARN)) {
            klog_write("ata: ATA-SKIP ");
            klog_write(name);
            klog_write(" identify-failed\n");
        }
        return 0;
    }

    if (words[0] & 0x8000u) {
        if (klog_begin(KLOG_WARN)) {
            klog_write("ata: ATA-SKIP ");
            klog_write(name);
            klog_write(" ATAPI-unsupported\n");
        }
        return 0;
    }

    if ((words[49] & 0x0200u) != 0) {
        sectors = (uint32_t)words[60] | ((uint32_t)words[61] << 16);
        lba48 = 0;
    }
    if (!sectors && (words[49] & 0x0400u) != 0) {
        sectors = ata_lba48_capacity(words);
        lba48 = 1;
    }
    if (!sectors) {
        if (klog_begin(KLOG_WARN)) {
            klog_write("ata: ATA-SKIP ");
            klog_write(name);
            klog_write(" no-LBA-capacity\n");
        }
        return 0;
    }

    channel->present[device] = 1;
    channel->lba48[device] = (uint8_t)lba48;
    channel->sectors[device] = sectors;
    if (device_count < ATA_MAX_DEVICES)
        device_slots[device_count] = (uint8_t)(channel_index * 2 + device);
    device_count++;

    klog_prefix();
    klog_write("ata: ATA-OK ");
    klog_write(name);
    klog_write(lba48 ? " LBA48 sectors=" : " LBA28 sectors=");
    klog_dec(sectors);
    klog_write("\n");
    return 1;
}

static int ata_recovery_self_test(void) {
    ata_channel_t *channel = ata_active();

    if (!channel)
        return 0;
    if (!ata_read_sectors_unlocked(0, 1, recovery_buffer_a))
        return 0;
    ata_software_reset(channel, (uint8_t)active_device);
    if (!ata_read_sectors_unlocked(0, 1, recovery_buffer_b))
        return 0;
    if (memcmp(recovery_buffer_a, recovery_buffer_b, 512u) != 0)
        return 0;
    return 1;
}

static int ata_multi_device_self_test(void) {
    if (device_count < 2u)
        return 1;
    if (!ata_select_device(1u))
        return 0;
    if (!ata_read_sectors_unlocked(0, 1, recovery_buffer_a))
        return 0;
    return ata_select_device(0u);
}

static int ata_init_unlocked(void) {
    channels[ATA_CHANNEL_PRIMARY].io_base = ATA_PRIMARY_IO;
    channels[ATA_CHANNEL_PRIMARY].ctrl_base = ATA_PRIMARY_CTRL;
    channels[ATA_CHANNEL_SECONDARY].io_base = ATA_SECONDARY_IO;
    channels[ATA_CHANNEL_SECONDARY].ctrl_base = ATA_SECONDARY_CTRL;
    for (uint32_t i = 0; i < 2u; i++) {
        channels[i].present[0] = 0;
        channels[i].present[1] = 0;
        channels[i].lba48[0] = 0;
        channels[i].lba48[1] = 0;
        channels[i].sectors[0] = 0;
        channels[i].sectors[1] = 0;
        outb(channels[i].ctrl_base, ATA_CTRL_NIEN);
        io_wait();
    }

    active_channel = -1;
    active_device = -1;
    active_sectors = 0;
    device_count = 0;
    cache_flush();
    cache_hits = 0;
    cache_misses = 0;
    for (uint32_t i = 0; i < ATA_MAX_DEVICES; i++)
        device_slots[i] = 0;

    for (uint32_t channel = 0; channel < 2u; channel++) {
        for (uint8_t device = 0; device < 2u; device++)
            (void)ata_probe_device(&channels[channel], (int)channel, device);
    }

    if (!device_count) {
        klog_log(KLOG_ERROR, "ATA-FAIL: no usable ATA device\n");
        return 0;
    }
    if (!ata_select_device(0)) {
        klog_log(KLOG_ERROR, "ATA-FAIL: device selection failed\n");
        return 0;
    }

    if (klog_begin(KLOG_INFO)) {
        klog_write("ata: ATA-SCAN-OK devices=");
        klog_dec(device_count);
        klog_write(" active=");
        klog_write(ata_device_name(active_channel, active_device));
        klog_write("\n");
    }

    if (!ata_recovery_self_test()) {
        klog_log(KLOG_ERROR, "ATA-FAIL: recovery self-test failed\n");
        return 0;
    }
    klog_prefix();
    klog_write("ata: ATA-RECOVERY-OK: software-reset reread verified\n");

    if (!ata_multi_device_self_test()) {
        klog_log(KLOG_ERROR, "ATA-FAIL: secondary device read failed\n");
        return 0;
    }
    if (device_count > 1u) {
        klog_prefix();
        klog_write("ata: ATA-MULTI-OK: second device sector read verified\n");
    }

    /* The read cache must serve a repeated sector without touching the
     * drive: read one sector twice and require the second read to hit. */
    cache_flush();
    {
        uint32_t hits_before = cache_hits;
        uint32_t misses_before = cache_misses;
        if (!cached_read_sector(0, recovery_buffer_a) ||
            !cached_read_sector(0, recovery_buffer_b)) {
            klog_log(KLOG_ERROR, "ATA-FAIL: cache self-test read failed\n");
            return 0;
        }
        if (cache_hits != hits_before + 1u ||
            cache_misses != misses_before + 1u ||
            memcmp(recovery_buffer_a, recovery_buffer_b, ATA_SECTOR_SIZE)) {
            klog_log(KLOG_ERROR, "ATA-FAIL: sector cache did not hit\n");
            return 0;
        }
        klog_prefix();
        klog_write("ata: ATA-CACHE-OK: repeated sector served from cache, "
                   "hits=");
        klog_dec(cache_hits);
        klog_write(" misses=");
        klog_dec(cache_misses);
        klog_write("\n");
    }
    return 1;
}

int ata_init(void) {
    int result;
    mutex_lock(&ata_lock);
    result = ata_init_unlocked();
    mutex_unlock(&ata_lock);
    return result;
}

int ata_select_device(uint32_t index) {
    uint8_t slot;

    if (index >= device_count || index >= ATA_MAX_DEVICES)
        return 0;
    slot = device_slots[index];
    active_channel = slot >> 1;
    active_device = slot & 1;
    active_sectors = channels[active_channel].sectors[active_device];
    cache_flush();
    return 1;
}

static ata_channel_t *ata_active(void) {
    if (active_channel < 0)
        return 0;
    return &channels[active_channel];
}

static void ata_log_failure(const char *operation, uint32_t lba,
                            const ata_channel_t *channel, uint32_t attempts,
                            int reason) {
    if (klog_begin(KLOG_ERROR)) {
        klog_write("ata: ");
        klog_write(operation);
        klog_write("-FAIL: ");
        klog_write(ata_device_name(active_channel, active_device));
        klog_write(" lba=");
        klog_dec(lba);
        klog_write(" reason=");
        klog_write(ata_wait_reason(reason));
        klog_write(" status=");
        klog_hex(ata_status(channel));
        klog_write(" error=");
        klog_hex(inb((uint16_t)(channel->io_base + ATA_REG_ERROR)));
        klog_write(" attempts=");
        klog_dec(attempts);
        klog_write("\n");
    }
}

static void ata_log_recovery(const char *operation, uint32_t lba,
                             int reason) {
    if (klog_begin(KLOG_WARN)) {
        klog_write("ata: ");
        klog_write(operation);
        klog_write("-RECOVER: lba=");
        klog_dec(lba);
        klog_write(" reason=");
        klog_write(ata_wait_reason(reason));
        klog_write(" reset-and-retry\n");
    }
}

static int ata_read_one(ata_channel_t *channel, uint8_t device, int lba48,
                        uint32_t lba, uint8_t *destination) {
    int wait;

    ata_program_lba(channel, device, lba, lba48,
                    lba48 ? ATA_CMD_READ_EXT : ATA_CMD_READ);
    wait = ata_wait_ready(channel, ATA_SR_DRQ, ATA_POLL_LIMIT);
    if (wait != ATA_WAIT_OK)
        return wait;

    /* One `rep insw` for the whole sector: the drive is already in PIO
     * transfer state, so 256 individual port reads only add latency. */
    inw_string((uint16_t)(channel->io_base + ATA_REG_DATA), destination,
               256u);
    return ata_wait_ready(channel, 0, ATA_POLL_LIMIT);
}

static int ata_read_sectors_unlocked(uint32_t lba, uint8_t count,
                                     void *buffer) {
    uint8_t *destination = (uint8_t *)buffer;
    ata_channel_t *channel = ata_active();

    if (!channel || !buffer || !count || !active_sectors ||
        lba >= active_sectors ||
        (uint32_t)count > active_sectors - lba ||
        (!channel->lba48[active_device] &&
         (lba >= 0x10000000u ||
          (uint32_t)count > 0x10000000u - lba)))
        return 0;

    for (uint32_t sector = 0; sector < count; sector++) {
        uint32_t current_lba = lba + sector;
        int reason = ATA_WAIT_ERROR;
        uint32_t attempts = 0;

        for (uint32_t attempt = 0; attempt <= ATA_RETRIES; attempt++) {
            attempts = attempt + 1u;
            reason = ata_read_one(channel, (uint8_t)active_device,
                                  channel->lba48[active_device], current_lba,
                                  destination + sector * 512u);
            if (reason == ATA_WAIT_OK)
                break;
            if (reason == ATA_WAIT_ABSENT)
                break;
            if (attempt < ATA_RETRIES) {
                ata_log_recovery("ATA-READ", current_lba, reason);
                ata_software_reset(channel, (uint8_t)active_device);
            }
        }
        if (reason != ATA_WAIT_OK) {
            ata_log_failure("ATA-READ", current_lba, channel, attempts, reason);
            return 0;
        }
    }
    return 1;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    int result;
    if (!buffer || !count) return 0;
    mutex_lock(&ata_lock);
    if (!ata_range_valid(lba, count)) {
        mutex_unlock(&ata_lock);
        return 0;
    }
    if (count == 1u)
        result = cached_read_sector(lba, buffer);
    else
        result = ata_read_sectors_unlocked(lba, count, buffer);
    mutex_unlock(&ata_lock);
    return result;
}

static int ata_flush(ata_channel_t *channel, uint8_t device, int lba48) {
    uint32_t attempts = 0;
    int reason = ATA_WAIT_ERROR;

    for (uint32_t attempt = 0; attempt <= ATA_RETRIES; attempt++) {
        attempts = attempt + 1u;
        ata_select(channel, device, 0, lba48);
        outb((uint16_t)(channel->io_base + ATA_REG_COMMAND),
             lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH);
        ata_delay_400ns(channel);
        reason = ata_wait_ready(channel, 0, ATA_POLL_LIMIT);
        if (reason == ATA_WAIT_OK)
            return 1;
        if (reason == ATA_WAIT_ABSENT)
            break;
        if (attempt < ATA_RETRIES) {
            ata_log_recovery("ATA-FLUSH", 0, reason);
            ata_software_reset(channel, device);
        }
    }
    if (reason == ATA_WAIT_ERROR) {
        /* A drive that aborts FLUSH CACHE still accepted the data; the
         * write is complete, only the cache barrier is missing. */
        if (klog_begin(KLOG_WARN)) {
            klog_write("ata: ATA-FLUSH-UNSUPPORTED: device rejected the "
                       "cache flush, write still accepted\n");
        }
        return 1;
    }
    ata_log_failure("ATA-FLUSH", 0, channel, attempts, reason);
    return 0;
}

static int ata_write_sectors_unlocked(uint32_t lba, uint8_t count,
                                      const void *buffer) {
    const uint8_t *source = (const uint8_t *)buffer;
    ata_channel_t *channel = ata_active();

    if (!channel || !buffer || !count || !active_sectors ||
        lba >= active_sectors ||
        (uint32_t)count > active_sectors - lba ||
        (!channel->lba48[active_device] &&
         (lba >= 0x10000000u ||
          (uint32_t)count > 0x10000000u - lba)))
        return 0;

    for (uint32_t sector = 0; sector < count; sector++) {
        uint32_t current_lba = lba + sector;
        int reason = ATA_WAIT_ERROR;
        uint32_t attempts = 0;

        for (uint32_t attempt = 0; attempt <= ATA_RETRIES; attempt++) {
            attempts = attempt + 1u;
            ata_program_lba(channel, (uint8_t)active_device, current_lba,
                            channel->lba48[active_device],
                            channel->lba48[active_device] ? ATA_CMD_WRITE_EXT
                                                          : ATA_CMD_WRITE);
            reason = ata_wait_ready(channel, ATA_SR_DRQ, ATA_POLL_LIMIT);
            if (reason == ATA_WAIT_OK) {
                uint32_t offset = sector * 512u;
                for (uint32_t word = 0; word < 256u; word++) {
                    uint32_t byte = offset + word * 2u;
                    uint16_t value = (uint16_t)source[byte] |
                                     ((uint16_t)source[byte + 1u] << 8);
                    outw((uint16_t)(channel->io_base + ATA_REG_DATA), value);
                }
                reason = ata_wait_ready(channel, 0, ATA_POLL_LIMIT);
            }
            if (reason == ATA_WAIT_OK)
                break;
            if (reason == ATA_WAIT_ABSENT)
                break;
            if (attempt < ATA_RETRIES) {
                ata_log_recovery("ATA-WRITE", current_lba, reason);
                ata_software_reset(channel, (uint8_t)active_device);
            }
        }
        if (reason != ATA_WAIT_OK) {
            ata_log_failure("ATA-WRITE", current_lba, channel, attempts,
                            reason);
            return 0;
        }
    }

    return ata_flush(channel, (uint8_t)active_device,
                     channel->lba48[active_device]);
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    int result;
    if (!buffer || !count) return 0;
    mutex_lock(&ata_lock);
    if (!ata_range_valid(lba, count)) {
        mutex_unlock(&ata_lock);
        return 0;
    }
    result = ata_write_sectors_unlocked(lba, count, buffer);
    cache_after_write(lba, count, buffer, result);
    mutex_unlock(&ata_lock);
    return result;
}

void ata_cache_stats(uint32_t *hits, uint32_t *misses) {
    mutex_lock(&ata_lock);
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
    mutex_unlock(&ata_lock);
}

uint32_t ata_sector_count(void) {
    return active_sectors;
}

uint32_t ata_device_count(void) {
    return device_count;
}
