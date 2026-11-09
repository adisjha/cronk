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
        // ERR/DF describe the previous command until a new command is issued.
        // Do not let one failed I/O permanently prevent subsequent recovery I/O.
        if ((status & 0x21) && !new_command) return false;
        if (data ? (status & 8) : !(status & 8)) return true;
    }
    return false;
}

static void settle(void) {
    for (unsigned i = 0; i < 4; ++i) (void)inb(CONTROL);
}

static bool command(uint8_t sector, uint8_t op) {
    outb(CONTROL, 2); // Polling driver, disable device interrupts.
    outb(BASE + 6, 0xe0);
    settle();
    if (!ready(false, true)) return false;
    outb(BASE + 2, 1);
    outb(BASE + 3, sector);
    outb(BASE + 4, 0);
    outb(BASE + 5, 0);
    outb(BASE + 7, op);
    settle();
    return ready(true, false);
}

bool ata_read(uint8_t sector, void *buffer) {
    if (!command(sector, 0x20)) return false;
    unsigned count = 256;
    asm volatile("cld; rep insw" : "+D"(buffer), "+c"(count) : "d"((uint16_t)BASE) : "memory");
    settle();
    return ready(false, false);
}

bool ata_write(uint8_t sector, const void *buffer) {
    if (!command(sector, 0x30)) return false;
    unsigned count = 256;
    asm volatile("cld; rep outsw" : "+S"(buffer), "+c"(count) : "d"((uint16_t)BASE) : "memory");
    settle();
    if (!ready(false, false)) return false;
    outb(BASE + 7, 0xe7); // Commit the device write cache before acknowledging.
    settle();
    return ready(false, false);
}
