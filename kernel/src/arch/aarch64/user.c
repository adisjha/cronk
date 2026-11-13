#include "user.h"
#include "arch/cpu.h"
#include "mm/pages.h"
#include "sched/scheduler.h"
#include "lib/memory.h"
#include <limine.h>
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable = {.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID};
static uint64_t tables[TASK_SLOTS][4][512] __attribute__((aligned(4096)));
static uint64_t empty[512] __attribute__((aligned(4096)));
static struct user_space *active_spaces[SCHEDULER_MAX_CPUS];
#define active active_spaces[arch_cpu_index()]
static uint64_t kernel_virtual, kernel_physical;
extern unsigned arm_user_enter(uintptr_t *resume);
extern void arm_user_leave(uintptr_t resume, unsigned fault) __attribute__((noreturn));
static uintptr_t physical(const void *p) {
    return (uintptr_t)p - kernel_virtual + kernel_physical;
}
void user_cpu_init(void) {
    if (!arch_cpu_index()) { kernel_virtual=executable.response->virtual_base; kernel_physical=executable.response->physical_base; }
    asm volatile("msr cpacr_el1, xzr; isb" ::: "memory");
    // A 48-bit TTBR0 address space; keep Limine's TTBR1/kernel translation intact.
    uint64_t tcr;
    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    tcr = (tcr & ~0xffffULL) | 16 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12);
    asm volatile("msr tcr_el1, %0; isb" :: "r"(tcr) : "memory");
    user_switch(NULL);
}
void user_space_init(struct user_space *space, unsigned slot) {
    uint64_t (*tree)[512] = tables[slot];
    memset(tree, 0, sizeof(tables[slot]));
    for (unsigned i = 0; i < 3; ++i)
        tree[i][(USER_CODE >> (39 - 9 * i)) & 511] = physical(tree[i+1]) | 3;
    uint64_t attributes = 3 | (3u << 8) | (1u << 10) | (1ULL << 53);
    tree[3][0] = pages_physical(space->code) | attributes | (3u << 6);
    for (unsigned i = 0; i < 4; ++i)
        tree[3][2+i] = (pages_physical(space->stack) + i * 4096) | attributes | (1u << 6) | (1ULL << 54);
    space->root = physical(tree[0]);
    // Synchronize instruction and data caches after loading program bytes.
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    for (size_t offset = 0; offset < PAGE_SIZE; offset += line)
        asm volatile("dc cvau, %0" :: "r"(space->code + offset) : "memory");
    asm volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
}
void user_switch(struct user_space *space) {
    active = space;
    uintptr_t root = space ? space->root : physical(empty);
    asm volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb ish; isb"
                 :: "r"(root) : "memory");
}
unsigned user_execute(struct user_space *space) {
    arch_irq_disable(); scheduler_user(space);
    unsigned fault = arm_user_enter(&space->resume);
    scheduler_user(NULL); arch_irq_enable();
    return fault;
}
void arm_user_fault(unsigned fault) {
    arm_user_leave(active->resume, fault);
}
uintptr_t arm_user_service(uint64_t *frame) {
    if (frame[8] == 0) arm_user_fault(0);
    if (frame[8] == 2) scheduler_sleep(1);
    else if (frame[8] == 1) {
        uintptr_t pointer = frame[1], remaining = 0;
        const char *src = NULL;
        if (pointer >= USER_CODE && pointer < USER_CODE + PAGE_SIZE) {
            src = (char *)active->code + pointer - USER_CODE;
            remaining = USER_CODE + PAGE_SIZE - pointer;
        } else if (pointer >= USER_STACK && pointer < USER_TOP) {
            src = (char *)active->stack + pointer - USER_STACK;
            remaining = USER_TOP - pointer;
        }
        size_t n = 0;
        while (n < remaining && src[n]) ++n;
        if (!src || n == remaining) arm_user_fault(0x24 + 1);
        active->write(active->context, src);
    } else arm_user_fault(0x15 + 1);
    return (uintptr_t)frame;
}
