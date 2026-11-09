#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TASK_SLOTS 8
struct task_info {
    unsigned id;
    const char *name;
    uint64_t slices;
    char output[40];
};

struct scheduler_sample {
    struct task_info tasks[TASK_SLOTS];
    uint64_t ticks, seconds;
};
void scheduler_sample(struct scheduler_sample *sample);
void scheduler_start(void);
void scheduler_wait_input(void);
void scheduler_sleep(unsigned ticks);
uint64_t scheduler_ticks(void);
unsigned scheduler_spawn(const char *name, void (*entry)(void *), void *context, void (*release)(void *));
void scheduler_reap(void);
void scheduler_output(void *unused, const char *text);
bool scheduler_stop(unsigned id);
bool scheduler_info(unsigned slot, struct task_info *info);
uint64_t scheduler_seconds(void);
