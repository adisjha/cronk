#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISK_BATCH_MAX 64u
void disk_init(void);
const char *disk_backend(void);
bool disk_read_many(uint32_t sector, unsigned count, void *buffer);
// Does not flush: the logical operation must end with disk_flush().
bool disk_write_many(uint32_t sector, unsigned count, const void *buffer);
bool disk_flush(void);
bool disk_read(uint32_t sector, void *buffer);
// Filesystem-facing durable write: returns true only after a successful flush.
bool disk_write(uint32_t sector, const void *buffer);

struct disk_counters { uint64_t reads, writes, flushes; };
struct disk_counters disk_counters(void);

void disk_service_start(uint64_t owned_bytes);
