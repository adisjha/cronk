#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TASK_SLOTS 16
#define SERVICE_SLOTS 5
#define SERVICE_STACK_SIZE 65536
#define SCHEDULER_MAX_CPUS 8
// Called on the boot CPU before publishing a secondary entry point.
uintptr_t scheduler_prepare_cpu(unsigned index);
struct task_info {
    unsigned id;
    const char *name;
    uint64_t slices, runtime, ram_bytes;
    unsigned ring, cpu;
    bool service;
    char output[40];
};

struct cpu_sample { uint64_t busy, idle; bool online; };
struct scheduler_sample {
    struct cpu_sample cpus[SCHEDULER_MAX_CPUS];
    unsigned cpu_count;
    struct task_info tasks[TASK_SLOTS];
    uint64_t timestamp, idle_runtime, seconds;
};
void scheduler_sample(struct scheduler_sample *sample);
struct user_space;
void scheduler_user(struct user_space *space);
void scheduler_start(void);
void scheduler_wait_input(void);
void scheduler_sleep(unsigned ticks);
uint64_t scheduler_ticks(void);
uint64_t scheduler_tick_period_ns(void);
unsigned scheduler_spawn(const char *name, void (*entry)(void *), void *context, void (*release)(void *));
void scheduler_reap(void);
void scheduler_output(void *unused, const char *text);
bool scheduler_stop(unsigned id);
bool scheduler_info(unsigned slot, struct task_info *info);
uint64_t scheduler_seconds(void);

void scheduler_task_memory(unsigned id, uint64_t bytes);

unsigned scheduler_spawn_service(const char *name, void (*entry)(void *), void *context, uint64_t owned_bytes);
unsigned scheduler_current_id(void);
void scheduler_current_name(const char *name);
// These three helpers require local interrupts disabled; block resumes masked.
void scheduler_block(void);
void scheduler_wake(unsigned id);
void scheduler_notify_input(void);

void scheduler_wait_reap(void);

void scheduler_secondary_start(void);

void scheduler_charge_current_memory(int64_t bytes);
