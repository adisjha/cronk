#include "benchmark.h"
#include "scheduler.h"
#include "term/terminal.h"
#include <stddef.h>

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
    __atomic_store_n(&r->finished, scheduler_ticks(), __ATOMIC_RELEASE);
}

void scheduler_benchmark(struct limine_framebuffer *fb) {
    unsigned reference[2];
    terminal_write(fb, "Two jobs, 100 million xorshift steps each. Scheduler resolution ~10 ms.\n");
    for (unsigned trial = 0; trial < 2; ++trial) {
        struct result jobs[2] = {{.seed = 1}, {.seed = 2}};
        uint64_t start = scheduler_ticks();
        unsigned first = scheduler_spawn("bench", compute, &jobs[0], NULL);
        if (!first || !scheduler_spawn("bench", compute, &jobs[1], NULL)) {
            if (first)
                scheduler_stop(first);
            terminal_write(fb, "benchmark needs two free task slots\n");
            return;
        }
        while (!__atomic_load_n(&jobs[0].finished, __ATOMIC_ACQUIRE) ||
               !__atomic_load_n(&jobs[1].finished, __ATOMIC_ACQUIRE)) {
            if (trial)
                scheduler_sleep(5);
            else
                terminal_tick(fb, NULL);
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

#include "arch/cpu.h"
struct parallel_job {
    unsigned seed, cpu, checksum;
    uint64_t start, end;
    unsigned *gate;
};
static void parallel_compute(void *context) {
    struct parallel_job *job = context;
    while (!__atomic_load_n(job->gate, __ATOMIC_ACQUIRE))
        arch_pause();
    if (__atomic_load_n(job->gate, __ATOMIC_ACQUIRE) != 1)
        return;
    job->cpu = arch_cpu_index();
    job->start = arch_timestamp();
    unsigned value = job->seed;
    for (unsigned i = 0; i < 100000000; ++i) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
    }
    job->checksum = value;
    __atomic_store_n(&job->end, arch_timestamp(), __ATOMIC_RELEASE);
}
void scheduler_parallel_benchmark(struct limine_framebuffer *fb) {
    unsigned gate = 0;
    struct parallel_job jobs[4] = {0};
    unsigned ids[4] = {0};
    struct scheduler_sample before, after;
    scheduler_sample(&before);
    for (unsigned i = 0; i < 4; ++i) {
        jobs[i].seed = i + 1;
        jobs[i].gate = &gate;
        ids[i] = scheduler_spawn("parallel bench", parallel_compute, &jobs[i], NULL);
        if (!ids[i]) {
            for (unsigned n = 0; n < i; ++n)
                scheduler_stop(ids[n]);
            __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
            // Running jobs retire on their owning CPU before this stack is released.
            for (;;) {
                bool live = false;
                for (unsigned slot = 0; slot < TASK_SLOTS; ++slot) {
                    struct task_info info;
                    if (!scheduler_info(slot, &info))
                        continue;
                    for (unsigned n = 0; n < i; ++n)
                        if (info.id == ids[n])
                            live = true;
                }
                if (!live)
                    break;
                scheduler_sleep(1);
            }
            terminal_write(fb, "four free task slots required\n");
            return;
        }
    }
    __atomic_store_n(&gate, 1, __ATOMIC_RELEASE);
    for (;;) {
        bool done = true;
        for (unsigned i = 0; i < 4; ++i)
            if (!__atomic_load_n(&jobs[i].end, __ATOMIC_ACQUIRE))
                done = false;
        if (done)
            break;
        scheduler_sleep(1);
    }
    scheduler_sample(&after);
    uint64_t busy = 0;
    for (unsigned i = 0; i < SCHEDULER_MAX_CPUS; ++i)
        busy += after.cpus[i].busy - before.cpus[i].busy;
    terminal_write(fb, "Parallel jobs complete; CPU capacity used: ");
    terminal_write_uint(fb, busy * 100 / (after.timestamp - before.timestamp));
    terminal_write(fb, "%\n");
    static const unsigned expected[4] = {1175469274u, 1085203167u, 113171973u, 1705664938u};
    for (unsigned i = 0; i < 4; ++i) {
        terminal_write(fb, "job ");
        terminal_write_uint(fb, i);
        terminal_write(fb, " core ");
        terminal_write_uint(fb, jobs[i].cpu);
        terminal_write(fb, " checksum ");
        terminal_write_uint(fb, jobs[i].checksum);
        terminal_write(fb, jobs[i].checksum == expected[i] ? " OK\n" : " FAILED\n");
    }
}
