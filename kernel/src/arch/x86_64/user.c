#include "user.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include "sched/scheduler.h"
#include <limine.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request kernel_address = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID
};

// A private low-half translation tree per program. All upper-half kernel
// mappings are shared, with U/S cleared at the root of each tree.
static uint64_t tables[TASK_SLOTS][5][512] __attribute__((aligned(4096)));
static uintptr_t kernel_root, kernel_virtual, kernel_physical;
static unsigned levels;
static struct user_space *active;
static uint64_t gdt[20];
static struct __attribute__((packed)) {
    uint32_t reserved;
    uint64_t rsp[3], reserved2, ist[7], reserved3;
    uint16_t reserved4, iomap;
} tss;

extern unsigned user_enter(uintptr_t *resume, void *rsp0);
extern void user_leave(uintptr_t resume, unsigned fault) __attribute__((noreturn));

void user_cpu_init(void) {
    kernel_virtual=kernel_address.response->virtual_base;
    kernel_physical=kernel_address.response->physical_base;
    uintptr_t cr4;
    asm volatile("mov %%cr3,%0" : "=r"(kernel_root));
    asm volatile("mov %%cr4,%0" : "=r"(cr4));
    levels = cr4 & (1u << 12) ? 5 : 4;
    // Cronk has no per-task FPU/SIMD context yet; trap its use rather than
    // sharing floating-point registers across protection domains.
    uintptr_t cr0;
    asm volatile("mov %%cr0,%0" : "=r"(cr0));
    asm volatile("mov %0,%%cr0" :: "r"(cr0 | (1UL << 3)) : "memory");
    uint16_t cs, ss;
    asm volatile("mov %%cs,%0; mov %%ss,%1" : "=r"(cs), "=r"(ss));
    gdt[cs >> 3] = 0x00af9a000000ffffULL;
    gdt[ss >> 3] = 0x00cf92000000ffffULL;
    gdt[16] = 0x00affa000000ffffULL; // user CS 0x83
    gdt[17] = 0x00cff2000000ffffULL; // user SS 0x8b
    uintptr_t base = (uintptr_t)&tss;
    tss.iomap = sizeof(tss); // No I/O permissions bitmap.
    gdt[18] = (sizeof(tss)-1) | ((base & 0xffffff) << 16) |
              (0x89ULL << 40) | ((base & 0xff000000) << 32);
    gdt[19] = base >> 32;
    struct { uint16_t limit; uintptr_t base; } __attribute__((packed))
        gdtr = {sizeof(gdt)-1, (uintptr_t)gdt};
    asm volatile("lgdt %0; ltr %w1" :: "m"(gdtr), "r"(0x90) : "memory");
}

static uintptr_t table_phys(void *p) {
    return (uintptr_t)p - kernel_virtual + kernel_physical;
}

void user_space_init(struct user_space *s, unsigned slot) {
    uint64_t (*tree)[512] = tables[slot];
    memset(tree, 0, sizeof(tables[slot]));
    const uint64_t *original = pages_direct(kernel_root & ~4095UL);
    for (unsigned i = 256; i < 512; ++i) tree[0][i] = original[i] & ~4ULL;
    for (unsigned i = 0; i + 1 < levels; ++i) {
        unsigned shift = 12 + 9 * (levels - 1 - i);
        tree[i][(USER_CODE >> shift) & 511] = table_phys(tree[i+1]) | 7;
    }
    uint64_t *leaf = tree[levels-1];
    leaf[0] = pages_physical(s->code) | 5; // read/execute
    for (unsigned i = 0; i < 4; ++i)
        leaf[2+i] = (pages_physical(s->stack) + i * PAGE_SIZE) | 7 | (1ULL << 63);
    s->root = table_phys(tree[0]);
}

void user_switch(struct user_space *s) {
    if (active == s) return;
    active = s;
    tss.rsp[0] = s ? s->resume : 0;
    uintptr_t root = s ? s->root : kernel_root;
    asm volatile("mov %0,%%cr3" :: "r"(root) : "memory");
}

unsigned user_execute(struct user_space *s) {
    asm volatile("cli" ::: "memory");
    scheduler_user(s);
    unsigned fault = user_enter(&s->resume, (char *)&tss + 4);
    scheduler_user(NULL);
    asm volatile("sti" ::: "memory");
    return fault;
}

// timer.S saves r15..rax followed by the hardware IRET frame.
uintptr_t user_service(uintptr_t sp) {
    uint64_t *f = (uint64_t *)sp;
    if (!active || (f[16] & 3) != 3) __builtin_trap();
    if (f[14] == 0) user_leave(active->resume, 0);
    if (f[14] == 2) {
        scheduler_sleep(1);
    } else if (f[14] == 1) {
        uintptr_t ptr = f[8]; // rsi
        const char *src = NULL;
        uintptr_t remaining = 0;
        if (ptr >= USER_CODE && ptr < USER_CODE + PAGE_SIZE) {
            src = (const char *)active->code + (ptr - USER_CODE);
            remaining = USER_CODE + PAGE_SIZE - ptr;
        } else if (ptr >= USER_STACK && ptr < USER_TOP) {
            src = (const char *)active->stack + (ptr - USER_STACK);
            remaining = USER_TOP - ptr;
        }
        size_t n = 0;
        while (n < remaining && src[n]) ++n;
        if (!src || n == remaining) user_leave(active->resume, 14 + 1);
        active->write(active->context, src);
    } else user_leave(active->resume, 6 + 1);
    return sp;
}

void user_exception(uint64_t *f) {
    // vector, error, rip, cs, rflags, rsp, ss
    if (active && (f[3] & 3) == 3) {
        // Original counter.bin used HLT to wait. Translate that single
        // instruction into a scheduler sleep for already-installed copies.
        if (f[0] == 13 && f[2] >= USER_CODE && f[2] < USER_CODE + PAGE_SIZE &&
            active->code[f[2]-USER_CODE] == 0xf4) {
            ++f[2];
            scheduler_sleep(1);
            return;
        }
        user_leave(active->resume, (unsigned)f[0] + 1);
    }
    for (;;) asm volatile("cli; hlt");
}
