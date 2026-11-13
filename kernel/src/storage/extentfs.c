// CRONKFS2: 4 KiB pages, contiguous extents, two checksummed directory banks.
#include "extentfs.h"
#include "block.h"
#include "lib/memory.h"
#include "program/program.h"
#include "term/terminal.h"
#define FS_PAGE 4096u
#define FIRST_DATA 5u
#define NOT_FOUND 65u
struct extent_entry {
    char name[32];
    uint32_t parent, kind, size, start, pages, checksum, flags, reserved;
};
struct extent_bank {
    char magic[8];
    uint64_t generation;
    uint32_t checksum;
    uint8_t installed[32], reserved[460];
    struct extent_entry entries[64];
    uint8_t tail[3584];
};
_Static_assert(sizeof(struct extent_entry) == 64, "extent entry size");
_Static_assert(sizeof(struct extent_bank) == 8192, "directory bank size");
static struct extent_bank banks[2];
static bool valid_bank[2];
static unsigned active_bank, volume_pages, current_dir;
static uint8_t file_buffer[FILE_TEXT_MAX + 1];
static uint32_t hash(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint32_t bank_hash(const struct extent_bank *b) {
    const uint8_t *p = (const void *)b;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*b); ++i)
        h = (h ^ ((i >= 16 && i < 20) ? 0 : p[i])) * 16777619u;
    return h;
}
bool extentfs_active(void) {
    uint8_t sector[512];
    return disk_read(0, sector) && !memcmp(sector, "CRONKFS2", 8);
}
static struct extent_entry *entry(unsigned slot) { return &banks[active_bank].entries[slot - 1]; }
static bool validate(const struct extent_bank *b) {
    if (memcmp(b->magic, "CRONKDIR", 8) || !b->generation || b->checksum != bank_hash(b))
        return false;
    for (unsigned i = 0; i < 64; ++i) {
        const struct extent_entry *e = &b->entries[i];
        if (!e->name[0])
            continue;
        if (e->name[31] || e->parent > 64 || e->kind > FOLDER || e->size > FILE_TEXT_MAX)
            return false;
        if (e->pages != (e->size + 4095) / 4096 ||
            (e->pages && (e->start < FIRST_DATA || e->pages > volume_pages ||
                          e->start > volume_pages - e->pages)))
            return false;
        if (!e->pages && e->start)
            return false;
        if (e->kind == FOLDER && e->size)
            return false;
        for (unsigned c = 0; e->name[c]; ++c)
            if (e->name[c] == '/')
                return false;
        if ((e->name[0] == '.' && !e->name[1]) ||
            (e->name[0] == '.' && e->name[1] == '.' && !e->name[2]))
            return false;
        for (unsigned j = 0; j < i; ++j) {
            const struct extent_entry *other = &b->entries[j];
            if (!other->name[0])
                continue;
            if (e->parent == other->parent && !memcmp(e->name, other->name, 32))
                return false;
            if (e->pages && other->pages && e->start < other->start + other->pages &&
                other->start < e->start + e->pages)
                return false;
        }
        unsigned parent = e->parent;
        for (unsigned depth = 0; parent; ++depth) {
            if (depth >= 64 || !b->entries[parent - 1].name[0] ||
                b->entries[parent - 1].kind != FOLDER)
                return false;
            parent = b->entries[parent - 1].parent;
            if (parent > 64)
                return false;
        }
    }
    return true;
}
static bool mount(void) {
    uint32_t header[128];
    if (!disk_read(0, header) || memcmp(header, "CRONKFS2", 8) || header[2] != 2 ||
        header[3] != 4096 || header[4] < FIRST_DATA || header[4] > (1u << 25))
        return false;
    volume_pages = header[4];
    for (unsigned b = 0; b < 2; ++b)
        valid_bank[b] = disk_read_many((1 + b * 2) * 8, 16, &banks[b]) && validate(&banks[b]);
    if (!valid_bank[0] && !valid_bank[1])
        return false;
    active_bank =
        valid_bank[1] && (!valid_bank[0] || banks[1].generation > banks[0].generation) ? 1 : 0;
    if (current_dir && (!entry(current_dir)->name[0] || entry(current_dir)->kind != FOLDER))
        current_dir = 0;
    return true;
}
static bool commit(const struct extent_entry *e, unsigned slot, unsigned marker) {
    unsigned target = 1 - active_bank;
    // All callers finish allocating/reading payloads before touching the spare bank.
    banks[target] = banks[active_bank];
    if (e)
        banks[target].entries[slot - 1] = *e;
    if (marker < 32)
        banks[target].installed[marker] = 1;
    if (banks[target].generation == UINT64_MAX)
        return false;
    ++banks[target].generation;
    banks[target].checksum = 0;
    banks[target].checksum = bank_hash(&banks[target]);
    unsigned sector = (1 + target * 2) * 8;
    if (!disk_write_many(sector + 1, 15, (uint8_t *)&banks[target] + 512) || !disk_flush() ||
        !disk_write_many(sector, 1, &banks[target]) || !disk_flush()) {
        valid_bank[target] = false;
        return false;
    }
    active_bank = target;
    valid_bank[target] = true;
    return true;
}
static unsigned allocate(unsigned pages) {
    if (!pages)
        return 0;
    unsigned candidate = FIRST_DATA;
    while (candidate <= volume_pages && pages <= volume_pages - candidate) {
        unsigned next = candidate;
        for (unsigned b = 0; b < 2; ++b)
            if (valid_bank[b])
                for (unsigned i = 0; i < 64; ++i) {
                    struct extent_entry *e = &banks[b].entries[i];
                    if (e->name[0] && e->pages && candidate < e->start + e->pages &&
                        e->start < candidate + pages && e->start + e->pages > next)
                        next = e->start + e->pages;
                }
        if (next == candidate)
            return candidate;
        candidate = next;
    }
    return UINT32_MAX;
}
static bool read_payload(const struct extent_entry *e, void *data) {
    uint8_t page[4096];
    size_t remaining = e->size;
    uint8_t *out = data;
    for (unsigned i = 0; i < e->pages; ++i) {
        if (!disk_read_many((e->start + i) * 8, 8, page))
            return false;
        size_t n = remaining < 4096 ? remaining : 4096;
        memcpy(out, page, n);
        out += n;
        remaining -= n;
    }
    ((uint8_t *)data)[e->size] = 0;
    return hash(data, e->size) == e->checksum;
}
static const char *write_entry(unsigned slot, struct extent_entry *e, const void *data,
                               size_t size) {
    if (size > FILE_TEXT_MAX)
        return "file too large (maximum 65535 bytes)";
    e->size = size;
    e->pages = (size + 4095) / 4096;
    e->start = allocate(e->pages);
    if (e->start == UINT32_MAX)
        return "no contiguous space available";
    e->checksum = hash(data, size);
    const uint8_t *source = data;
    size_t remaining = size;
    uint8_t page[4096];
    for (unsigned i = 0; i < e->pages; ++i) {
        memset(page, 0, sizeof(page));
        size_t n = remaining < 4096 ? remaining : 4096;
        memcpy(page, source, n);
        source += n;
        remaining -= n;
        if (!disk_write_many((e->start + i) * 8, 8, page))
            return "disk I/O error";
    }
    if (e->pages && !disk_flush())
        return "disk I/O error";
    return commit(e, slot, 32) ? NULL : "disk I/O error";
}
struct extent_path {
    unsigned slot, parent;
    char leaf[32];
    bool trailing;
};
static const char *resolve(const char *text, struct extent_path *p) {
    unsigned slot = *text == '/' ? 0 : current_dir;
    while (*text) {
        while (*text == '/')
            ++text;
        if (!*text)
            break;
        if (slot && entry(slot)->kind != FOLDER)
            return "not a folder";
        const char *begin = text;
        while (*text && *text != '/')
            ++text;
        size_t n = text - begin;
        if (n > 31)
            return "name exceeds 31 characters";
        p->trailing = *text == '/';
        while (*text == '/')
            ++text;
        if (n == 1 && begin[0] == '.')
            continue;
        if (n == 2 && !memcmp(begin, "..", 2)) {
            slot = slot ? entry(slot)->parent : 0;
            continue;
        }
        p->parent = slot;
        memset(p->leaf, 0, 32);
        memcpy(p->leaf, begin, n);
        for (slot = 1; slot <= 64; ++slot)
            if (entry(slot)->name[0] && entry(slot)->parent == p->parent &&
                !memcmp(entry(slot)->name, p->leaf, 32))
                break;
        if (slot == NOT_FOUND && *text)
            return "folder not found";
    }
    p->slot = slot;
    if (p->trailing && slot && slot != NOT_FOUND && entry(slot)->kind != FOLDER)
        return "not a folder";
    return NULL;
}
static unsigned empty_slot(void) {
    unsigned i;
    for (i = 1; i <= 64 && entry(i)->name[0]; ++i) {
    }
    return i;
}
static const char *text_path(const char *name, struct extent_path *p) {
    if (!*name)
        return "path required";
    if (!mount())
        return "data disk unavailable or invalid";
    const char *error = resolve(name, p);
    if (error)
        return error;
    if (p->trailing || !p->slot || (p->slot != NOT_FOUND && entry(p->slot)->kind != TEXT_FILE))
        return "not a text file";
    return NULL;
}
const char *extentfs_load(const char *name, char *text, size_t *size) {
    struct extent_path p = {0};
    const char *error = text_path(name, &p);
    if (error)
        return error;
    text[0] = 0;
    *size = 0;
    if (p.slot == NOT_FOUND)
        return NULL;
    struct extent_entry e = *entry(p.slot);
    if (!read_payload(&e, text))
        return "disk I/O or checksum error";
    *size = e.size;
    return NULL;
}
const char *extentfs_save(const char *name, const char *text, size_t size) {
    struct extent_path p = {0};
    const char *error = text_path(name, &p);
    if (error)
        return error;
    unsigned slot = p.slot == NOT_FOUND ? empty_slot() : p.slot;
    if (slot > 64)
        return "disk full";
    struct extent_entry e = {0};
    memcpy(e.name, p.leaf, 32);
    e.parent = p.parent;
    e.kind = TEXT_FILE;
    return write_entry(slot, &e, text, size);
}
unsigned extentfs_cwd(void) { return current_dir; }
int extentfs_directory(unsigned dir, struct file_item *items, unsigned *parent) {
    if (!mount() || dir > 64 || (dir && (!entry(dir)->name[0] || entry(dir)->kind != FOLDER)))
        return -1;
    *parent = dir ? entry(dir)->parent : 0;
    int count = 0;
    for (unsigned i = 1; i <= 64; ++i)
        if (entry(i)->name[0] && entry(i)->parent == dir) {
            memcpy(items[count].name, entry(i)->name, 32);
            items[count].kind = entry(i)->kind;
            items[count++].slot = i;
        }
    return count;
}
void extentfs_print_path(struct limine_framebuffer *fb, unsigned dir) {
    unsigned chain[64], n = 0;
    while (dir && n < 64) {
        chain[n++] = dir;
        dir = entry(dir)->parent;
    }
    terminal_putchar(fb, '/');
    while (n) {
        terminal_write(fb, entry(chain[--n])->name);
        if (n)
            terminal_putchar(fb, '/');
    }
}
void extentfs_preview(struct limine_framebuffer *fb, unsigned slot) {
    if (!mount() || !slot || slot > 64 || !entry(slot)->name[0] ||
        !read_payload(entry(slot), file_buffer))
        terminal_write(fb, "disk I/O error\n");
    else {
        terminal_write(fb, (char *)file_buffer);
        terminal_putchar(fb, '\n');
    }
}
void extentfs_install(const char *name, unsigned marker, const void *data, size_t size) {
    if (marker >= 32 || !mount() || banks[active_bank].installed[marker] || size > FILE_TEXT_MAX)
        return;
    struct extent_path p = {0};
    if (resolve(name, &p))
        return;
    if (p.slot == NOT_FOUND) {
        unsigned slot = empty_slot();
        if (slot > 64)
            return;
        struct extent_entry e = {0};
        memcpy(e.name, p.leaf, 32);
        e.kind = EXEC_FILE;
        if (write_entry(slot, &e, data, size))
            return;
    }
    commit(NULL, 0, marker);
}
static const char *operate(struct limine_framebuffer *fb, enum file_action action, char *args) {
    if (!mount())
        return "data disk unavailable or invalid";
    if (action == FILE_PWD) {
        extentfs_print_path(fb, current_dir);
        terminal_putchar(fb, '\n');
        return NULL;
    }
    while (*args == ' ')
        ++args;
    char *name = args, *rest = args;
    if (*args == '"') {
        name = ++rest;
        while (*rest && *rest != '"')
            ++rest;
        if (!*rest)
            return "missing closing quote";
        *rest++ = 0;
        if (*rest && *rest != ' ')
            return "expected space after quoted path";
        if (*rest)
            ++rest;
    } else {
        while (*rest && *rest != ' ')
            ++rest;
        if (*rest)
            *rest++ = 0;
    }
    if (action != FILE_WRITE && action != FILE_MOVE && action != FILE_COPY && *rest)
        return "quote paths containing spaces";
    if (!*name && action != FILE_LIST && action != FILE_CD)
        return "path required";
    if (!*name && action == FILE_CD)
        name = "/";
    struct extent_path p = {0};
    const char *error = resolve(name, &p);
    if (error)
        return error;
    unsigned slot = p.slot;
    bool folder = !slot || (slot != NOT_FOUND && entry(slot)->kind == FOLDER);
    if (action == FILE_WRITE)
        return extentfs_save(name, rest, ({
                                 size_t n = 0;
                                 while (rest[n])
                                     ++n;
                                 n;
                             }))
                   ?: "saved";
    if (action == FILE_MKDIR) {
        if (slot != NOT_FOUND)
            return "already exists";
        slot = empty_slot();
        if (slot > 64)
            return "disk full";
        struct extent_entry e = {0};
        memcpy(e.name, p.leaf, 32);
        e.kind = FOLDER;
        e.parent = p.parent;
        e.checksum = hash(NULL, 0);
        return commit(&e, slot, 32) ? "folder created" : "disk I/O error";
    }
    if (slot == NOT_FOUND)
        return "path not found";
    if (action == FILE_CD || action == FILE_LIST) {
        if (!folder)
            return "not a folder";
        if (action == FILE_CD) {
            current_dir = slot;
            return NULL;
        }
        bool any = false;
        for (unsigned i = 1; i <= 64; ++i)
            if (entry(i)->name[0] && entry(i)->parent == slot) {
                terminal_write(fb, entry(i)->name);
                if (entry(i)->kind == FOLDER)
                    terminal_putchar(fb, '/');
                terminal_putchar(fb, '\n');
                any = true;
            }
        return any ? NULL : "(empty)";
    }
    if (action == FILE_REMOVE || action == FILE_RMDIR) {
        if (!slot || slot == current_dir)
            return "cannot remove root or current folder";
        if (folder != (action == FILE_RMDIR))
            return folder ? "use rmdir for folders" : "not a folder";
        for (unsigned i = 1; folder && i <= 64; ++i)
            if (entry(i)->name[0] && entry(i)->parent == slot)
                return "folder is not empty";
        struct extent_entry empty = {0};
        return commit(&empty, slot, 32) ? "removed" : "disk I/O error";
    }
    if (action == FILE_MOVE || action == FILE_COPY) {
        if (!slot)
            return "source not found";
        struct extent_entry e = *entry(slot);
        struct extent_path dest = {0};
        if (*rest == '"') {
            ++rest;
            size_t n = 0;
            while (rest[n] && rest[n] != '"')
                ++n;
            if (!rest[n] || rest[n + 1])
                return "invalid destination";
            rest[n] = 0;
        }
        if (!*rest)
            return "destination required";
        error = resolve(rest, &dest);
        if (error)
            return error;
        if (dest.slot != NOT_FOUND) {
            if (dest.slot && entry(dest.slot)->kind != FOLDER)
                return "destination exists";
            dest.parent = dest.slot;
            memcpy(dest.leaf, e.name, 32);
            for (unsigned i = 1; i <= 64; ++i)
                if (entry(i)->name[0] && entry(i)->parent == dest.parent &&
                    !memcmp(entry(i)->name, dest.leaf, 32))
                    return "destination exists";
        } else if (dest.trailing)
            return "destination folder not found";
        if (folder) {
            if (action == FILE_COPY)
                return "folder copy is not supported";
            for (unsigned a = dest.parent; a; a = entry(a)->parent)
                if (a == slot)
                    return "cannot move folder inside itself";
        }
        memcpy(e.name, dest.leaf, 32);
        e.parent = dest.parent;
        if (action == FILE_COPY) {
            unsigned target = empty_slot();
            if (target > 64)
                return "disk full";
            if (!read_payload(entry(slot), file_buffer))
                return "disk I/O error";
            error = write_entry(target, &e, file_buffer, e.size);
            return error ?: "copied";
        }
        return commit(&e, slot, 32) ? "moved" : "disk I/O error";
    }
    if (folder)
        return "is a folder";
    struct extent_entry e = *entry(slot);
    if (!read_payload(&e, file_buffer))
        return "disk I/O or checksum error";
    if (action == FILE_READ) {
        if (e.kind == EXEC_FILE)
            return "binary file";
        terminal_write(fb, (char *)file_buffer);
        terminal_putchar(fb, '\n');
    } else {
        if (e.kind != EXEC_FILE)
            return "not an executable";
        program_run(fb, e.name, file_buffer, e.size, action == FILE_RUNBG);
    }
    return NULL;
}
void extentfs_command(struct limine_framebuffer *fb, enum file_action action, char *args) {
    const char *message = operate(fb, action, args);
    if (message) {
        terminal_write(fb, message);
        terminal_putchar(fb, '\n');
    }
}

bool extentfs_scratch(unsigned *sector, unsigned *sectors) {
    if (!mount())
        return false;
    unsigned page = allocate(8);
    if (page == UINT32_MAX)
        return false;
    *sector = page * 8;
    *sectors = 64;
    return true;
}
