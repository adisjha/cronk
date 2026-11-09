#include "clock.h"
#include "io.h"

// The PC interval timer counts down at 1,193,182 Hz. Channel 2 leaves
// channel 0 available for scheduling. The speaker stays disconnected.
#define PIT_HZ 1193182u
#define PIT_PERIOD 65535u

static uint16_t previous_count;
static unsigned elapsed_ticks;
static unsigned previous_second;
static bool synchronized;

static uint8_t rtc(unsigned reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint16_t pit_count(void) {
    outb(0x43, 0x80); // Latch both bytes of channel 2.
    uint16_t low = inb(0x42);
    return low | ((uint16_t)inb(0x42) << 8);
}

void clock_start(void) {
    uint8_t control = inb(0x61) & ~3u;
    outb(0x61, control);
    outb(0x43, 0xb4); // Channel 2, low/high bytes, rate generator.
    outb(0x42, 0xff);
    outb(0x42, 0xff);
    outb(0x61, control | 1);
    previous_count = pit_count();
    previous_second = 60;
    elapsed_ticks = 0;
    synchronized = false;

    // Wait for the first RTC second edge during boot. If the RTC is stopped,
    // let the shell start and continue sampling from its regular polling loop.
    unsigned waited = 0;
    uint16_t before = previous_count;
    struct clock_sample sample;
    while (waited < 2 * PIT_HZ && !clock_read(&sample)) {
        uint16_t after = pit_count();
        waited += before >= after ? before - after : before + PIT_PERIOD - after;
        before = after;
        asm volatile("pause");
    }
}

static unsigned decode(uint8_t value, bool binary) {
    return binary ? value : (value >> 4) * 10 + (value & 15);
}

bool clock_read(struct clock_sample *sample) {
    static unsigned hour, minute;
    if (rtc(0x0a) & 0x80) return false; // RTC update in progress.
    uint8_t raw_second = rtc(0);
    uint16_t count = pit_count();
    static bool binary;
    if (previous_second == 60 || decode(raw_second, binary) != previous_second) {
        uint8_t mode = rtc(0x0b);
        uint8_t raw_minute = rtc(2), raw_hour = rtc(4);
        if ((rtc(0x0a) & 0x80) || rtc(0) != raw_second) return false;
        binary = (mode & 4) != 0;
        hour = decode(raw_hour & 0x7f, binary);
        if (!(mode & 2)) hour = hour % 12 + ((raw_hour & 0x80) ? 12 : 0);
        minute = decode(raw_minute, binary);
        synchronized = previous_second != 60;
        previous_second = decode(raw_second, binary);
        elapsed_ticks = 0;
    } else {
        unsigned delta = previous_count >= count ? previous_count - count
                                                 : previous_count + PIT_PERIOD - count;
        if (elapsed_ticks < PIT_HZ) elapsed_ticks += delta;
    }
    previous_count = count;
    if (!synchronized) return false;
    unsigned ms = (uint64_t)elapsed_ticks * 1000 / PIT_HZ;
    *sample = (struct clock_sample){ hour, minute, previous_second, ms < 1000 ? ms : 999 };
    return true;
}
