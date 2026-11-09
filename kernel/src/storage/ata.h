#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ata_read(uint8_t sector, void *buffer);
bool ata_write(uint8_t sector, const void *buffer);
