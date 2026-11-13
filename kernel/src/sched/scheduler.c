#include "scheduler.h"
#include "arch/cpu.h"
#include "arch/user.h"
#include "drivers/keyboard/keyboard.h"
#include "lib/memory.h"
#define STACK_SIZE 8192
struct task {
    struct task_info info;
    uint64_t wake_tick;
    uintptr_t sp;
    void (*entry)(void *);
    void *context;
    void (*release)(void *);
    struct user_space *user;
    bool waiting_input, running, stopping;
};
static struct task tasks[TASK_SLOTS];
struct scheduler_cpu {
    unsigned current;
    uint64_t accounted_at, busy, idle;
    struct task idle_task;
    unsigned char idle_stack[STACK_SIZE] __attribute__((aligned(16)));
    bool online;
};
static struct scheduler_cpu cpus[SCHEDULER_MAX_CPUS];
static unsigned char stacks[TASK_SLOTS - 1][STACK_SIZE] __attribute__((aligned(16)));
static unsigned char service_stacks[SERVICE_SLOTS][SERVICE_STACK_SIZE] __attribute__((aligned(16)));
static unsigned next_id = 2, service_count, placement, reaper_id;
static uint64_t ticks;
static bool reap_pending;
static unsigned scheduler_lock;
static uint64_t lock(void) {
    uint64_t flags = arch_irq_save();
    while (__atomic_exchange_n(&scheduler_lock, 1, __ATOMIC_ACQUIRE))
        arch_pause();
    return flags;
}
static void unlock(uint64_t flags) {
    __atomic_store_n(&scheduler_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(flags);
}
static struct scheduler_cpu *this_cpu(void) { return &cpus[arch_cpu_index()]; }
static struct task *current_task(void) {
    struct scheduler_cpu *c = this_cpu();
    return c->current == TASK_SLOTS ? &c->idle_task : &tasks[c->current];
}
static void account(struct scheduler_cpu *c, uint64_t now) {
    uint64_t elapsed = now - c->accounted_at;
    if (c->current == TASK_SLOTS)
        c->idle += elapsed;
    else {
        c->busy += elapsed;
        tasks[c->current].info.runtime += elapsed;
    }
    c->accounted_at = now;
}
static void wake(unsigned id) {
    for (unsigned i = 0; i < TASK_SLOTS; ++i)
        if (tasks[i].info.id == id)
            tasks[i].wake_tick = 0;
}
static void retire(struct task *t) {
    t->info.id = 0;
    t->running = false;
    t->stopping = false;
    reap_pending = true;
    wake(reaper_id);
}
void scheduler_reap(void) {
    uint64_t flags = lock();
    if (reap_pending) {
        reap_pending = false;
        for (unsigned i = 1; i < TASK_SLOTS; ++i) {
            if (!tasks[i].info.id && !tasks[i].running && tasks[i].release) {
                tasks[i].release(tasks[i].context);
                tasks[i].release = NULL;
            }
        }
    }
    unlock(flags);
}
void scheduler_output(void *unused, const char *text) {
    (void)unused;
    uint64_t flags = lock();
    char *out = current_task()->info.output;
    unsigned n = 0;
    while (n + 1 < sizeof(tasks[0].info.output) && text[n] && text[n] != '\n') {
        out[n] = text[n];
        ++n;
    }
    out[n] = 0;
    unlock(flags);
}
void scheduler_user(struct user_space *space) {
    uint64_t flags = lock();
    current_task()->user = space;
    current_task()->info.ring = space ? ARCH_USER_LEVEL : ARCH_KERNEL_LEVEL;
    unlock(flags);
    user_switch(space);
}
static void idle(void) {
    for (;;)
        arch_idle();
}
static void task_main(void) {
    current_task()->entry(current_task()->context);
    uint64_t flags = lock();
    current_task()->stopping = true;
    unlock(flags);
    arch_irq_disable();
    arch_yield();
    for (;;)
        arch_idle();
}
uintptr_t scheduler_prepare_cpu(unsigned index) {
    if (index >= SCHEDULER_MAX_CPUS)
        return 0;
    struct scheduler_cpu *c = &cpus[index];
    c->current = TASK_SLOTS;
    uintptr_t top = arch_stack_top((uintptr_t)&c->idle_stack[STACK_SIZE]);
    c->idle_task.sp = arch_initial_frame(top, idle);
    return top;
}
void scheduler_start(void) {
    arch_irq_disable();
    scheduler_prepare_cpu(0);
    this_cpu()->current = 0;
    arch_scheduler_init();
    this_cpu()->accounted_at = arch_timestamp();
    this_cpu()->online = true;
    tasks[0].info =
        (struct task_info){.id = 1, .name = "shell", .ram_bytes = 65536, .ring = ARCH_KERNEL_LEVEL};
    tasks[0].running = true;
    arch_irq_enable();
}
void scheduler_secondary_start(void) {
    uint64_t flags = lock();
    this_cpu()->accounted_at = arch_timestamp();
    this_cpu()->online = true;
    unlock(flags);
    arch_irq_enable();
    for (;;)
        arch_idle();
}
uintptr_t scheduler_dispatch(uintptr_t sp) {
    uint64_t flags = lock();
    struct scheduler_cpu *c = this_cpu();
    account(c, arch_timestamp());
    struct task *old = current_task();
    old->sp = sp;
    old->running = false;
    if (old->stopping)
        retire(old);
    unsigned next = c->current == TASK_SLOTS ? 0 : (c->current + 1) % TASK_SLOTS;
    for (unsigned n = 0; n < TASK_SLOTS; ++n) {
        struct task *t = &tasks[next];
        if (t->info.id && !t->stopping && !t->running && t->info.cpu == arch_cpu_index() &&
            t->wake_tick <= ticks) {
            c->current = next;
            t->running = true;
            // Tasks never migrate: no other CPU can select the stack we are leaving.
            uintptr_t result = t->sp;
            struct user_space *user = t->user;
            unlock(flags);
            user_switch(user);
            return result;
        }
        next = (next + 1) % TASK_SLOTS;
    }
    c->current = TASK_SLOTS;
    uintptr_t result = c->idle_task.sp;
    unlock(flags);
    user_switch(NULL);
    return result;
}
uintptr_t scheduler_tick(uintptr_t sp) {
    uint64_t flags = lock();
    if (!arch_cpu_index())
        __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
    if (this_cpu()->current < TASK_SLOTS)
        ++current_task()->info.slices;
    unlock(flags);
    arch_timer_eoi();
    return scheduler_dispatch(sp);
}
uintptr_t scheduler_keyboard(uintptr_t sp) {
    keyboard_interrupt();
    scheduler_notify_input();
    arch_keyboard_eoi();
    return this_cpu()->current == TASK_SLOTS ? scheduler_dispatch(sp) : sp;
}
void scheduler_wait_input(void) {
    uint64_t flags = lock();
    bool wait = !keyboard_pending();
    if (wait) {
        current_task()->waiting_input = true;
        current_task()->wake_tick = ticks + 100;
    }
    unlock(flags);
    // Input may arrive before yield; a zero wake_tick then makes this task runnable.
    if (wait) {
        arch_irq_disable();
        arch_yield();
        flags = lock();
        current_task()->waiting_input = false;
        unlock(flags);
    }
    arch_irq_enable();
}
void scheduler_sleep(unsigned delay) {
    arch_irq_disable();
    uint64_t flags = lock();
    current_task()->wake_tick = ticks + delay;
    unlock(flags);
    arch_yield();
    arch_irq_enable();
}
uint64_t scheduler_ticks(void) { return __atomic_load_n(&ticks, __ATOMIC_RELAXED); }
uint64_t scheduler_tick_period_ns(void) {
#if defined(__aarch64__)
    return 10000000;
#else
    return (uint64_t)11932 * 1000000000 / 1193182;
#endif
}
unsigned scheduler_spawn(const char *name, void (*entry)(void *), void *context,
                         void (*release)(void *)) {
    uint64_t flags = lock();
    unsigned id = 0;
    for (unsigned i = 1; i < TASK_SLOTS; ++i) {
        if (tasks[i].info.id || tasks[i].release || tasks[i].running)
            continue;
        if (next_id >= 1000 && next_id < 1000 + SERVICE_SLOTS)
            next_id = 1000 + SERVICE_SLOTS;
        id = next_id++;
        unsigned cpu;
        do {
            cpu = (++placement) % SCHEDULER_MAX_CPUS;
        } while (!cpus[cpu].online);
        uintptr_t top = arch_stack_top((uintptr_t)&stacks[i - 1][STACK_SIZE]);
        tasks[i] = (struct task){.info = {.id = id,
                                          .name = name,
                                          .ram_bytes = STACK_SIZE,
                                          .ring = ARCH_KERNEL_LEVEL,
                                          .cpu = cpu},
                                 .sp = arch_initial_frame(top, task_main),
                                 .entry = entry,
                                 .context = context,
                                 .release = release};
        break;
    }
    unlock(flags);
    return id;
}
bool scheduler_stop(unsigned id) {
    if (id < 2)
        return false;
    uint64_t flags = lock();
    bool found = false;
    for (unsigned i = 1; i < TASK_SLOTS; ++i)
        if (tasks[i].info.id == id && !tasks[i].info.service) {
            if (tasks[i].running)
                tasks[i].stopping = true;
            else
                retire(&tasks[i]);
            found = true;
            break;
        }
    unlock(flags);
    return found;
}
bool scheduler_info(unsigned slot, struct task_info *info) {
    if (slot >= TASK_SLOTS)
        return false;
    uint64_t flags = lock();
    *info = tasks[slot].info;
    unlock(flags);
    return info->id != 0;
}
uint64_t scheduler_seconds(void) {
    return scheduler_ticks() * scheduler_tick_period_ns() / 1000000000;
}
void scheduler_sample(struct scheduler_sample *sample) {
    uint64_t flags = lock();
    uint64_t now = arch_timestamp();
    memset(sample, 0, sizeof(*sample));
    for (unsigned i = 0; i < SCHEDULER_MAX_CPUS; ++i)
        if (cpus[i].online) {
            account(&cpus[i], now);
            sample->cpus[i] = (struct cpu_sample){cpus[i].busy, cpus[i].idle, true};
            ++sample->cpu_count;
            sample->idle_runtime += cpus[i].idle;
        }
    sample->timestamp = now;
    sample->seconds = scheduler_seconds();
    for (unsigned i = 0; i < TASK_SLOTS; ++i)
        sample->tasks[i] = tasks[i].info;
    unlock(flags);
}
void scheduler_task_memory(unsigned id, uint64_t bytes) {
    uint64_t flags = lock();
    for (unsigned i = 0; i < TASK_SLOTS; ++i)
        if (tasks[i].info.id == id)
            tasks[i].info.ram_bytes = bytes;
    unlock(flags);
}
unsigned scheduler_spawn_service(const char *name, void (*entry)(void *), void *context,
                                 uint64_t owned_bytes) {
    uint64_t flags = lock();
    unsigned id = 0;
    if (service_count < SERVICE_SLOTS)
        for (unsigned i = 1; i < TASK_SLOTS; ++i) {
            if (tasks[i].info.id || tasks[i].release || tasks[i].running)
                continue;
            unsigned service = service_count++;
            id = 1000 + service;
            uintptr_t top = arch_stack_top((uintptr_t)&service_stacks[service][SERVICE_STACK_SIZE]);
            tasks[i] = (struct task){.info = {.id = id,
                                              .name = name,
                                              .ring = ARCH_KERNEL_LEVEL,
                                              .service = true,
                                              .ram_bytes = SERVICE_STACK_SIZE + owned_bytes},
                                     .sp = arch_initial_frame(top, task_main),
                                     .entry = entry,
                                     .context = context};
            break;
        }
    unlock(flags);
    return id;
}
unsigned scheduler_current_id(void) { return current_task()->info.id; }
void scheduler_current_name(const char *name) {
    uint64_t flags = lock();
    current_task()->info.name = name;
    unlock(flags);
}
void scheduler_block(void) {
    uint64_t flags = lock();
    current_task()->wake_tick = UINT64_MAX;
    unlock(flags);
    arch_yield();
    arch_irq_disable();
}
void scheduler_wake(unsigned id) {
    uint64_t flags = lock();
    wake(id);
    unlock(flags);
}
void scheduler_notify_input(void) {
    if (!keyboard_pending())
        return;
    uint64_t flags = lock();
    for (unsigned i = 0; i < TASK_SLOTS; ++i)
        if (tasks[i].waiting_input)
            tasks[i].wake_tick = 0;
    unlock(flags);
}
void scheduler_wait_reap(void) {
    arch_irq_disable();
    uint64_t flags = lock();
    reaper_id = scheduler_current_id();
    bool wait = !reap_pending;
    if (wait)
        current_task()->wake_tick = UINT64_MAX;
    unlock(flags);
    if (wait)
        arch_yield();
    arch_irq_enable();
}

void scheduler_charge_current_memory(int64_t bytes) {
    uint64_t flags = lock();
    current_task()->info.ram_bytes += bytes;
    unlock(flags);
}
