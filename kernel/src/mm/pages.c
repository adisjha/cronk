#include "pages.h"
#include "lib/memory.h"
#include <limine.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request map = { .id = LIMINE_MEMMAP_REQUEST_ID };
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request direct = { .id = LIMINE_HHDM_REQUEST_ID };

// Each usable region holds its descriptor and one owner byte per page at its base.
struct region {
    struct region *next;
    size_t count;
    unsigned char owners[];
};
static struct region *first;
static struct page_stats stats;
static size_t active_program;

void *pages_direct(uint64_t physical) {
    return (void *)(direct.response->offset + physical);
}

uint64_t pages_physical(const void *address) {
    return (uintptr_t)address - direct.response->offset;
}

void pages_init(void) {
    if (!map.response || !direct.response) return;
    struct region **tail = &first;
    for (size_t i = 0; i < map.response->entry_count; ++i) {
        struct limine_memmap_entry *entry = map.response->entries[i];
        if (entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES ||
            entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            stats.boot += entry->length / PAGE_SIZE;
        }
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        uintptr_t base = (entry->base + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t end = entry->base + entry->length;
        if (end <= base || end - base < PAGE_SIZE) continue;
        size_t count = (end - base) / PAGE_SIZE;
        size_t metadata = (sizeof(struct region) + count + PAGE_SIZE - 1) / PAGE_SIZE;
        struct region *region = pages_direct(base);
        region->next = NULL;
        region->count = count;
        memset(region->owners, PAGE_FREE, count);
        memset(region->owners, PAGE_METADATA, metadata);
        *tail = region;
        tail = &region->next;
        stats.managed += count;
        stats.available += count - metadata;
    }
}

struct page_stats pages_stats(void) {
    return stats;
}

void *pages_alloc(size_t count, enum page_owner owner) {
    if (!count || count > stats.available) return NULL;
    for (struct region *r = first; r; r = r->next) {
        size_t run = 0;
        for (size_t i = 0; i < r->count; ++i) {
            run = r->owners[i] == PAGE_FREE ? run + 1 : 0;
            if (run != count) continue;
            size_t start = i + 1 - count;
            memset(r->owners + start, owner, count);
            stats.available -= count;
            active_program += count;
            if (active_program > stats.peak_program) stats.peak_program = active_program;
            void *address = (unsigned char *)r + start * PAGE_SIZE;
            return memset(address, 0, count * PAGE_SIZE);
        }
    }
    return NULL;
}

void pages_free(void *address, size_t count) {
    uintptr_t addr = (uintptr_t)address;
    if (!count || addr % PAGE_SIZE) return;
    for (struct region *r = first; r; r = r->next) {
        if (addr < (uintptr_t)r) continue;
        size_t start = (addr - (uintptr_t)r) / PAGE_SIZE;
        if (start >= r->count || count > r->count - start) continue;
        for (size_t i = start; i < start + count; ++i) {
            if (r->owners[i] != PAGE_CODE && r->owners[i] != PAGE_STACK) return;
        }
        memset(r->owners + start, PAGE_FREE, count);
        stats.available += count;
        active_program -= count;
        return;
    }
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
