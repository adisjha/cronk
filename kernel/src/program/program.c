#include "program.h"
#include "storage/files.h"
#include "mm/pages.h"
#include "arch/x86_64/program_window.h"
#include "lib/memory.h"
#include "term/terminal.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request modules = {
    .id = LIMINE_MODULE_REQUEST_ID
};

#include "sched/scheduler.h"

typedef void (*write_fn)(void *, const char *);
extern void program_enter(void *entry, void *stack_top, write_fn write, void *context);

struct instance {
    void *code;
    unsigned char *stack, *window;
    uint64_t *leaf, original;
    char name[32];
};
static struct instance instances[TASK_SLOTS];

void program_install(void) {
    struct limine_module_response *r = modules.response;
    for (size_t i = 0; r && i < r->module_count; ++i) {
        struct limine_file *file = r->modules[i];
        const char *name = file->path;
        for (const char *p = name; *p; ++p) if (*p == '/') name = p + 1;
        size_t len = 0;
        while (name[len]) ++len;
        if (len == 11 && !memcmp(name, "hello.cronk", len))
            files_install_program("hello.bin", 8, file->address, file->size);
        if (len == 13 && !memcmp(name, "counter.cronk", len))
            files_install_program("counter.bin", 9, file->address, file->size);
    }
}

static void release(void *context) {
    struct instance *p = context;
    if (p->leaf) program_window_set(p->window, p->leaf, p->original);
    if (p->stack) pages_free(p->stack, 4);
    if (p->code) pages_free(p->code, 1);
    *p = (struct instance){0};
}

static void background_main(void *context) {
    struct instance *p = context;
    program_enter(p->window + 8, p->stack + 4 * PAGE_SIZE, scheduler_output, NULL);
}

static void foreground_write(void *context, const char *text) {
    terminal_write(context, text);
}

void program_run(struct limine_framebuffer *fb, const char *name, const void *image,
                 size_t size, bool background) {
    if (size <= 8 || size > PAGE_SIZE || memcmp(image, "CRONK001", 8)) {
        terminal_write(fb, "invalid executable\n");
        return;
    }
    unsigned slot = background ? 1 : 0;
    while (slot < TASK_SLOTS && instances[slot].code) ++slot;
    if (slot == TASK_SLOTS) {
        terminal_write(fb, "task table full\n");
        return;
    }
    struct instance *p = &instances[slot];
    p->code = pages_alloc(1, PAGE_CODE);
    p->stack = pages_alloc(4, PAGE_STACK);
    p->window = program_page + slot * PAGE_SIZE;
    if (p->code && p->stack) p->leaf = program_window_entry(p->window);
    if (!p->leaf) {
        release(p);
        terminal_write(fb, "unable to allocate or map program memory\n");
        return;
    }
    memcpy(p->code, image, size);
    p->original = *p->leaf;
    program_window_set(p->window, p->leaf, pages_physical(p->code) | 1);
    if (background) {
        size_t n = 0;
        while (n + 1 < sizeof(p->name) && name[n]) ++n;
        memcpy(p->name, name, n);
        p->name[n] = 0;
        unsigned id = scheduler_spawn(p->name, background_main, p, release);
        if (id) {
            terminal_write(fb, "started ");
            terminal_write(fb, name);
            terminal_write(fb, " as task ");
            terminal_write_uint(fb, id);
            terminal_putchar(fb, '\n');
            return;
        }
        terminal_write(fb, "task table full\n");
    } else {
        program_enter(p->window + 8, p->stack + 4 * PAGE_SIZE, foreground_write, fb);
    }
    release(p);
}
