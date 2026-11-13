#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void *kernel_memcpy(void *, const void *, size_t);
void *kernel_memset(void *, int, size_t);
void *kernel_memmove(void *, const void *, size_t);
int kernel_memcmp(const void *, const void *, size_t);
uint64_t memory_sum64(const void *, size_t);

static const size_t sizes[] = {0,1,2,3,4,7,8,9,15,16,17,31,32,33,63,64,65,
    127,128,129,255,256,257,511,512,513,1023,1024,1025,2048};
static void pattern(unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = (i * 53 + i / 7) & 255;
}
int main(void) {
    unsigned char src[4096], actual[4096], expected[4096];
    pattern(src, sizeof(src));
    for (unsigned k = 0; k < sizeof(sizes)/sizeof(*sizes); ++k) {
        size_t n = sizes[k];
        for (unsigned a = 0; a < 16; ++a) for (unsigned b = 0; b < 16; ++b) {
            memset(actual, 0xa7, sizeof(actual)); memcpy(expected, actual, sizeof(actual));
            assert(kernel_memcpy(actual + a, src + b, n) == actual + a);
            memcpy(expected + a, src + b, n);
            assert(!memcmp(actual, expected, sizeof(actual)));
            assert(kernel_memset(actual + a, 0x15a, n) == actual + a);
            memset(expected + a, 0x5a, n);
            assert(!memcmp(actual, expected, sizeof(actual)));
            uint64_t sum = 0;
            for (size_t i = 0; i < n / 8; ++i) {
                uint64_t value; memcpy(&value, src + b + i * 8, 8); sum += value;
            }
            assert(memory_sum64(src + b, n / 8) == sum);
            assert(!kernel_memcmp(actual, expected, sizeof(actual)));
        }
        for (unsigned a = 0; a < 32; ++a) for (unsigned b = 0; b < 32; ++b) {
            pattern(actual, sizeof(actual)); memcpy(expected, actual, sizeof(actual));
            assert(kernel_memmove(actual + a, actual + b, n) == actual + a);
            memmove(expected + a, expected + b, n);
            assert(!memcmp(actual, expected, sizeof(actual)));
        }
    }
    // Inaccessible pages catch even a single byte read/written past an end.
    size_t page = sysconf(_SC_PAGESIZE);
    unsigned char *a = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *b = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(a != MAP_FAILED && b != MAP_FAILED);
    assert(!mprotect(a + page, page, PROT_READ | PROT_WRITE));
    assert(!mprotect(b + page, page, PROT_READ | PROT_WRITE));
    pattern(a + page, page);
    for (unsigned k = 0; k < sizeof(sizes)/sizeof(*sizes); ++k) {
        size_t n = sizes[k];
        unsigned char *src_end = a + page * 2 - n, *dst_end = b + page * 2 - n;
        kernel_memcpy(dst_end, src_end, n);
        assert(!memcmp(dst_end, src_end, n));
        kernel_memset(dst_end, 0x43, n);
        kernel_memmove(dst_end, dst_end, n);
        memory_sum64(src_end, n / 8);
    }
    munmap(a, page * 3); munmap(b, page * 3);
    puts("PASS memory: alignment, sizes, overlap, fill truncation, checksums, guard pages");
}
