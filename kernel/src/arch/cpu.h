#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct limine_framebuffer;
#if defined(__aarch64__)
#define ARCH_NAME "ARM64"
#define ARCH_KERNEL_LEVEL 1
#define ARCH_USER_LEVEL 0
static inline unsigned arch_cpu_index(void) {
    uint64_t index; asm volatile("mrs %0, tpidr_el1" : "=r"(index)); return (unsigned)index;
}
static inline void arch_bind_cpu(unsigned index) {
    asm volatile("msr tpidr_el1, %0; isb" :: "r"((uint64_t)index) : "memory");
}
void arch_start_secondary_cpus(void);
void arch_release_secondary_cpus(void);
bool arch_boot_reclaim_safe(void);
void arch_cpu_scheduler_init(void);
static inline void arch_irq_disable(void) { asm volatile("msr daifset, #2" ::: "memory"); }
static inline void arch_irq_enable(void) { asm volatile("msr daifclr, #2" ::: "memory"); }
static inline void arch_yield(void) { asm volatile("svc #48" ::: "memory"); }
static inline void arch_pause(void) { asm volatile("yield" ::: "memory"); }
static inline void arch_idle(void) { asm volatile("wfi" ::: "memory"); }
static inline uint64_t arch_timestamp(void) {
    uint64_t t; asm volatile("isb; mrs %0, cntvct_el0" : "=r"(t) :: "memory"); return t;
}
static inline uint64_t arch_counter_frequency(void) {
    uint64_t f; asm volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f;
}
static inline uintptr_t arch_stack_top(uintptr_t end) { return end & ~(uintptr_t)15; }
void arch_early_init(void);
void arch_serial_putc(char c);
struct limine_framebuffer *arch_display_init(struct limine_framebuffer *fallback);
#else
#define ARCH_NAME "x86-64"
static inline unsigned arch_cpu_index(void) { return 0; }
static inline void arch_start_secondary_cpus(void) {}
static inline void arch_release_secondary_cpus(void) {}
static inline bool arch_boot_reclaim_safe(void) { return true; }
#define ARCH_KERNEL_LEVEL 0
#define ARCH_USER_LEVEL 3
static inline void arch_irq_disable(void) { asm volatile("cli" ::: "memory"); }
static inline void arch_irq_enable(void) { asm volatile("sti" ::: "memory"); }
static inline void arch_yield(void) { asm volatile("int $48" ::: "memory"); }
static inline void arch_pause(void) { asm volatile("pause" ::: "memory"); }
static inline void arch_idle(void) { asm volatile("hlt" ::: "memory"); }
static inline uint64_t arch_timestamp(void) {
    uint32_t lo, hi; asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uintptr_t arch_stack_top(uintptr_t end) { return end - 8; }
void arch_early_init(void);
static inline void arch_serial_putc(char c) { (void)c; }
static inline struct limine_framebuffer *arch_display_init(struct limine_framebuffer *fb) { return fb; }
#endif
uintptr_t arch_initial_frame(uintptr_t top, void (*entry)(void));
void arch_scheduler_init(void);
void arch_timer_eoi(void);
void arch_keyboard_eoi(void);

static inline uint64_t arch_irq_save(void) {
    uint64_t flags;
#if defined(__aarch64__)
    asm volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) :: "memory");
#else
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    return flags;
}
static inline void arch_irq_restore(uint64_t flags) {
#if defined(__aarch64__)
    asm volatile("msr daif, %0" :: "r"(flags) : "memory");
#else
    if (flags & (1u << 9)) arch_irq_enable();
#endif
}

void arch_own_page_tables(void);
void arch_use_kernel_tables(void);
