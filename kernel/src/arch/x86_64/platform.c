#include "arch/cpu.h"
#include "arch/x86_64/user.h"
#include "arch/x86_64/io.h"
#include "lib/memory.h"
#define PIT_HZ 1193182u
#define DIVISOR 11932u
struct gate {
    uint16_t low, selector;
    uint8_t ist, flags;
    uint16_t middle;
    uint32_t high, reserved;
} __attribute__((packed));

static struct gate idt[256];
extern uintptr_t exception_vectors[32];
extern void syscall_irq(void), scheduler_irq(void), keyboard_irq(void), yield_irq(void);
uintptr_t arch_initial_frame(uintptr_t top, void (*entry)(void)) {
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

void arch_scheduler_init(void) {
    uint16_t cs;
    asm volatile("mov %%cs,%0" : "=r"(cs));
    user_cpu_init();
    for (unsigned i = 0; i < 32; ++i) install_gate(i, exception_vectors[i], cs);
    install_gate(128, (uintptr_t)syscall_irq, cs);
    idt[128].flags = 0xee;
    install_gate(32, (uintptr_t)scheduler_irq, cs);
    install_gate(33, (uintptr_t)keyboard_irq, cs);
    install_gate(48, (uintptr_t)yield_irq, cs);
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
}
void arch_timer_eoi(void) { outb(0x20, 0x20); }
void arch_keyboard_eoi(void) { outb(0x20, 0x20); }

void arch_early_init(void) { arch_own_page_tables(); }
