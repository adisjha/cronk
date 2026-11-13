#include "arch/clock.h"
#include "arch/cpu.h"
#include "platform.h"
static uint64_t epoch, origin, frequency;
void clock_start(void) {
    volatile uint32_t *rtc = arm_mmio(0x09010000);
    epoch = rtc[0];
    origin = arch_timestamp(); frequency = arch_counter_frequency();
}
bool clock_read(struct clock_sample *sample) {
    if (!frequency) return false;
    uint64_t delta = arch_timestamp() - origin;
    uint64_t seconds = epoch + delta / frequency;
    *sample = (struct clock_sample){seconds / 3600 % 24, seconds / 60 % 60,
                                   seconds % 60, (delta % frequency) * 1000 / frequency};
    return true;
}
