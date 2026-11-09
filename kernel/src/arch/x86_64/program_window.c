#include "program_window.h"
#include "mm/pages.h"

uint64_t *program_window_entry(const void *window) {
    uintptr_t root, cr4;
    asm volatile("mov %%cr3, %0" : "=r"(root));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    const uint64_t physical_mask = 0x000ffffffffff000ULL;
    uintptr_t address = (uintptr_t)window;
    uint64_t *table = pages_direct(root & physical_mask);
    unsigned shift = (cr4 & (1u << 12)) ? 48 : 39;
    for (; shift > 12; shift -= 9) {
        uint64_t entry = table[(address >> shift) & 511];
        if (!(entry & 1) || (entry & (1u << 7))) return NULL;
        table = pages_direct(entry & physical_mask);
    }
    uint64_t *leaf = &table[(address >> 12) & 511];
    return (*leaf & 1) ? leaf : NULL;
}

void program_window_set(const void *window, uint64_t *entry, uint64_t value) {
    *entry = value;
    asm volatile("invlpg (%0)" :: "r"(window) : "memory");
}
