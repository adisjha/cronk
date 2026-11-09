#include "hardware_benchmark.h"
#include "scheduler.h"
#include "mm/pages.h"
#include "storage/ata.h"
#include "storage/files.h"
#include "lib/memory.h"
#include "term/terminal.h"

#define RAM_PAGES 2048u
#define SECTOR_BYTES 512u

static volatile uint64_t sink;

static uint64_t stamp(void) {
    uint32_t lo, hi;
    // MFENCE drains stores before ending the RAM write measurement.
    asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(0));
}

static void fixed(struct limine_framebuffer *fb, uint64_t hundredths) {
    terminal_write_uint(fb, hundredths / 100);
    terminal_putchar(fb, '.');
    terminal_putchar(fb, '0' + hundredths / 10 % 10);
    terminal_putchar(fb, '0' + hundredths % 10);
}

static uint64_t calibrate(void) {
    // Cross a tick boundary, then measure >= 100 complete PIT intervals.
    uint64_t tick = scheduler_ticks();
    while (scheduler_ticks() == tick) scheduler_sleep(1);
    uint64_t start_tick = scheduler_ticks(), start = stamp();
    scheduler_sleep(100);
    uint64_t end = stamp(), elapsed = scheduler_ticks() - start_tick;
    uint64_t ns = elapsed * scheduler_tick_period_ns();
    return ns ? (end - start) * 1000000000 / ns : 0;
}

static uint64_t microseconds(uint64_t elapsed, uint64_t hz) {
    return elapsed / hz * 1000000 + elapsed % hz * 1000000 / hz;
}

static void rate(struct limine_framebuffer *fb, const char *name, uint64_t bytes,
                 uint64_t elapsed, uint64_t hz) {
    terminal_write(fb, name);
    if (!elapsed) { terminal_write(fb, "timer unavailable\n"); return; }
    // Bytes/s / 10,000 is decimal MB/s with two fractional digits.
    uint64_t us = microseconds(elapsed, hz);
    if (!us) { terminal_write(fb, "sample too short\n"); return; }
    uint64_t per_second = bytes * 1000000 / us;
    fixed(fb, per_second / 10000);
    terminal_write(fb, " MB/s   ");
    fixed(fb, bytes / 10000);
    terminal_write(fb, " MB in ");
    terminal_write_uint(fb, us / 1000);
    terminal_write(fb, " ms\n");
}

static void cpu_bench(struct limine_framebuffer *fb, uint64_t hz) {
    unsigned a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        char brand[49] = {0};
        for (unsigned i = 0; i < 3; ++i) {
            cpuid(0x80000002 + i, &a, &b, &c, &d);
            memcpy(brand + i * 16, &a, 4); memcpy(brand + i * 16 + 4, &b, 4);
            memcpy(brand + i * 16 + 8, &c, 4); memcpy(brand + i * 16 + 12, &d, 4);
        }
        terminal_write(fb, "CPU: "); terminal_write(fb, brand); terminal_putchar(fb, '\n');
    }
    cpuid(0, &a, &b, &c, &d);
    unsigned maximum = a;
    terminal_write(fb, "Advertised base clock: ");
    if (maximum >= 0x16) {
        cpuid(0x16, &a, &b, &c, &d);
        a &= 0xffff;
    } else a = 0;
    if (a) { terminal_write_uint(fb, a); terminal_write(fb, " MHz\n"); }
    else terminal_write(fb, "not exposed by this CPU/VM\n");
    terminal_write(fb, "TSC timer (PIT-calibrated): ");
    fixed(fb, hz / 10000); terminal_write(fb, " MHz\n");
    terminal_write(fb, "TSC is a timer rate, not live core clock speed.\n");
    unsigned value = 1;
    const unsigned steps = 10000000;
    uint64_t start = stamp();
    asm volatile("" : "+r"(value) :: "memory");
    for (unsigned i = 0; i < steps; ++i) {
        value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    }
    asm volatile("" : "+r"(value) :: "memory");
    uint64_t elapsed = stamp() - start;
    sink = value;
    if (value != 455775947u) {
        terminal_write(fb, "CPU checksum FAILED\n");
        return;
    }
    terminal_write(fb, "CPU xorshift: ");
    if (elapsed) fixed(fb, (uint64_t)steps * hz / elapsed / 10000);
    else terminal_write(fb, "--");
    terminal_write(fb, " Msteps/s (10M steps, checksum OK)\n");
}

static void ram_bench(struct limine_framebuffer *fb, uint64_t hz) {
    uint64_t *buffer = pages_alloc(RAM_PAGES, PAGE_CODE);
    if (!buffer) { terminal_write(fb, "RAM: needs 8 MiB of contiguous free RAM; skipped\n"); return; }
    volatile uint64_t *words = buffer;
    const size_t bytes = RAM_PAGES * PAGE_SIZE, count = bytes / sizeof(*buffer);
    const uint64_t pattern = 0x5a5a5a5a5a5a5a5aULL;
    memset(buffer, 0x5a, bytes);
    terminal_write(fb, "RAM: 8 MiB working set, scalar sequential access.\n");
    unsigned passes = 0;
    uint64_t sum = 0, start = stamp(), elapsed;
    do {
        for (size_t i = 0; i < count; ++i) sum += words[i];
        ++passes;
        elapsed = stamp() - start;
    } while (elapsed < hz / 4 && passes < 1024);
    sink = sum;
    bool read_valid = sum == pattern * count * passes;
    if (read_valid) rate(fb, "RAM read:  ", bytes * passes, elapsed, hz);
    else terminal_write(fb, "RAM read checksum FAILED\n");
    passes = 0;
    start = stamp();
    do {
        for (size_t i = 0; i < count; ++i) words[i] = pattern ^ passes;
        ++passes;
        elapsed = stamp() - start;
    } while (elapsed < hz / 4 && passes < 1024);
    bool valid = true;
    for (size_t i = 0; i < count; ++i)
        if (words[i] != (pattern ^ (passes - 1))) { valid = false; break; }
    if (valid) rate(fb, "RAM write: ", bytes * passes, elapsed, hz);
    else terminal_write(fb, "RAM write verification FAILED\n");
    pages_free(buffer, RAM_PAGES);
    terminal_write(fb, read_valid && valid ? "RAM verified and released. Results include cache effects.\n" :
                       "RAM buffer released after verification failure.\n");
}

static void disk_bench(struct limine_framebuffer *fb, uint64_t hz) {
    unsigned char original[SECTOR_BYTES], buffer[SECTOR_BYTES], check[SECTOR_BYTES];
    if (!ata_read(0, buffer) || memcmp(buffer, "CRONKFS1", 8)) {
        terminal_write(fb, "Disk: data disk unavailable or invalid; skipped\n"); return;
    }
    unsigned scratch = 0;
    for (unsigned i = 1; i <= FILE_SLOTS; ++i) {
        if (!ata_read(i, original)) {
            terminal_write(fb, "Disk: I/O error finding an unused slot\n"); return;
        }
        if (!original[0]) { scratch = i; break; }
    }
    terminal_write(fb, "Disk: 512-byte ATA PIO; repeated 32 KiB file area.\n");
    unsigned operations = 0;
    uint64_t start = stamp(), elapsed;
    bool ok = true;
    do {
        if (!ata_read(1 + operations % FILE_SLOTS, buffer)) { ok = false; break; }
        sink += buffer[0];
        ++operations;
        elapsed = stamp() - start;
    } while (elapsed < hz / 4 && operations < 65536);
    elapsed = stamp() - start;
    if (ok) rate(fb, "Disk read:  ", operations * SECTOR_BYTES, elapsed, hz);
    else { terminal_write(fb, "Disk read: I/O error; writes skipped\n"); return; }
    if (!scratch) {
        terminal_write(fb, "Disk write: skipped; no unused file slot (files preserved)\n"); return;
    }
    // Keep the filename empty throughout: even interrupted tests cannot turn
    // scratch data into a visible file. Never write the header or an occupied slot.
    memset(buffer, 0, sizeof(buffer));
    operations = 0;
    start = stamp();
    do {
        for (unsigned i = 32; i < 511; ++i) buffer[i] = (unsigned char)(i + operations);
        if (!ata_write(scratch, buffer)) { ok = false; break; }
        ++operations;
        elapsed = stamp() - start;
    } while (elapsed < hz / 4 && operations < 4096);
    elapsed = stamp() - start;
    bool verified = ok && ata_read(scratch, check) && !memcmp(buffer, check, sizeof(buffer));
    bool restored = ata_read(scratch, check) && !memcmp(original, check, sizeof(original));
    if (!restored)
        restored = ata_write(scratch, original) && ata_read(scratch, check) &&
                   !memcmp(original, check, sizeof(original));
    if (!restored) terminal_write(fb, "Disk scratch restore FAILED (unused slot only)\n");
    else if (!verified) terminal_write(fb, "Disk write: I/O or verification error; scratch restored\n");
    else {
        rate(fb, "Disk write: ", operations * SECTOR_BYTES, elapsed, hz);
        terminal_write(fb, "Writes flushed; readback verified; unused slot restored.\n");
    }
}

void hardware_benchmark(struct limine_framebuffer *fb, enum benchmark_part part) {
    terminal_write(fb, "HARDWARE BENCHMARK   Calibrating timer (~1 second)...\n");
    uint64_t hz = calibrate();
    if (!hz) { terminal_write(fb, "Timer calibration failed\n"); return; }
    if (part == BENCH_ALL || part == BENCH_CPU) cpu_bench(fb, hz);
    if (part == BENCH_ALL || part == BENCH_RAM) ram_bench(fb, hz);
    if (part == BENCH_ALL || part == BENCH_DISK) disk_bench(fb, hz);
    terminal_write(fb, "MB/s is decimal. Elapsed guest time includes other tasks.\n"
                       "QEMU/host caching affects results; these are guest-path speeds.\n");
}
