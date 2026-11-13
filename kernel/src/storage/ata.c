#include "ata.h"
#include "arch/x86_64/io.h"

// Dedicated ISA IDE master attached by the QEMU run targets.
#define BASE 0x1e8
#define CONTROL 0x3ee

static bool ready(bool data, bool new_command) {
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t status = inb(BASE + 7);
        if (!status || status == 0xff) return false;
        if (status & 0x80) continue;
        if ((status & 0x21) && !new_command) return false;
        if (data ? (status & 8) : !(status & 8)) return true;
    }
    return false;
}

static void settle(void) {
    for (unsigned i = 0; i < 4; ++i) (void)inb(CONTROL);
}

static bool abort_transfer(void) {
    // Abort any remaining data phase after a partial multi-sector failure.
    // Otherwise a subsequent restore could wait forever for DRQ to disappear.
    outb(CONTROL, 6); // SRST + nIEN
    for (unsigned i = 0; i < 64; ++i) (void)inb(CONTROL);
    outb(CONTROL, 2);
    settle();
    (void)ready(false, true);
    return false;
}

static bool transfer(uint32_t sector, unsigned sectors, void *buffer, bool write) {
    if (!buffer || !sectors || sectors > 256 || sector >= (1u << 28) ||
        sectors > (1u << 28) - sector) return false;
    outb(CONTROL, 2);
    outb(BASE + 6, 0xe0 | (sector >> 24));
    settle();
    if (!ready(false, true)) return false;
    outb(BASE + 2, sectors == 256 ? 0 : sectors);
    outb(BASE + 3, sector);
    outb(BASE + 4, sector >> 8);
    outb(BASE + 5, sector >> 16);
    outb(BASE + 7, write ? 0x30 : 0x20);
    settle();
    unsigned char *data = buffer;
    for (unsigned i = 0; i < sectors; ++i) {
        if (!ready(true, false)) return abort_transfer();
        unsigned words = 256;
        if (write) {
            const void *src = data;
            asm volatile("cld; rep outsw" : "+S"(src), "+c"(words) : "d"((uint16_t)BASE) : "memory");
        } else {
            void *dst = data;
            asm volatile("cld; rep insw" : "+D"(dst), "+c"(words) : "d"((uint16_t)BASE) : "memory");
        }
        data += 512;
        settle();
    }
    return ready(false, false) ? true : abort_transfer();
}

bool ata_read_many(uint32_t sector, unsigned count, void *buffer) {
    return transfer(sector, count, buffer, false);
}
bool ata_write_many(uint32_t sector, unsigned count, const void *buffer) {
    return transfer(sector, count, (void *)buffer, true);
}
bool ata_flush(void) {
    outb(CONTROL, 2);
    outb(BASE + 6, 0xe0);
    settle();
    if (!ready(false, true)) return false;
    outb(BASE + 7, 0xe7);
    settle();
    return ready(false, false) ? true : abort_transfer();
}
