#include "files.h"
#include "program/program.h"
#include "ata.h"
#include "lib/memory.h"
#include "term/terminal.h"

#define SLOTS 64
struct record {
    char name[32];
    char text[480];
};
_Static_assert(sizeof(struct record) == 512, "one record per sector");

// Legacy text records end with zero. Binary records reserve the final three
// bytes for a little-endian payload length and a type tag.
#define BINARY_CAPACITY 477
#define BINARY_TAG 1

void files_install_program(const char *name, unsigned marker, const void *data, size_t size) {
    unsigned char header[512];
    if (!ata_read(0, header) || memcmp(header, "CRONKFS1", 8) || header[marker] ||
        size > BINARY_CAPACITY || size <= 8 || memcmp(data, "CRONK001", 8)) return;
    size_t name_size = 1;
    while (name[name_size - 1]) ++name_size;
    if (name_size > 32) return;
    struct record record;
    unsigned empty = 0;
    bool exists = false;
    for (unsigned slot = 1; slot <= SLOTS; ++slot) {
        if (!ata_read(slot, &record)) return;
        if (!memcmp(record.name, name, name_size)) {
            exists = true;
            break;
        }
        if (!record.name[0] && !empty) empty = slot;
    }
    if (!exists) {
        if (!empty) return;
        memset(&record, 0, sizeof(record));
        memcpy(record.name, name, name_size);
        memcpy(record.text, data, size);
        record.text[477] = size & 255;
        record.text[478] = size >> 8;
        record.text[479] = BINARY_TAG;
        if (!ata_write(empty, &record)) return;
    }
    // Mark installation complete only after the record is durable. Deleting
    // the file later must survive a reboot.
    header[marker] = 1;
    ata_write(0, header);
}

void files_command(struct limine_framebuffer *fb, enum file_action action, char *args) {
    struct record record;
    if (!ata_read(0, &record) || memcmp(&record, "CRONKFS1", 8)) {
        terminal_write(fb, "Cronk data disk unavailable\n");
        return;
    }
    size_t len = 0;
    char *text = args;
    if (action != FILE_LIST) {
        while (*text && *text != ' ') { ++text; ++len; }
        if (!len || len >= sizeof(record.name) || (action != FILE_WRITE && *text)) {
            terminal_write(fb, "use a filename of 1 to 31 characters without spaces\n");
            return;
        }
        if (*text) *text++ = 0;
    }
    unsigned empty = 0, slot = 0;
    bool listed = false;
    for (unsigned i = 1; i <= SLOTS; ++i) {
        if (!ata_read(i, &record)) goto io_error;
        if (!record.name[0]) {
            if (!empty) empty = i;
            continue;
        }
        if (record.name[31] || (record.text[479] && record.text[479] != BINARY_TAG)) {
            terminal_write(fb, "invalid file record\n");
            return;
        }
        if (action == FILE_LIST) {
            terminal_write(fb, record.name);
            terminal_putchar(fb, '\n');
            listed = true;
        } else if (!memcmp(record.name, args, len + 1)) {
            slot = i;
            break;
        }
    }
    if (action == FILE_LIST) {
        if (!listed) terminal_write(fb, "(empty)\n");
        return;
    }
    if (!slot && action != FILE_WRITE) {
        terminal_write(fb, "file not found\n");
        return;
    }
    if (action == FILE_RUN || action == FILE_RUNBG) {
        size_t size = (uint8_t)record.text[477] | ((size_t)(uint8_t)record.text[478] << 8);
        if (record.text[479] != BINARY_TAG || size > BINARY_CAPACITY) {
            terminal_write(fb, "not an executable\n");
        } else {
            program_run(fb, record.name, record.text, size, action == FILE_RUNBG);
        }
        return;
    }
    if (action == FILE_READ && record.text[479] == BINARY_TAG) {
        terminal_write(fb, "binary file\n");
        return;
    }
    if (action == FILE_READ) {
        terminal_write(fb, record.text);
        terminal_putchar(fb, '\n');
        return;
    }
    if (!slot) slot = empty;
    if (!slot) {
        terminal_write(fb, "disk full\n");
        return;
    }
    memset(&record, 0, sizeof(record));
    if (action == FILE_WRITE) {
        size_t n = 0;
        while (text[n]) ++n;
        if (n >= sizeof(record.text)) {
            terminal_write(fb, "file too large\n");
            return;
        }
        memcpy(record.name, args, len);
        memcpy(record.text, text, n);
    }
    if (!ata_write(slot, &record)) goto io_error;
    terminal_write(fb, action == FILE_WRITE ? "saved\n" : "removed\n");
    return;
io_error:
    terminal_write(fb, "disk I/O error\n");
}
