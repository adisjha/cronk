#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define CACHE_INFO_MAX 14u
struct cache_info {
    unsigned level, kind, line, ways; // kind: 1 data, 2 instruction, 3 unified
    uint64_t bytes;
};
unsigned arch_cache_info(struct cache_info info[CACHE_INFO_MAX]);
#if defined(__aarch64__)
// Recover only from an optional cache-register probe, not arbitrary faults.
bool arm_cache_probe_fault(uint64_t *frame, uint64_t esr);
#endif
