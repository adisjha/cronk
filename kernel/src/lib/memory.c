#include "memory.h"

// x86-64 and AArch64 normal RAM permit unaligned accesses. may_alias keeps bulk
// accesses valid for arbitrary C objects; aligned(1) permits byte alignment.
typedef uint64_t word __attribute__((may_alias, aligned(1)));

static void forward(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 64) {
        word a = ((const word *)s)[0], b = ((const word *)s)[1];
        word c = ((const word *)s)[2], e = ((const word *)s)[3];
        word f = ((const word *)s)[4], g = ((const word *)s)[5];
        word h = ((const word *)s)[6], j = ((const word *)s)[7];
        ((word *)d)[0] = a; ((word *)d)[1] = b;
        ((word *)d)[2] = c; ((word *)d)[3] = e;
        ((word *)d)[4] = f; ((word *)d)[5] = g;
        ((word *)d)[6] = h; ((word *)d)[7] = j;
        d += 64; s += 64; n -= 64;
    }
    while (n >= 8) { *(word *)d = *(const word *)s; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    forward(dest, src, n);
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    uint8_t *d = dest;
    uint64_t value = (uint8_t)c * 0x0101010101010101ULL;
    while (n >= 64) {
        ((word *)d)[0] = value; ((word *)d)[1] = value;
        ((word *)d)[2] = value; ((word *)d)[3] = value;
        ((word *)d)[4] = value; ((word *)d)[5] = value;
        ((word *)d)[6] = value; ((word *)d)[7] = value;
        d += 64; n -= 64;
    }
    while (n >= 8) { *(word *)d = value; d += 8; n -= 8; }
    while (n--) *d++ = (uint8_t)c;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    if ((uintptr_t)d <= (uintptr_t)s) forward(d, s, n);
    else {
        d += n; s += n;
        while (n >= 64) {
            for (unsigned i = 0; i < 8; ++i) {
                d -= 8; s -= 8; *(word *)d = *(const word *)s;
            }
            n -= 64;
        }
        while (n >= 8) { d -= 8; s -= 8; *(word *)d = *(const word *)s; n -= 8; }
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = s1, *b = s2;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

uint64_t memory_sum64(const void *src, size_t count) {
    const word *s = src;
    uint64_t a = 0, b = 0, c = 0, d = 0;
    // Four independent sums avoid one long dependency chain. All words are read.
    while (count >= 4) { a += s[0]; b += s[1]; c += s[2]; d += s[3]; s += 4; count -= 4; }
    while (count--) a += *s++;
    return a + b + c + d;
}
