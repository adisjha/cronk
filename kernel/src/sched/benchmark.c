#include <stddef.h>
#include "benchmark.h"
#include "scheduler.h"
#include "term/terminal.h"

struct result {
    unsigned seed;
    volatile unsigned checksum;
    volatile uint64_t finished;
};

static void compute(void *context) {
    struct result *r = context;
    unsigned value = r->seed;
    for (unsigned i = 0; i < 100000000; ++i) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
    }
    r->checksum = value;
    r->finished = scheduler_ticks();
}

void scheduler_benchmark(struct limine_framebuffer *fb) {
    unsigned reference[2];
    terminal_write(fb, "Two jobs, 100 million xorshift steps each. PIT resolution ~10 ms.\n");
    for (unsigned trial = 0; trial < 2; ++trial) {
        struct result jobs[2] = {{.seed = 1}, {.seed = 2}};
        uint64_t start = scheduler_ticks();
        unsigned first = scheduler_spawn("bench", compute, &jobs[0], NULL);
        if (!first || !scheduler_spawn("bench", compute, &jobs[1], NULL)) {
            if (first) scheduler_stop(first);
            terminal_write(fb, "benchmark needs two free task slots\n");
            return;
        }
        while (!jobs[0].finished || !jobs[1].finished) {
            if (trial) scheduler_sleep(5);
            else terminal_tick(fb, NULL);
        }
        uint64_t end = jobs[0].finished > jobs[1].finished ? jobs[0].finished : jobs[1].finished;
        terminal_write(fb, trial ? "Sleeping shell: " : "Polling shell:  ");
        terminal_write_uint(fb, (end - start) * 10);
        terminal_write(fb, " ms\n");
        for (unsigned i = 0; i < 2; ++i) {
            if (trial && reference[i] != jobs[i].checksum) {
                terminal_write(fb, "checksum mismatch\n");
                return;
            }
            reference[i] = jobs[i].checksum;
        }
    }
    terminal_write(fb, "Both job checksums matched.\n");
}
