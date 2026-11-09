#include "scheduler.h"
#include "drivers/keyboard/keyboard.h"
#include "arch/x86_64/clock.h"
#include "arch/x86_64/io.h"
#include "lib/memory.h"

#define PIT_HZ 1193182u
#define DIVISOR 11932u
#define STACK_SIZE 8192

struct gate {
    uint16_t low, selector;
    uint8_t ist, flags;
    uint16_t middle;
    uint32_t high, reserved;
} __attribute__((packed));

static struct gate idt[256];
static struct task {
    struct task_info info;
    uint64_t wake_tick;
    uintptr_t sp;
    void (*entry)(void *);
    void *context;
    void (*release)(void *);
} tasks[TASK_SLOTS + 1]; // Last slot has a dedicated idle stack.
static unsigned char idle_stack[STACK_SIZE] __attribute__((aligned(16)));
static unsigned char stacks[TASK_SLOTS-1][STACK_SIZE] __attribute__((aligned(16)));
static unsigned current, next_id = 2;
static volatile uint64_t ticks;
static volatile bool reap_pending;
static bool waiting_for_input;

// Only the shell reaps, so allocator mutations stay serialized.
void scheduler_reap(void) {
    if (!reap_pending) return;
    asm volatile("cli" ::: "memory");
    reap_pending = false;
    for (unsigned i = 1; i < TASK_SLOTS; ++i) {
        if (!tasks[i].info.id && tasks[i].release) {
            tasks[i].release(tasks[i].context);
            tasks[i].release = NULL;
        }
    }
    asm volatile("sti" ::: "memory");
}

void scheduler_output(void *unused, const char *text) {
    (void)unused;
    char *out = tasks[current].info.output;
    asm volatile("cli" ::: "memory");
    unsigned n = 0;
    while (n + 1 < sizeof(tasks[0].info.output) && text[n] && text[n] != '\n') {
        out[n] = text[n];
        ++n;
    }
    out[n] = 0;
    asm volatile("sti" ::: "memory");
}
extern void scheduler_irq(void), keyboard_irq(void), yield_irq(void);

static void idle(void) {
    for (;;) asm volatile("hlt");
}

static uintptr_t initial_frame(uintptr_t top, void (*entry)(void)) {
    // Match timer.S: 15 registers, then the five long-mode IRET slots.
    uint64_t *frame = (uint64_t *)top - 20;
    memset(frame, 0, 20 * sizeof(*frame));
    frame[15] = (uintptr_t)entry;
    asm volatile("mov %%cs,%0" : "=m"(*(uint16_t *)&frame[16]));
    frame[17] = 0x202;
    frame[18] = top;
    asm volatile("mov %%ss,%0" : "=m"(*(uint16_t *)&frame[19]));
    return (uintptr_t)frame;
}

static void install_gate(unsigned vector, uintptr_t handler, uint16_t cs) {
    idt[vector] = (struct gate){handler, cs, 0, 0x8e, handler >> 16, handler >> 32, 0};
}

static void task_main(void) {
    tasks[current].entry(tasks[current].context);
    asm volatile("cli" ::: "memory");
    tasks[current].info.id = 0;
    reap_pending = true;
    if (waiting_for_input) tasks[0].wake_tick = 0;
    asm volatile("int $48" ::: "memory");
    for (;;) asm volatile("sti; hlt");
}

void scheduler_start(void) {
    asm volatile("cli" ::: "memory");
    uint16_t cs;
    asm volatile("mov %%cs,%0" : "=r"(cs));
    install_gate(32, (uintptr_t)scheduler_irq, cs);
    install_gate(33, (uintptr_t)keyboard_irq, cs);
    install_gate(48, (uintptr_t)yield_irq, cs);
    tasks[TASK_SLOTS].sp = initial_frame((uintptr_t)&idle_stack[STACK_SIZE] - 8, idle);
    struct { uint16_t limit; uintptr_t base; } __attribute__((packed))
        descriptor = {sizeof(idt)-1, (uintptr_t)idt};
    asm volatile("lidt %0" :: "m"(descriptor) : "memory");
    // Use legacy PIC delivery on the boot CPU. Limine can leave the
    // local APIC enabled with its external-interrupt input masked.
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1bu));
    low &= ~((1u << 11) | (1u << 10));
    asm volatile("wrmsr" :: "a"(low), "d"(high), "c"(0x1bu));
    // Remap the legacy PIC and enable PIT channel 0 and the keyboard.
    outb(0x20, 0x11); outb(0xa0, 0x11);
    outb(0x21, 32);   outb(0xa1, 40);
    outb(0x21, 4);    outb(0xa1, 2);
    outb(0x21, 1);    outb(0xa1, 1);
    outb(0x21, 0xfc); outb(0xa1, 0xff);
    outb(0x43, 0x34);
    outb(0x40, DIVISOR & 255);
    outb(0x40, DIVISOR >> 8);
    tasks[0].info = (struct task_info){.id = 1, .name = "shell"};
    asm volatile("sti" ::: "memory");
}

// Called with interrupts disabled and all general registers saved.
uintptr_t scheduler_dispatch(uintptr_t sp) {
    tasks[current].sp = sp;
    unsigned next = current == TASK_SLOTS ? 0 : (current + 1) % TASK_SLOTS;
    for (unsigned n = 0; n < TASK_SLOTS; ++n) {
        if (tasks[next].info.id && tasks[next].wake_tick <= ticks) {
            current = next;
            return tasks[current].sp;
        }
        next = (next + 1) % TASK_SLOTS;
    }
    current = TASK_SLOTS;
    return tasks[current].sp;
}

uintptr_t scheduler_tick(uintptr_t sp) {
    ++ticks;
    // Charge the interval just completed. Voluntary yields do not add slices.
    if (current < TASK_SLOTS) ++tasks[current].info.slices;
    if (tasks[0].wake_tick >= ticks) {
        struct clock_sample sample;
        clock_read(&sample);
    }
    outb(0x20, 0x20);
    return scheduler_dispatch(sp);
}

uintptr_t scheduler_keyboard(uintptr_t sp) {
    keyboard_interrupt();
    if (waiting_for_input && keyboard_pending()) tasks[0].wake_tick = 0;
    outb(0x20, 0x20);
    return current == TASK_SLOTS ? scheduler_dispatch(sp) : sp;
}

void scheduler_wait_input(void) {
    asm volatile("cli" ::: "memory");
    if (!keyboard_pending() && !reap_pending) {
        waiting_for_input = true;
        tasks[current].wake_tick = ticks + 100; // Periodic UI/clock refresh.
        asm volatile("int $48" ::: "memory");
        waiting_for_input = false;
    }
    asm volatile("sti" ::: "memory");
}

void scheduler_sleep(unsigned delay) {
    asm volatile("cli" ::: "memory");
    tasks[current].wake_tick = ticks + delay;
    asm volatile("int $48; sti" ::: "memory");
}

uint64_t scheduler_ticks(void) { return ticks; }

unsigned scheduler_spawn(const char *name, void (*entry)(void *), void *context, void (*release)(void *)) {
    asm volatile("cli" ::: "memory");
    unsigned id = 0;
    for (unsigned i = 1; i < TASK_SLOTS; ++i) {
        if (tasks[i].info.id || tasks[i].release) continue;
        uintptr_t top = (uintptr_t)&stacks[i-1][STACK_SIZE] - 8;
        id = next_id++;
        tasks[i] = (struct task){{.id = id, .name = name}, 0, initial_frame(top, task_main), entry, context, release};
        break;
    }
    asm volatile("sti" ::: "memory");
    return id;
}

bool scheduler_stop(unsigned id) {
    if (id < 2) return false;
    asm volatile("cli" ::: "memory");
    bool found = false;
    for (unsigned i = 1; i < TASK_SLOTS; ++i) {
        if (tasks[i].info.id == id) {
            tasks[i].info.id = 0;
            reap_pending = true;
            found = true;
            break;
        }
    }
    asm volatile("sti" ::: "memory");
    return found;
}

bool scheduler_info(unsigned slot, struct task_info *info) {
    if (slot >= TASK_SLOTS) return false;
    asm volatile("cli" ::: "memory");
    *info = tasks[slot].info;
    asm volatile("sti" ::: "memory");
    return info->id != 0;
}

uint64_t scheduler_seconds(void) {
    return ticks * DIVISOR / PIT_HZ;
}

void scheduler_sample(struct scheduler_sample *sample) {
    asm volatile("cli" ::: "memory");
    sample->ticks = ticks;
    sample->seconds = scheduler_seconds();
    for (unsigned i = 0; i < TASK_SLOTS; ++i) sample->tasks[i] = tasks[i].info;
    asm volatile("sti" ::: "memory");
}
