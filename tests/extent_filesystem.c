#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef memcpy
#undef memset
#undef memmove
#include "../kernel/src/storage/extentfs.c"
#define IMAGE_BYTES (1024 * 1024)
static unsigned char image_data[IMAGE_BYTES], baseline[IMAGE_BYTES];
static int fail_write = -1, fail_flush = -1, writes, flushes;
bool disk_read_many(uint32_t sector, unsigned count, void *p) {
    if ((uint64_t)sector * 512 + count * 512 > IMAGE_BYTES)
        return false;
    memcpy(p, image_data + sector * 512, count * 512);
    return true;
}
bool disk_read(uint32_t sector, void *p) { return disk_read_many(sector, 1, p); }
bool disk_write_many(uint32_t sector, unsigned count, const void *p) {
    if (writes++ == fail_write)
        return false;
    if ((uint64_t)sector * 512 + count * 512 > IMAGE_BYTES)
        return false;
    memcpy(image_data + sector * 512, p, count * 512);
    return true;
}
bool disk_flush(void) { return flushes++ != fail_flush; }
void terminal_write(struct limine_framebuffer *fb, const char *s) {
    (void)fb;
    (void)s;
}
void terminal_putchar(struct limine_framebuffer *fb, char c) {
    (void)fb;
    (void)c;
}
void program_run(struct limine_framebuffer *fb, const char *n, const void *p, size_t s, bool b) {
    (void)fb;
    (void)n;
    (void)p;
    (void)s;
    (void)b;
}
static void format(void) {
    memset(image_data, 0, sizeof(image_data));
    memcpy(image_data, "CRONKFS2", 8);
    uint32_t *h = (void *)image_data;
    h[2] = 2;
    h[3] = 4096;
    h[4] = IMAGE_BYTES / 4096;
    struct extent_bank b = {0};
    memcpy(b.magic, "CRONKDIR", 8);
    b.generation = 1;
    b.checksum = bank_hash(&b);
    memcpy(image_data + 4096, &b, sizeof(b));
    current_dir = 0;
    fail_write = fail_flush = -1;
    writes = flushes = 0;
}
static const char *command(enum file_action action, const char *text) {
    char args[256];
    snprintf(args, sizeof(args), "%s", text);
    return operate(NULL, action, args);
}
int main(void) {
    static char payload[65536], output[65536];
    size_t n;
    for (unsigned i = 0; i < 65535; ++i)
        payload[i] = 'a' + i % 26;
    format();
    assert(!extentfs_save("large.txt", payload, 65535));
    assert(!extentfs_load("large.txt", output, &n) && n == 65535 && !memcmp(payload, output, n));
    assert(mount() && entry(1)->pages == 16 && entry(1)->start >= FIRST_DATA);
    assert(extentfs_save("too-large", payload, 65536));
    assert(!strcmp(command(FILE_MKDIR, "docs"), "folder created"));
    assert(!strcmp(command(FILE_COPY, "large.txt docs/copy.txt"), "copied"));
    assert(!strcmp(command(FILE_MOVE, "docs/copy.txt docs/renamed.txt"), "moved"));
    assert(!extentfs_load("/docs/renamed.txt", output, &n) && n == 65535 &&
           !memcmp(payload, output, n));
    assert(!strcmp(command(FILE_RMDIR, "docs"), "folder is not empty"));
    assert(!strcmp(command(FILE_MOVE, "docs docs/child"), "cannot move folder inside itself"));
    assert(!extentfs_save("empty.txt", "", 0));
    assert(!extentfs_load("empty.txt", output, &n) && !n);
    // Failure at every payload/bank/header write or flush must leave a valid old or new file.
    format();
    assert(!extentfs_save("atomic.txt", "old version", 11));
    memcpy(baseline, image_data, IMAGE_BYTES);
    for (unsigned failure = 0; failure < 5; ++failure) {
        memcpy(image_data, baseline, IMAGE_BYTES);
        writes = flushes = 0;
        fail_write = failure;
        fail_flush = -1;
        extentfs_save("atomic.txt", payload, 5000);
        fail_write = -1;
        assert(!extentfs_load("atomic.txt", output, &n));
        assert((n == 11 && !memcmp(output, "old version", 11)) ||
               (n == 5000 && !memcmp(output, payload, 5000)));
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
        memcpy(image_data, baseline, IMAGE_BYTES);
        writes = flushes = 0;
        fail_write = -1;
        fail_flush = failure;
        extentfs_save("atomic.txt", payload, 5000);
        fail_flush = -1;
        assert(!extentfs_load("atomic.txt", output, &n));
        assert(n == 11 || n == 5000);
    }
    // A torn newest directory bank must fall back to its intact predecessor.
    memcpy(image_data, baseline, IMAGE_BYTES);
    assert(mount());
    image_data[(1 + active_bank * 2) * 4096 + 600] ^= 1;
    assert(mount());
    // Repeated replacement must recycle obsolete extents instead of leaking disk space.
    format();
    for (unsigned i = 0; i < 80; ++i) {
        payload[0] = 'a' + i % 26;
        assert(!extentfs_save("reuse.txt", payload, 65535));
    }
    assert(!extentfs_load("reuse.txt", output, &n) && !memcmp(payload, output, n));
    // Full volumes fail without modifying directory banks or existing payloads.
    format();
    for (unsigned i = 0; i < 15; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "full%u", i);
        assert(!extentfs_save(name, payload, 65535));
    }
    memcpy(baseline, image_data, IMAGE_BYTES);
    assert(extentfs_save("overflow", payload, 65535));
    assert(!memcmp(baseline, image_data, IMAGE_BYTES));
    // There can be enough free pages in total but no adjacent pair.
    format();
    ((uint32_t *)image_data)[4] = 40;
    for (unsigned i = 0; i < 34; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "f%u", i);
        assert(!extentfs_save(name, payload, 1));
    }
    for (unsigned i = 0; i < 34; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "f%u", i);
        assert(!strcmp(command(FILE_REMOVE, name), "removed"));
    }
    memcpy(baseline, image_data, IMAGE_BYTES);
    assert(!strcmp(extentfs_save("fragmented", payload, 5000), "no contiguous space available"));
    assert(!memcmp(baseline, image_data, IMAGE_BYTES));
    format();
    assert(!extentfs_save("reuse.txt", payload, 65535));
    // Corrupt payloads are rejected, not silently returned.
    assert(mount());
    image_data[entry(1)->start * 4096] ^= 1;
    assert(extentfs_load("reuse.txt", output, &n));
    puts("PASS extents: 64 KiB files, contiguous pages, copy/move/folders, boundary limits, atomic "
         "failure recovery, extent reuse, checksums");
}
