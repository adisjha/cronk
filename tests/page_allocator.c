#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
// Boot request sections are ELF-specific; this harness supplies the responses.
#define section(name) unused
#define PAGE_ALLOCATOR_TEST
#include "../kernel/src/mm/pages.c"
#undef section

int main(void) {
    void *ram = aligned_alloc(PAGE_SIZE, 1024 * PAGE_SIZE);
    assert(ram);
    struct limine_memmap_entry entry = {.base = (uintptr_t)ram, .length = 1024 * PAGE_SIZE,
                                        .type = LIMINE_MEMMAP_USABLE};
    void *boot_ram=aligned_alloc(PAGE_SIZE,64*PAGE_SIZE);assert(boot_ram);
    struct limine_memmap_entry boot_entry={.base=(uintptr_t)boot_ram,.length=64*PAGE_SIZE,.type=LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE};
    struct limine_memmap_entry *entries[] = {&entry,&boot_entry};
    struct limine_memmap_response response = {.entry_count = 2, .entries = entries};
    struct limine_hhdm_response hhdm = {.offset = 0};
    map.response = &response; direct.response = &hhdm;
    pages_init();
    struct page_stats baseline = pages_stats();
    assert(baseline.available < 1024 && baseline.available > 1000);
    assert(!pages_alloc(1, PAGE_FREE)); assert(!pages_alloc(1, PAGE_METADATA));
    assert(!pages_alloc(0, PAGE_BUFFER));
    void *a = pages_alloc(4, PAGE_BUFFER), *b = pages_alloc(8, PAGE_DEVICE);
    void *c = pages_alloc(1, PAGE_CODE), *d = pages_alloc(4, PAGE_STACK);
    assert(a && b && c && d);
    assert(pages_stats().peak_program == 5);
    void *display = pages_alloc(3, PAGE_DISPLAY);
    assert(display);
    struct memory_breakdown parts = pages_breakdown();
    assert(parts.programs == 5 && parts.buffers == 4 && parts.devices == 8 && parts.display == 3);
    assert(parts.kernel_modules + parts.bootloader + parts.metadata + parts.programs +
           parts.buffers + parts.devices + parts.display ==
           pages_stats().boot + pages_stats().managed - pages_stats().available);
    pages_free(display, 3);
    assert(pages_breakdown().display == 0);
    for (size_t i = 0; i < 8 * PAGE_SIZE; ++i) assert(((unsigned char *)b)[i] == 0);
    memset(a, 0xcd, 4 * PAGE_SIZE);
    pages_free(a, 4);
    void *reuse = pages_alloc(4, PAGE_BUFFER);
    assert(reuse == a);
    for (size_t i = 0; i < 4 * PAGE_SIZE; ++i) assert(((unsigned char *)reuse)[i] == 0);
    size_t available = pages_stats().available;
    pages_free((unsigned char *)b + 1, 1); assert(pages_stats().available == available);
    pages_free(ram, 1); assert(pages_stats().available == available); // Metadata is protected.
    pages_free(reuse, 4); pages_free(b, 8); pages_free(c, 1); pages_free(d, 4);
    assert(pages_stats().available == baseline.available);
    pages_free(c, 1); assert(pages_stats().available == baseline.available); // Double free ignored.
    void *slots[1000];
    for (unsigned i = 0; i < 1000; ++i) { slots[i] = pages_alloc(1, PAGE_BUFFER); assert(slots[i]); }
    for (unsigned i = 0; i < 1000; i += 2) pages_free(slots[i], 1);
    // A fragmented prefix must not hide the remaining contiguous tail.
    void *tail = pages_alloc(8, PAGE_BUFFER); assert(tail);
    for (unsigned i = 1; i < 1000; i += 2) pages_free(slots[i], 1);
    pages_free(tail, 8);
    assert(pages_stats().available == baseline.available);
    void *whole = pages_alloc(baseline.available, PAGE_BUFFER); assert(whole);
    assert(!pages_alloc(1, PAGE_BUFFER));
    pages_free(whole, baseline.available);
    assert(pages_stats().available == baseline.available && active_program == 0);
    size_t before_reclaim=pages_stats().available;
    assert(pages_reclaim_boot()==64);
    assert(pages_stats().boot==0&&pages_breakdown().bootloader==0);
    assert(pages_stats().available==before_reclaim+63);
    assert(pages_reclaim_boot()==0);
    free(boot_ram);
    free(ram);
    puts("PASS allocator: ownership, zeroing, reuse, fragmentation, exhaustion, accounting");
}
