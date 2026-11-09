#pragma once
#include <stdint.h>

// Leaf entry for the kernel's page-aligned executable program window.
uint64_t *program_window_entry(const void *window);
void program_window_set(const void *window, uint64_t *entry, uint64_t value);
extern unsigned char program_page[];
