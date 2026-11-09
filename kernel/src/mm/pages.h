#pragma once
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096u

enum page_owner { PAGE_FREE, PAGE_METADATA, PAGE_CODE, PAGE_STACK };
struct page_stats {
    size_t managed, available, boot, peak_program;
};
void pages_init(void);
struct page_stats pages_stats(void);
void *pages_alloc(size_t count, enum page_owner owner);
void pages_free(void *address, size_t count);
void *pages_direct(uint64_t physical);
uint64_t pages_physical(const void *address);
const unsigned char *pages_inspect(size_t index, enum page_owner *owner);
