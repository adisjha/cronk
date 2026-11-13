#include "arch/cache.h"
static void cpuid(unsigned leaf, unsigned sub, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
unsigned arch_cache_info(struct cache_info info[CACHE_INFO_MAX]) {
    unsigned a, b, c, d, leaf = 0, count = 0;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 4) leaf = 4;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x8000001d) leaf = 0x8000001d;
    if (!leaf) return 0;
    for (unsigned sub = 0; sub < CACHE_INFO_MAX; ++sub) {
        cpuid(leaf, sub, &a, &b, &c, &d);
        unsigned kind = a & 31;
        if (!kind) break;
        if (kind > 3) continue;
        unsigned level = (a >> 5) & 7, line = (b & 4095) + 1;
        unsigned ways = (b >> 22) + 1, partitions = ((b >> 12) & 1023) + 1;
        uint64_t size = (uint64_t)line * ways * partitions * ((uint64_t)c + 1);
        if (!level || !size || size > (1ULL << 40)) continue;
        info[count++] = (struct cache_info){level, kind, line, ways, size};
    }
    return count;
}
