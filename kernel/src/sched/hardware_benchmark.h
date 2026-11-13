#pragma once
#include <limine.h>

enum benchmark_part { BENCH_ALL, BENCH_CPU, BENCH_RAM, BENCH_DISK, BENCH_CACHES };
void hardware_benchmark(struct limine_framebuffer *fb, enum benchmark_part part);
void hardware_ram_benchmark(struct limine_framebuffer *fb, const char *size);
