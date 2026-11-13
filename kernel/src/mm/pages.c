#include "pages.h"
#include <stdbool.h>
#include "lib/memory.h"
#include <limine.h>
#ifndef PAGE_ALLOCATOR_TEST
#include "arch/cpu.h"
#else
static uint64_t arch_irq_save(void) { return 0; }
static void arch_irq_restore(uint64_t flags) { (void)flags; }
#endif

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request map = { .id = LIMINE_MEMMAP_REQUEST_ID };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request direct = { .id = LIMINE_HHDM_REQUEST_ID };

// Each usable region holds its descriptor and one owner byte per page at its base.
struct region {
    struct region *next;
    size_t count;
    size_t available, hint;
    unsigned char owners[];
};
static struct region *first;
static struct page_stats stats;
static size_t active_program;
static uint64_t direct_offset;
static struct { uint64_t base, length; } boot_ranges[256];
static size_t boot_count;
static bool boot_ranges_complete = true;
#ifndef PAGE_ALLOCATOR_TEST
static unsigned memory_lock;
#endif
static uint64_t memory_enter(void) {
    uint64_t flags = arch_irq_save();
#ifndef PAGE_ALLOCATOR_TEST
    while (__atomic_exchange_n(&memory_lock, 1, __ATOMIC_ACQUIRE)) arch_pause();
#endif
    return flags;
}
static void memory_leave(uint64_t flags) {
#ifndef PAGE_ALLOCATOR_TEST
    __atomic_store_n(&memory_lock, 0, __ATOMIC_RELEASE);
#endif
    arch_irq_restore(flags);
}
static struct memory_breakdown breakdown;
static size_t owners_used[PAGE_DISPLAY + 1];

void *pages_direct(uint64_t physical) {
    return (void *)(direct_offset + physical);
}

uint64_t pages_physical(const void *address) {
    return (uintptr_t)address - direct_offset;
}

static size_t add_region(uint64_t physical, uint64_t length) {
    uintptr_t base=(physical+PAGE_SIZE-1)&~(uintptr_t)(PAGE_SIZE-1);
    uintptr_t end=(physical+length)&~(uintptr_t)(PAGE_SIZE-1);
    if(end<=base || end-base<2*PAGE_SIZE) return 0;
    size_t count=(end-base)/PAGE_SIZE;
    size_t metadata=(sizeof(struct region)+count+PAGE_SIZE-1)/PAGE_SIZE;
    struct region *r=pages_direct(base);
    r->next=first; r->count=count; r->available=count-metadata; r->hint=metadata;
    memset(r->owners,PAGE_FREE,count); memset(r->owners,PAGE_METADATA,metadata);
    first=r; breakdown.metadata+=metadata; stats.managed+=count; stats.available+=count-metadata;
    return count;
}
void pages_init(void) {
    if(!map.response || !direct.response) return;
    direct_offset=direct.response->offset;
    for(size_t i=0;i<map.response->entry_count;++i) {
        struct limine_memmap_entry *e=map.response->entries[i];
        if(e->type==LIMINE_MEMMAP_EXECUTABLE_AND_MODULES || e->type==LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            stats.boot+=e->length/PAGE_SIZE;
            if(e->type==LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
                breakdown.bootloader+=e->length/PAGE_SIZE;
                if(boot_count<256) { boot_ranges[boot_count].base=e->base; boot_ranges[boot_count++].length=e->length; }
                else boot_ranges_complete=false;
            } else breakdown.kernel_modules+=e->length/PAGE_SIZE;
        }
        if(e->type==LIMINE_MEMMAP_USABLE) add_region(e->base,e->length);
    }
}
size_t pages_reclaim_boot(void) {
    uint64_t flags=memory_enter();
    if(!boot_ranges_complete) { memory_leave(flags); return 0; }
    size_t reclaimed=0;
#ifndef PAGE_ALLOCATOR_TEST
#if !defined(__aarch64__)
    uintptr_t sp; asm volatile("mov %%rsp,%0":"=r"(sp));
    uintptr_t keep_start=(pages_physical((void *)sp)-65536)&~(uintptr_t)4095;
    uintptr_t keep_end=(pages_physical((void *)sp)+65536+4095)&~(uintptr_t)4095;
#endif
#endif
    for(size_t i=0;i<boot_count;++i) {
        uint64_t start=boot_ranges[i].base, end=start+boot_ranges[i].length;
#ifndef PAGE_ALLOCATOR_TEST
#if !defined(__aarch64__)
        if(start<keep_end && end>keep_start) {
            if(start<keep_start) reclaimed+=add_region(start,keep_start-start);
            if(end>keep_end) reclaimed+=add_region(keep_end,end-keep_end);
            continue;
        }
#endif
#endif
        reclaimed+=add_region(start,end-start);
    }
    boot_count=0;
    stats.boot-=reclaimed; breakdown.bootloader-=reclaimed;
    memory_leave(flags); return reclaimed;
}

struct page_stats pages_stats(void) {
    uint64_t flags = memory_enter();
    struct page_stats result = stats;
    memory_leave(flags);
    return result;
}

void *pages_alloc(size_t count, enum page_owner owner) {
    if (!count || owner < PAGE_CODE || owner > PAGE_DISPLAY) return NULL;
    uint64_t flags = memory_enter();
    if (count > stats.available) { memory_leave(flags); return NULL; }
    for (struct region *r = first; r; r = r->next) {
        if (count > r->available) continue;
        size_t run = 0;
        for (size_t i = r->hint; i < r->count; ++i) {
            run = r->owners[i] == PAGE_FREE ? run + 1 : 0;
            if (run != count) continue;
            size_t start = i + 1 - count;
            memset(r->owners + start, owner, count);
            stats.available -= count;
            owners_used[owner] += count;
            r->available -= count;
            while (r->hint < r->count && r->owners[r->hint] != PAGE_FREE) ++r->hint;
            if (owner == PAGE_CODE || owner == PAGE_STACK) active_program += count;
            if (active_program > stats.peak_program) stats.peak_program = active_program;
            void *address = (unsigned char *)r + start * PAGE_SIZE;
            memory_leave(flags);
            return memset(address, 0, count * PAGE_SIZE);
        }
    }
    memory_leave(flags);
    return NULL;
}

void pages_free(void *address, size_t count) {
    uintptr_t addr = (uintptr_t)address;
    if (!count || addr % PAGE_SIZE) return;
    uint64_t flags = memory_enter();
    for (struct region *r = first; r; r = r->next) {
        if (addr < (uintptr_t)r) continue;
        size_t start = (addr - (uintptr_t)r) / PAGE_SIZE;
        if (start >= r->count || count > r->count - start) continue;
        size_t program_pages = 0;
        for (size_t i = start; i < start + count; ++i) {
            if (r->owners[i] < PAGE_CODE || r->owners[i] > PAGE_DISPLAY) { memory_leave(flags); return; }
            if (r->owners[i] == PAGE_CODE || r->owners[i] == PAGE_STACK) ++program_pages;
        }
        for (size_t i = start; i < start + count; ++i) --owners_used[r->owners[i]];
        memset(r->owners + start, PAGE_FREE, count);
        stats.available += count;
        r->available += count;
        if (start < r->hint) r->hint = start;
        active_program -= program_pages;
        memory_leave(flags);
        return;
    }
    memory_leave(flags);
}

const unsigned char *pages_inspect(size_t index, enum page_owner *owner) {
    for (struct region *r = first; r; r = r->next) {
        if (index < r->count) {
            *owner = r->owners[index];
            return (const unsigned char *)r + index * PAGE_SIZE;
        }
        index -= r->count;
    }
    return NULL;
}

struct memory_breakdown pages_breakdown(void) {
    uint64_t flags = memory_enter();
    struct memory_breakdown result = breakdown;
    result.programs = owners_used[PAGE_CODE] + owners_used[PAGE_STACK];
    result.buffers = owners_used[PAGE_BUFFER];
    result.devices = owners_used[PAGE_DEVICE];
    result.display = owners_used[PAGE_DISPLAY];
    memory_leave(flags);
    return result;
}
