#include "platform.h"
#include "arch/cache.h"
#include "arch/cpu.h"
#include "arch/user.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include "sched/scheduler.h"

static volatile uint32_t *distributor, *controller, *uart;
static uint64_t next_timer[SCHEDULER_MAX_CPUS];
extern char arm_vectors[];
extern uintptr_t scheduler_dispatch(uintptr_t), scheduler_tick(uintptr_t);
extern uintptr_t arm_user_service(uint64_t *);

// QEMU virt uses fixed device ranges. Add Device-nGnRE mappings to the
// bootloader's kernel tables; never access MMIO through cached RAM aliases.
void *arm_mmio(uintptr_t physical) {
    uintptr_t va = (uintptr_t)pages_direct(physical);
    uint64_t root;
    asm volatile("mrs %0, ttbr1_el1" : "=r"(root));
    uint64_t *table = pages_direct(root & 0x0000fffffffff000ULL);
    for (unsigned level = 0; level < 2; ++level) {
        unsigned index = (va >> (39 - level * 9)) & 511;
        if (!(table[index] & 1)) {
            void *next = pages_alloc(1, PAGE_DEVICE);
            if (!next) for (;;) arch_idle();
            table[index] = pages_physical(next) | 3;
        }
        if ((table[index] & 3) != 3) for (;;) arch_idle();
        table = pages_direct(table[index] & 0x0000fffffffff000ULL);
    }
    unsigned index = (va >> 21) & 511;
    uint64_t mair;
    asm volatile("mrs %0, mair_el1" : "=r"(mair));
    mair = (mair & ~(0xffULL << 16)) | (0x04ULL << 16);
    asm volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    table[index] = (physical & ~(uintptr_t)0x1fffff) | 1 | (2u << 2) | (1u << 10) |
                   (1ULL << 53) | (1ULL << 54);
    asm volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
    return (void *)va;
}

void arch_early_init(void) {
    arch_bind_cpu(0);
    arch_own_page_tables();
    uart = arm_mmio(0x09000000);
    distributor = arm_mmio(0x08000000);
    controller = (void *)((uintptr_t)distributor + 0x10000);
    asm volatile("msr vbar_el1, %0; isb" :: "r"(arm_vectors) : "memory");
    arch_serial_putc('\n');
}
void arch_serial_putc(char c) {
    if (!uart) return;
    if (c == '\n') arch_serial_putc('\r');
    while (uart[0x18 / 4] & (1u << 5)) arch_pause();
    uart[0] = (unsigned char)c;
}
int arm_serial_getc(void) {
    if (!uart || (uart[0x18 / 4] & (1u << 4))) return -1;
    return uart[0] & 255;
}
uintptr_t arch_initial_frame(uintptr_t top, void (*entry)(void)) {
    uint64_t *frame = (void *)(top - 272);
    memset(frame, 0, 272);
    frame[32] = (uintptr_t)entry;
    frame[33] = 5; // EL1h, interrupts enabled.
    return (uintptr_t)frame;
}
void arch_scheduler_init(void) {
    user_cpu_init();
    extern void arm_input_init(void);
    arm_input_init();
    distributor[0] = 0;
    unsigned groups = (distributor[1] & 31) + 1;
    for (unsigned i = 0; i < groups; ++i) distributor[(0x180 / 4) + i] = ~0u;
    distributor[0] = 1;
    arch_cpu_scheduler_init();
}
void arch_cpu_scheduler_init(void) {
    // PPI enable/priority and the CPU interface are banked per physical CPU.
    ((volatile uint8_t *)distributor)[0x400 + 27] = 0x80;
    distributor[0x100 / 4] = 1u << 27;
    controller[0x04 / 4] = 0xff;
    controller[0x08 / 4] = 0;
    controller[0] = 1;
    uint64_t timer_interval = arch_counter_frequency() / 100;
    unsigned cpu = arch_cpu_index();
    next_timer[cpu] = arch_timestamp() + timer_interval;
    asm volatile("msr cntv_cval_el0, %0; mov x9, #1; msr cntv_ctl_el0, x9; isb"
                 :: "r"(next_timer[cpu]) : "x9", "memory");
}

void arch_timer_eoi(void) {}
void arch_keyboard_eoi(void) {}
uintptr_t arm_irq(uint64_t *frame) {
    unsigned id = controller[0x0c / 4];
    uintptr_t next = (uintptr_t)frame;
    if ((id & 1023) == 27) {
        uint64_t now = arch_timestamp(), timer_interval = arch_counter_frequency() / 100;
        unsigned cpu = arch_cpu_index();
        next_timer[cpu] += timer_interval;
        if (next_timer[cpu] <= now) next_timer[cpu] = now + timer_interval;
        asm volatile("msr cntv_cval_el0, %0; isb" :: "r"(next_timer[cpu]) : "memory");
        next = scheduler_tick(next);
    }
    if ((id & 1023) < 1020) controller[0x10 / 4] = id;
    asm volatile("dsb sy" ::: "memory");
    return next;
}
uintptr_t arm_sync(uint64_t *frame) {
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    bool user = (frame[33] & 15) == 0;
    if (!user && arm_cache_probe_fault(frame, esr)) return (uintptr_t)frame;
    if ((esr >> 26) == 0x15) {
        if (!user && (esr & 0xffff) == 48) return scheduler_dispatch((uintptr_t)frame);
        if (user && !(esr & 0xffff)) return arm_user_service(frame);
    }
    extern void arm_user_fault(unsigned fault) __attribute__((noreturn));
    if (user) arm_user_fault((esr >> 26) + 1);
    // Kernel faults are diagnostic and fatal; do not resume a corrupt context.
    const char *message = "\nARM64 kernel exception ESR/ELR/FAR: ";
    while (*message) arch_serial_putc(*message++);
    uint64_t far;
    asm volatile("mrs %0, far_el1" : "=r"(far));
    uint64_t values[] = {esr, frame[32], far};
    for (unsigned i = 0; i < 3; ++i) {
        for (int bit = 60; bit >= 0; bit -= 4) arch_serial_putc("0123456789abcdef"[(values[i] >> bit) & 15]);
        arch_serial_putc(' ');
    }
    for (;;) { arch_irq_disable(); arch_idle(); }
}
