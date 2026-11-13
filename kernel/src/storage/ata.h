#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ata_read_many(uint32_t sector, unsigned count, void *buffer);
// Buffered completion only; caller must flush at the operation boundary.
bool ata_write_many(uint32_t sector, unsigned count, const void *buffer);
bool ata_flush(void);
