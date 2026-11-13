#include "storage/extentfs.h"
#include "hardware_benchmark.h"
#include "arch/cpu.h"
#include "arch/cache.h"
#include "scheduler.h"
#include "mm/pages.h"
#include "storage/block.h"
#include "storage/files.h"
#include "lib/memory.h"
#include "term/terminal.h"

#define RAM_DEFAULT_BYTES (8u * 1024 * 1024)
#define SECTOR_BYTES 512u

static volatile uint64_t sink;

static void scalar_copy(void *dest, const void *src, size_t bytes) {
    volatile unsigned char *d = dest;
    const volatile unsigned char *s = src;
    for (size_t i = 0; i < bytes; ++i) d[i] = s[i];
}

static uint64_t stamp(void) {
#if defined(__aarch64__)
    uint64_t t;
    asm volatile("dsb sy; isb; mrs %0, cntvct_el0; isb" : "=r"(t) :: "memory");
    return t;
#else
    uint32_t lo, hi;
    // MFENCE drains stores before ending the RAM write measurement.
    asm volatile("mfence; lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
#endif
}

#if !defined(__aarch64__)
static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(0));
}

#endif

static void fixed(struct limine_framebuffer *fb, uint64_t hundredths) {
    terminal_write_uint(fb, hundredths / 100);
    terminal_putchar(fb, '.');
    terminal_putchar(fb, '0' + hundredths / 10 % 10);
    terminal_putchar(fb, '0' + hundredths % 10);
}

static uint64_t calibrate(void) {
#if defined(__aarch64__)
    return arch_counter_frequency();
#else
    // Cross a tick boundary, then measure >= 100 complete PIT intervals.
    uint64_t tick = scheduler_ticks();
    while (scheduler_ticks() == tick) scheduler_sleep(1);
    uint64_t start_tick = scheduler_ticks(), start = stamp();
    scheduler_sleep(100);
    uint64_t end = stamp(), elapsed = scheduler_ticks() - start_tick;
    uint64_t ns = elapsed * scheduler_tick_period_ns();
    return ns ? (end - start) * 1000000000 / ns : 0;
#endif
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
#if defined(__aarch64__)
    terminal_write(fb, "CPU: AArch64 / ARM64\nGeneric timer: ");
    fixed(fb, hz / 10000); terminal_write(fb, " MHz\n");
    terminal_write(fb, "Timer frequency is not live CPU core clock speed.\n");
#else
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
#endif
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

static void working_size(struct limine_framebuffer *fb, size_t bytes);

static void ram_bench(struct limine_framebuffer *fb, uint64_t hz, size_t bytes) {
    const size_t page_count = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *buffer = pages_alloc(page_count, PAGE_BUFFER);
    if (!buffer) {
        terminal_write(fb, "RAM: needs "); working_size(fb, bytes);
        terminal_write(fb, " of contiguous free RAM; skipped\n"); return;
    }
    volatile uint64_t *words = buffer;
    const size_t count = bytes / sizeof(*buffer);
    const uint64_t pattern = 0x5a5a5a5a5a5a5a5aULL;
    memset(buffer, 0x5a, bytes);
    terminal_write(fb, "RAM: "); working_size(fb, bytes);
    terminal_write(fb, " working set, scalar sequential access.\n");
    unsigned passes = 0;
    uint64_t sum = 0, start = stamp(), elapsed;
    do {
        for (size_t i = 0; i < count; ++i) sum += words[i];
        ++passes;
        elapsed = stamp() - start;
    } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
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
    } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
    bool valid = true;
    for (size_t i = 0; i < count; ++i)
        if (words[i] != (pattern ^ (passes - 1))) { valid = false; break; }
    if (valid) rate(fb, "RAM write: ", bytes * passes, elapsed, hz);
    else terminal_write(fb, "RAM write verification FAILED\n");
    if (read_valid && valid) {
        memset(buffer, 0x5a, bytes);
        passes = 0; sum = 0; start = stamp();
        do {
            sum += memory_sum64(buffer, count);
            ++passes;
            elapsed = stamp() - start;
        } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
        sink = sum;
        if (sum == pattern * count * passes) rate(fb, "RAM bulk read: ", bytes * passes, elapsed, hz);
        else { terminal_write(fb, "RAM bulk read FAILED\n"); valid = false; }
        passes = 0; start = stamp();
        do {
            memset(buffer, passes & 255, bytes);
            ++passes;
            elapsed = stamp() - start;
        } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
        bool filled = true;
        for (size_t i = 0; i < bytes; ++i)
            if (((unsigned char *)buffer)[i] != ((passes - 1) & 255)) { filled = false; break; }
        if (filled) rate(fb, "RAM bulk fill: ", bytes * passes, elapsed, hz);
        else { terminal_write(fb, "RAM bulk fill FAILED\n"); valid = false; }
        void *copy = pages_alloc(page_count, PAGE_BUFFER);
        if (!copy) {
            terminal_write(fb, "RAM bulk copy: needs another "); working_size(fb, bytes);
            terminal_write(fb, "; skipped\n");
        }
        else {
            memset(copy, ~((unsigned char *)buffer)[0], bytes);
            passes = 0; start = stamp();
            do {
                scalar_copy(copy, buffer, bytes);
                ++passes;
                elapsed = stamp() - start;
            } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
            if (!memcmp(copy, buffer, bytes)) rate(fb, "RAM byte copy: ", bytes * passes, elapsed, hz);
            else { terminal_write(fb, "RAM byte copy FAILED\n"); valid = false; }
            memset(copy, ~((unsigned char *)buffer)[0], bytes);
            passes = 0; start = stamp();
            do {
                memcpy(copy, buffer, bytes);
                ++passes;
                elapsed = stamp() - start;
            } while (elapsed < hz / 4 && (uint64_t)passes * bytes < (1ULL << 36));
            if (!memcmp(copy, buffer, bytes)) rate(fb, "RAM bulk copy: ", bytes * passes, elapsed, hz);
            else { terminal_write(fb, "RAM bulk copy FAILED\n"); valid = false; }
            pages_free(copy, page_count);
        }
    }
    pages_free(buffer, page_count);
    terminal_write(fb, read_valid && valid ? "RAM verified and released. Results include cache effects.\n" :
                       "RAM buffer released after verification failure.\n");
}

static void working_size(struct limine_framebuffer *fb, size_t bytes) {
    if (bytes < 1024 * 1024 || bytes % (1024 * 1024)) { terminal_write_uint(fb, bytes / 1024); terminal_write(fb, " KiB"); }
    else { terminal_write_uint(fb, bytes / (1024 * 1024)); terminal_write(fb, " MiB"); }
}

static void cache_geometry(struct limine_framebuffer *fb) {
    struct cache_info info[CACHE_INFO_MAX];
    unsigned count = arch_cache_info(info);
    terminal_write(fb, "Guest-reported cache geometry (may be virtualized):\n");
    if (!count) terminal_write(fb, "  Cache capacities not exposed to this guest.\n");
    for (unsigned i = 0; i < count; ++i) {
        terminal_write(fb, "  L"); terminal_write_uint(fb, info[i].level);
        terminal_write(fb, info[i].kind == 1 ? " data: " : info[i].kind == 2 ? " instruction: " : " unified: ");
        working_size(fb, info[i].bytes);
        terminal_write(fb, "; line "); terminal_write_uint(fb, info[i].line);
        terminal_write(fb, " B; ways "); terminal_write_uint(fb, info[i].ways); terminal_putchar(fb, '\n');
    }
    terminal_write(fb, "Unreported levels are unknown, not proof they do not exist.\n");
}

static void fixed_field(struct limine_framebuffer *fb, uint64_t value, unsigned width) {
    uint64_t integer = value / 100;
    unsigned digits = 1;
    while (integer >= 10) { integer /= 10; ++digits; }
    for (unsigned n = digits + 3; n < width; ++n) terminal_putchar(fb, ' ');
    fixed(fb, value);
}

// Timed passes cover every word. Small working sets run in batches to amortize
// counter/fence overhead; verification and setup stay outside the timed region.
static bool stream_sample(void *buffer, size_t bytes, uint64_t hz, bool write, uint64_t *hundredths) {
    unsigned repeats = bytes < 256 * 1024 ? (256 * 1024) / bytes : 1;
    uint64_t passes = 0, sum = 0, elapsed, start;
    const uint64_t pattern = 0x5a5a5a5a5a5a5a5aULL;
    size_t words = bytes / 8;
    memset(buffer, 0x5a, bytes);
    sink = memory_sum64(buffer, words); // Untimed warm-up pass.
    start = stamp();
    do {
        for (unsigned i = 0; i < repeats; ++i) {
            if (write) memset(buffer, passes & 255, bytes);
            else sum += memory_sum64(buffer, words);
            ++passes;
        }
        elapsed = stamp() - start;
    } while (elapsed < hz / 10 && passes * bytes < (1ULL << 36));
    sink = sum;
    if (write) {
        uint64_t expected = ((passes - 1) & 255) * 0x0101010101010101ULL;
        const uint64_t *data = buffer;
        for (size_t i = 0; i < words; ++i) if (data[i] != expected) return false;
    } else if (sum != pattern * words * passes) return false;
    uint64_t us = microseconds(elapsed, hz);
    if (!us) return false;
    *hundredths = passes * bytes * 100 / us; // Decimal MB/s, two places.
    return true;
}

static uint32_t random_step(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return *state = value;
}

static bool dependent_sample(void *buffer, size_t bytes, uint64_t hz, uint64_t *hundredths) {
    // 128-byte spacing covers a line on Apple Silicon and avoids packing nodes
    // within the usual 64-byte x86 line. Sattolo shuffle forms one random cycle.
    const size_t spacing = 128, nodes = bytes / spacing;
    unsigned char *base = buffer;
    for (size_t i = 0; i < nodes; ++i) *(uintptr_t *)(base + i * spacing) = i;
    uint32_t random = 0x9e3779b9;
    for (size_t i = nodes - 1; i; --i) {
        size_t j = random_step(&random) % i;
        uintptr_t *a = (void *)(base + i * spacing), *b = (void *)(base + j * spacing);
        uintptr_t temporary = *a; *a = *b; *b = temporary;
    }
    for (size_t i = 0; i < nodes; ++i) {
        uintptr_t *node = (void *)(base + i * spacing);
        *node = (uintptr_t)(base + *node * spacing);
    }
    uintptr_t cursor = (uintptr_t)base;
    // Validate and warm the entire cycle before timing any pointer chasing.
    for (size_t i = 0; i < nodes; ++i) {
        cursor = *(volatile uintptr_t *)cursor;
        if (cursor < (uintptr_t)base || cursor >= (uintptr_t)base + bytes ||
            (cursor - (uintptr_t)base) % spacing || (cursor == (uintptr_t)base && i + 1 != nodes)) return false;
    }
    if (cursor != (uintptr_t)base) return false;
    uint64_t steps = 0, start = stamp(), elapsed;
    do {
        for (unsigned i = 0; i < 4096; ++i) cursor = *(volatile uintptr_t *)cursor;
        steps += 4096;
        elapsed = stamp() - start;
    } while (elapsed < hz / 10 && steps < (1u << 28));
    sink = cursor;
    uint64_t us = microseconds(elapsed, hz);
    if (!us) return false;
    *hundredths = us * 100000 / steps; // ns/dependent load, two places.
    return true;
}

static void cache_bench(struct limine_framebuffer *fb, uint64_t hz) {
    cache_geometry(fb);
    terminal_write(fb, "Data working-set sweep: 4 KiB through 256 MiB.\n"
                       "Warm bulk read/fill; random dependent loads.\n"
                       "Instruction-cache execution speed is not measured.\n"
                       "  Working set   Read MB/s   Fill MB/s   Load ns\n");
    const size_t maximum = 256UL * 1024 * 1024;
    void *buffer = pages_alloc(maximum / PAGE_SIZE, PAGE_BUFFER);
    if (!buffer) {
        terminal_write(fb, "Cache sweep: needs 256 MiB contiguous free RAM; skipped\n");
        return;
    }
    bool valid = true;
    for (size_t bytes = 4096; bytes <= maximum; bytes *= 2) {
        uint64_t read, fill, latency;
        if (!stream_sample(buffer, bytes, hz, false, &read) ||
            !stream_sample(buffer, bytes, hz, true, &fill) ||
            !dependent_sample(buffer, bytes, hz, &latency)) {
            terminal_write(fb, "Cache sweep verification FAILED\n"); valid = false; break;
        }
        terminal_write(fb, "  ");
        unsigned number = bytes < 1024 * 1024 ? bytes / 1024 : bytes / (1024 * 1024);
        unsigned digits = number >= 100 ? 3 : number >= 10 ? 2 : 1;
        working_size(fb, bytes);
        for (unsigned i = digits + 4; i < 11; ++i) terminal_putchar(fb, ' ');
        fixed_field(fb, read, 10); terminal_putchar(fb, ' ');
        fixed_field(fb, fill, 10); terminal_putchar(fb, ' ');
        fixed_field(fb, latency, 9); terminal_putchar(fb, '\n');
    }
    pages_free(buffer, maximum / PAGE_SIZE);
    terminal_write(fb, valid ? "Cache sweep verified and released.\n" : "Cache sweep buffer released.\n");
    terminal_write(fb, "Rows are working sets, not isolated L1/L2/L3 speeds.\n"
                       "Load ns includes TLB/VM/loop costs; stores remain cached.\n");
}

static void io_counts(struct limine_framebuffer *fb, struct disk_counters before,
                      struct disk_counters after) {
    terminal_write(fb, "  Commands R/W/flush: ");
    terminal_write_uint(fb, after.reads - before.reads); terminal_putchar(fb, '/');
    terminal_write_uint(fb, after.writes - before.writes); terminal_putchar(fb, '/');
    terminal_write_uint(fb, after.flushes - before.flushes); terminal_putchar(fb, '\n');
}

static bool restore_scratch(unsigned sector, unsigned count, const void *original, void *check) {
    if (disk_read_many(sector, count, check) && !memcmp(original, check, count * 512))
        return disk_flush();
    return disk_write_many(sector, count, original) && disk_flush() &&
           disk_read_many(sector, count, check) && !memcmp(original, check, count * 512);
}

static void disk_bench(struct limine_framebuffer *fb, uint64_t hz) {
    unsigned char header[512];
    if (!disk_read(0, header) || (memcmp(header, "CRONKFS1", 8) && memcmp(header,"CRONKFS2",8))) {
        terminal_write(fb, "Disk: data disk unavailable or invalid; skipped\n"); return;
    }
    bool extents=!memcmp(header,"CRONKFS2",8);
    unsigned scratch=0,count=0,area_start=1;
    if(extents&&extentfs_scratch(&scratch,&count))area_start=scratch;
    // Snapshot every file record; only empty, consecutive records are scratch.
    size_t area_bytes = FILE_SLOTS * 512;
    unsigned char *original = pages_alloc(2 * area_bytes / PAGE_SIZE, PAGE_BUFFER);
    if (!original) { terminal_write(fb, "Disk: needs 64 KiB buffer; skipped\n"); return; }
    unsigned char *buffer = original + area_bytes;
    if (!disk_read_many(area_start, FILE_SLOTS, original)) {
        terminal_write(fb, "Disk: I/O error reading directory snapshot\n"); goto done;
    }
    unsigned run_start = 0, run = 0;
    for (unsigned i = 0; !extents && i < FILE_SLOTS; ++i) {
        if (original[i * 512]) { run = 0; continue; }
        if (!run) run_start = i + 1;
        ++run;
        if (run > count) { scratch = run_start; count = run; }
        if (count == 8) break;
    }
    terminal_write(fb, "Disk: "); terminal_write(fb, disk_backend());
    terminal_write(fb, "; 32 KiB file area.\n");
    for (unsigned batch = 0; batch < 2; ++batch) {
        unsigned operations = 0;
        uint64_t start = stamp(), elapsed;
        bool ok = true;
        struct disk_counters before = disk_counters();
        do {
            bool read = batch ? disk_read_many(area_start, FILE_SLOTS, buffer) :
                                disk_read(area_start + operations % FILE_SLOTS, buffer);
            if (!read) { ok = false; break; }
            sink += buffer[0];
            ++operations;
            elapsed = stamp() - start;
        } while (elapsed < hz / 4 && operations < (batch ? 32768u : 65536u));
        elapsed = stamp() - start;
        struct disk_counters after = disk_counters();
        if (!ok || (batch && memcmp(original, buffer, area_bytes))) {
            terminal_write(fb, "Disk read: I/O or verification error; writes skipped\n"); goto done;
        }
        rate(fb, batch ? "Disk batch read: " : "Disk read:  ",
             (uint64_t)operations * (batch ? area_bytes : 512), elapsed, hz);
        io_counts(fb, before, after);
    }
    if (!scratch) {
        terminal_write(fb, "Disk write: skipped; no unused file slot (files preserved)\n"); goto done;
    }
    terminal_write(fb, "Scratch batch: "); terminal_write_uint(fb, count);
    terminal_write(fb, extents?" free sectors; extent metadata untouched.\n":" empty sectors; filenames stay empty.\n");
    const void *saved = original + (scratch - area_start) * 512;
    for (unsigned batch = 0; batch < 2; ++batch) {
        memset(buffer, 0, count * 512);
        for (unsigned sector = 0; sector < count; ++sector)
            memset(buffer + sector * 512 + 32, 0x5a, 479);
        unsigned rounds = 0;
        bool ok = true;
        struct disk_counters before = disk_counters();
        uint64_t start = stamp(), elapsed;
        do {
            for (unsigned sector = 0; sector < count; ++sector)
                buffer[sector * 512 + 32] = rounds;
            if (batch) ok = disk_write_many(scratch, count, buffer) && disk_flush();
            else {
                for (unsigned sector = 0; sector < count && ok; ++sector)
                    ok = disk_write(scratch + sector, buffer + sector * 512);
            }
            if (!ok) break;
            ++rounds;
            elapsed = stamp() - start;
        } while (elapsed < hz / 4 && rounds < 4096);
        elapsed = stamp() - start;
        struct disk_counters after = disk_counters();
        // Readback lives in a separate allocation to preserve the final pattern.
        size_t check_pages = (count * 512 + PAGE_SIZE - 1) / PAGE_SIZE;
        unsigned char *check = pages_alloc(check_pages, PAGE_BUFFER);
        bool verified = ok && check && disk_read_many(scratch, count, check) &&
                        !memcmp(buffer, check, count * 512);
        // buffer is now reusable for restoration verification even under OOM.
        bool restored = restore_scratch(scratch, count, saved, buffer);
        if (check) pages_free(check, check_pages);
        if (!restored) { terminal_write(fb, "Disk scratch restore FAILED (unused slots only)\n"); goto done; }
        if (!verified) {
            terminal_write(fb, "Disk write: I/O or verification error; scratch restored\n"); goto done;
        }
        rate(fb, batch ? "Disk batch write: " : "Disk write: ",
             (uint64_t)rounds * count * 512, elapsed, hz);
        io_counts(fb, before, after);
    }
    terminal_write(fb, "Writes flushed; readback verified; scratch run restored.\n");
done:
    pages_free(original, 2 * area_bytes / PAGE_SIZE);
}

static void run_benchmark(struct limine_framebuffer *fb, enum benchmark_part part, size_t bytes) {
#if defined(__aarch64__)
    terminal_write(fb, "HARDWARE BENCHMARK   ARM64 generic timer\n");
#else
    terminal_write(fb, "HARDWARE BENCHMARK   Calibrating timer (~1 second)...\n");
#endif
    uint64_t hz = calibrate();
    if (!hz) { terminal_write(fb, "Timer calibration failed\n"); return; }
    if (part == BENCH_ALL || part == BENCH_CPU) cpu_bench(fb, hz);
    if (part == BENCH_ALL || part == BENCH_RAM) ram_bench(fb, hz, bytes);
    if (part == BENCH_ALL || part == BENCH_DISK) disk_bench(fb, hz);
    if (part == BENCH_CACHES) cache_bench(fb, hz);
    terminal_write(fb, "MB/s is decimal. Elapsed guest time includes other tasks.\n"
                       "QEMU/host caching affects results; these are guest-path speeds.\n");
}

void hardware_benchmark(struct limine_framebuffer *fb, enum benchmark_part part) {
    run_benchmark(fb, part, RAM_DEFAULT_BYTES);
}
// Units are case-insensitive; a bare integer retains the original MiB meaning.
static bool unit_equal(const char *text, const char *unit) {
    while (*text && *unit) {
        char c = *text++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != *unit++) return false;
    }
    return !*text && !*unit;
}

void hardware_ram_benchmark(struct limine_framebuffer *fb, const char *size) {
    size_t value = 0, multiplier = 1024 * 1024;
    bool digits = false;
    while (*size == ' ') ++size;
    while (*size >= '0' && *size <= '9') {
        unsigned digit = *size++ - '0';
        if (value > (262144u - digit) / 10) goto invalid;
        value = value * 10 + digit;
        digits = true;
    }
    while (*size == ' ') ++size;
    if (*size) {
        if (unit_equal(size, "kib")) multiplier = 1024;
        else if (!unit_equal(size, "mib")) goto invalid;
    }
    if (!digits || !value || value > (256u * 1024 * 1024) / multiplier) goto invalid;
    run_benchmark(fb, BENCH_RAM, value * multiplier);
    return;
invalid:
    terminal_write(fb, "usage: bench ram <size> [KiB|MiB] (1 KiB-256 MiB; default MiB)\n");
}
