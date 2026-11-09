#include "files.h"
#include "program/program.h"
#include "ata.h"
#include "lib/memory.h"
#include "term/terminal.h"


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
    for (unsigned slot = 1; slot <= FILE_SLOTS; ++slot) {
        if (!ata_read(slot, &record)) return;
        if ((uint8_t)record.text[479] / 3 == 0 && !memcmp(record.name, name, name_size)) {
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

// The final byte encodes parent slot * 3 + kind. Old text and executable
// records (tags 0 and 1) therefore already belong to root, slot 0.
static struct node {
    char name[32];
    unsigned parent;
    enum file_kind kind;
} nodes[FILE_SLOTS + 1];
static unsigned cwd;

unsigned files_cwd(void) { return cwd; }

static bool load_directory(void) {
    struct record r;
    if (!ata_read(0, &r) || memcmp(&r, "CRONKFS1", 8)) return false;
    for (unsigned i = 1; i <= FILE_SLOTS; ++i) {
        if (!ata_read(i, &r)) return false;
        struct node *n = &nodes[i];
        memcpy(n->name, r.name, sizeof(n->name));
        if (!n->name[0]) continue;
        unsigned tag = (uint8_t)r.text[479];
        n->parent = tag / 3;
        n->kind = tag % 3;
        if (n->name[31] || n->parent > FILE_SLOTS) return false;
    }
    for (unsigned i = 1; i <= FILE_SLOTS; ++i) {
        if (!nodes[i].name[0]) continue;
        unsigned parent = nodes[i].parent;
        for (unsigned depth = 0; parent; ++depth) {
            if (depth >= FILE_SLOTS || !nodes[parent].name[0] || nodes[parent].kind != FOLDER) return false;
            parent = nodes[parent].parent;
        }
    }
    return true;
}

int files_directory(unsigned dir, struct file_item items[FILE_SLOTS], unsigned *parent) {
    if (!load_directory()) return -1;
    *parent = nodes[dir].parent;
    int count = 0;
    for (unsigned i = 1; i <= FILE_SLOTS; ++i) {
        if (!nodes[i].name[0] || nodes[i].parent != dir) continue;
        struct file_item *item = &items[count++];
        memcpy(item->name, nodes[i].name, sizeof(item->name));
        item->kind = nodes[i].kind;
        item->slot = i;
    }
    return count;
}

void files_print_path(struct limine_framebuffer *fb, unsigned dir) {
    unsigned chain[FILE_SLOTS], length = 0;
    while (dir) { chain[length++] = dir; dir = nodes[dir].parent; }
    terminal_putchar(fb, '/');
    while (length) {
        terminal_write(fb, nodes[chain[--length]].name);
        if (length) terminal_putchar(fb, '/');
    }
}

#define MISSING (FILE_SLOTS + 1)
struct path { unsigned slot, parent; char leaf[32]; bool directory; };

static const char *resolve(const char *s, struct path *path) {
    unsigned slot = *s == '/' ? 0 : cwd;
    while (*s) {
        while (*s == '/') ++s;
        if (!*s) break;
        if (slot && nodes[slot].kind != FOLDER) return "not a folder";
        const char *start = s;
        while (*s && *s != '/') ++s;
        size_t n = s - start;
        if (n > 31) return "name exceeds 31 characters";
        path->directory = *s == '/';
        while (*s == '/') ++s;
        if (n == 1 && *start == '.') continue;
        if (n == 2 && !memcmp(start, "..", 2)) { slot = nodes[slot].parent; continue; }
        path->parent = slot;
        memcpy(path->leaf, start, n);
        path->leaf[n] = 0;
        for (slot = 1; slot <= FILE_SLOTS; ++slot) {
            if (nodes[slot].parent == path->parent && !memcmp(nodes[slot].name, path->leaf, n+1)) break;
        }
        if (slot == MISSING && *s) return "folder not found";
    }
    path->slot = slot;
    if (path->directory && slot && slot != MISSING && nodes[slot].kind != FOLDER) return "not a folder";
    return NULL;
}

static void print_text(struct limine_framebuffer *fb, struct record *r) {
    // Folder tags occupy the last byte even for text records.
    r->text[479] = 0;
    terminal_write(fb, r->text);
    terminal_putchar(fb, '\n');
}

static const char *text_path(const char *name, struct path *path) {
    if (!*name) return "path required";
    if (!load_directory()) return "data disk unavailable or invalid";
    const char *error = resolve(name, path);
    if (error) return error;
    if (path->directory || !path->slot ||
        (path->slot != MISSING && nodes[path->slot].kind != TEXT_FILE))
        return "not a text file";
    return NULL;
}

const char *files_text_load(const char *name, char text[FILE_TEXT_MAX + 1], size_t *size) {
    struct path path = {0};
    const char *error = text_path(name, &path);
    if (error) return error;
    *size = 0;
    text[0] = 0;
    if (path.slot == MISSING) return NULL;
    struct record r;
    if (!ata_read(path.slot, &r)) return "disk I/O error";
    while (*size < FILE_TEXT_MAX && r.text[*size]) ++*size;
    memcpy(text, r.text, *size);
    text[*size] = 0;
    return NULL;
}

const char *files_text_save(const char *name, const char *text, size_t size) {
    if (size > FILE_TEXT_MAX) return "file too large";
    struct path path = {0};
    const char *error = text_path(name, &path);
    if (error) return error;
    unsigned slot = path.slot;
    if (slot == MISSING) {
        for (slot = 1; slot <= FILE_SLOTS && nodes[slot].name[0]; ++slot) {}
        if (slot > FILE_SLOTS) return "disk full";
    }
    struct record r = {0};
    memcpy(r.name, path.leaf, sizeof(r.name));
    memcpy(r.text, text, size);
    r.text[479] = (char)(path.parent * 3 + TEXT_FILE);
    return ata_write(slot, &r) ? NULL : "disk I/O error";
}

void files_preview(struct limine_framebuffer *fb, unsigned slot) {
    struct record r;
    if (!ata_read(slot, &r)) terminal_write(fb, "disk I/O error\n");
    else print_text(fb, &r);
}

static const char *operate(struct limine_framebuffer *fb, enum file_action action, char *args) {
    if (!load_directory()) return "data disk unavailable or invalid";
    if (action == FILE_PWD) {
        files_print_path(fb, cwd);
        terminal_putchar(fb, '\n');
        return NULL;
    }
    while (*args == ' ') ++args;
    char *name = args, *text = args;
    if (*args == '"') {
        name = ++text;
        while (*text && *text != '"') ++text;
        if (!*text) return "missing closing quote";
        *text++ = 0;
        if (*text && *text != ' ') return "expected space after quoted path";
        if (*text) ++text;
    } else {
        while (*text && *text != ' ') ++text;
        if (*text) *text++ = 0;
    }
    if (action != FILE_WRITE && action != FILE_MOVE && action != FILE_COPY && *text) return "quote paths containing spaces";
    if (!*name && action != FILE_LIST && action != FILE_CD) return "path required";
    if (!*name && action == FILE_CD) name = "/";
    struct path path = {0};
    const char *error = resolve(name, &path);
    if (error) return error;
    unsigned slot = path.slot;
    bool folder = !slot || (slot != MISSING && nodes[slot].kind == FOLDER);
    if (action == FILE_MOVE || action == FILE_COPY) {
        if (!slot || slot == MISSING) return "source not found";
        struct path dest = {0};
        if (*text == '"') {
            ++text;
            size_t n = 0;
            while (text[n] && text[n] != '"') ++n;
            if (!text[n] || text[n+1]) return "invalid destination";
            text[n] = 0;
        }
        if (!*text) return "destination required";
        error = resolve(text, &dest);
        if (error) return error;
        if (dest.slot != MISSING) {
            if (dest.slot && nodes[dest.slot].kind != FOLDER) return "destination exists";
            dest.parent = dest.slot;
            memcpy(dest.leaf, nodes[slot].name, 32);
            for (unsigned i = 1; i <= FILE_SLOTS; ++i)
                if (nodes[i].name[0] && nodes[i].parent == dest.parent &&
                    !memcmp(nodes[i].name, dest.leaf, 32)) return "destination exists";
        } else if (dest.directory) return "destination folder not found";
        if (folder) {
            if (action == FILE_COPY) return "folder copy is not supported";
            for (unsigned ancestor = dest.parent; ancestor; ancestor = nodes[ancestor].parent)
                if (ancestor == slot) return "cannot move folder inside itself";
        }
        struct record r;
        if (!ata_read(slot, &r)) return "disk I/O error";
        memcpy(r.name, dest.leaf, 32);
        r.text[479] = dest.parent * 3 + nodes[slot].kind;
        unsigned target = slot;
        if (action == FILE_COPY) {
            for (target = 1; target <= FILE_SLOTS && nodes[target].name[0]; ++target) {}
            if (target > FILE_SLOTS) return "disk full";
        }
        return ata_write(target, &r) ? (action == FILE_COPY ? "copied" : "moved") : "disk I/O error";
    }
    if (action == FILE_WRITE || action == FILE_MKDIR) {
        if (action == FILE_MKDIR && slot != MISSING) return "already exists";
        if (action == FILE_WRITE && (folder || path.directory)) return "is a folder or folder path";
        if (slot == MISSING) {
            for (slot = 1; slot <= FILE_SLOTS && nodes[slot].name[0]; ++slot) {}
            if (slot > FILE_SLOTS) return "disk full";
        }
        struct record r = {0};
        memcpy(r.name, path.leaf, sizeof(r.name));
        if (action == FILE_WRITE) {
            size_t n = 0;
            while (text[n]) ++n;
            if (n > sizeof(r.text)-1) return "file too large";
            memcpy(r.text, text, n);
        }
        r.text[479] = (char)(path.parent * 3 + (action == FILE_MKDIR ? FOLDER : TEXT_FILE));
        if (!ata_write(slot, &r)) return "disk I/O error";
        return action == FILE_MKDIR ? "folder created" : "saved";
    }
    if (slot == MISSING) return "path not found";
    if (action == FILE_CD || action == FILE_LIST) {
        if (!folder) return "not a folder";
        if (action == FILE_CD) { cwd = slot; return NULL; }
        bool any = false;
        for (unsigned i = 1; i <= FILE_SLOTS; ++i) {
            if (!nodes[i].name[0] || nodes[i].parent != slot) continue;
            terminal_write(fb, nodes[i].name);
            if (nodes[i].kind == FOLDER) terminal_putchar(fb, '/');
            terminal_putchar(fb, '\n');
            any = true;
        }
        return any ? NULL : "(empty)";
    }
    if (action == FILE_REMOVE || action == FILE_RMDIR) {
        if (!slot || slot == cwd) return "cannot remove root or current folder";
        if (folder != (action == FILE_RMDIR)) return folder ? "use rmdir for folders" : "not a folder";
        if (folder) {
            for (unsigned i = 1; i <= FILE_SLOTS; ++i)
                if (nodes[i].name[0] && nodes[i].parent == slot) return "folder is not empty";
        }
        struct record empty = {0};
        return ata_write(slot, &empty) ? "removed" : "disk I/O error";
    }
    if (folder) return "is a folder";
    struct record r;
    if (!ata_read(slot, &r)) return "disk I/O error";
    if (action == FILE_READ) {
        if (nodes[slot].kind == EXEC_FILE) return "binary file";
        print_text(fb, &r);
    } else {
        size_t size = (uint8_t)r.text[477] | ((size_t)(uint8_t)r.text[478] << 8);
        if (nodes[slot].kind != EXEC_FILE || size > BINARY_CAPACITY) return "not an executable";
        program_run(fb, r.name, r.text, size, action == FILE_RUNBG);
    }
    return NULL;
}

void files_command(struct limine_framebuffer *fb, enum file_action action, char *args) {
    const char *message = operate(fb, action, args);
    if (message) {
        terminal_write(fb, message);
        terminal_putchar(fb, '\n');
    }
}
