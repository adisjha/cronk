#pragma once
#include <stdint.h>

// Decimal MB, rounded to two places. Buffers must hold at least 32 bytes.
void format_mb(char text[32], uint64_t bytes);
void format_uint(char text[32], uint64_t value);
