#include "arch/cpu.h"
#include "lib/memory.h"
#include "mm/pages.h"
static uintptr_t owned_root;
#if defined(__aarch64__)
static uint64_t kernel_mair;
#endif
static uintptr_t clone(uintptr_t source, unsigned level, unsigned levels) {
    uint64_t *copy = pages_alloc(1, PAGE_DEVICE);
    if (!copy)
        return 0;
    memcpy(copy, pages_direct(source), PAGE_SIZE);
    if (level + 1 < levels)
        for (unsigned i = 0; i < 512; ++i) {
#if defined(__aarch64__)
            bool table = (copy[i] & 3) == 3;
            uint64_t mask = 0x0000fffffffff000ULL;
#else
            bool table = (copy[i] & 1) && !(copy[i] & (1u << 7));
            uint64_t mask = 0x000ffffffffff000ULL;
#endif
            if (!table)
                continue;
            uintptr_t next = clone(copy[i] & mask, level + 1, levels);
            if (!next)
                return 0;
            copy[i] = (copy[i] & ~mask) | next;
        }
    return pages_physical(copy);
}
void arch_own_page_tables(void) {
    uintptr_t root;
    unsigned levels;
#if defined(__aarch64__)
    asm volatile("mrs %0, ttbr1_el1" : "=r"(root));
    levels = 4;
    root &= 0x0000fffffffff000ULL;
#else
    uintptr_t cr4;
    asm volatile("mov %%cr3,%0; mov %%cr4,%1" : "=r"(root), "=r"(cr4));
    levels = (cr4 & (1u << 12)) ? 5 : 4;
    root &= ~4095ULL;
#endif
    owned_root = clone(root, 0, levels);
    // Never reclaim boot tables unless the replacement is complete.
    if (!owned_root)
        for (;;)
            arch_idle();
    arch_use_kernel_tables();
}
void arch_use_kernel_tables(void) {
#if defined(__aarch64__)
    // BSP mappings may add a Device MAIR slot before AP startup.
    if (!arch_cpu_index())
        asm volatile("mrs %0, mair_el1" : "=r"(kernel_mair));
    asm volatile(
        "msr mair_el1,%0; dsb ishst; msr ttbr1_el1,%1; isb; tlbi vmalle1; dsb ish; isb" ::"r"(
            kernel_mair),
        "r"(owned_root)
        : "memory");
#else
    asm volatile("mov %0,%%cr3" ::"r"(owned_root) : "memory");
#endif
}
