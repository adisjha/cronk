#include "program.h"
#include "storage/files.h"
#include "mm/pages.h"
#include "arch/user.h"
#include "arch/cpu.h"
#include "lib/memory.h"
#include "term/terminal.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request modules = {
    .id = LIMINE_MODULE_REQUEST_ID
};

#include "sched/scheduler.h"

struct instance {
    struct user_space space;
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
#if defined(__aarch64__)
        if (len == 11 && !memcmp(name, "crash.cronk", len))
            files_install_program("crash-arm64.bin", 13, file->address, file->size);
        if (len == 11 && !memcmp(name, "hello.cronk", len))
            files_install_program("hello-arm64.bin", 11, file->address, file->size);
        if (len == 13 && !memcmp(name, "counter.cronk", len))
            files_install_program("counter-arm64.bin", 12, file->address, file->size);
#else
        if (len == 11 && !memcmp(name, "crash.cronk", len))
            files_install_program("crash.bin", 10, file->address, file->size);
        if (len == 11 && !memcmp(name, "hello.cronk", len))
            files_install_program("hello.bin", 8, file->address, file->size);
        if (len == 13 && !memcmp(name, "counter.cronk", len))
            files_install_program("counter.bin", 9, file->address, file->size);
#endif
    }
}

static void release(void *context) {
    struct instance *p = context;
    if (p->space.stack) pages_free(p->space.stack, 4);
    if (p->space.code) pages_free(p->space.code, 1);
    *p = (struct instance){0};
}

static void background_main(void *context) {
    struct instance *p = context;
    user_execute(&p->space);
}

static void foreground_write(void *context, const char *text) {
    terminal_write(context, text);
}

void program_run(struct limine_framebuffer *fb, const char *name, const void *image,
                 size_t size, bool background) {
    if (size <= 8 || size > USER_WRITE - USER_CODE || memcmp(image,
#if defined(__aarch64__)
        "CRONKA64",
#else
        "CRONK001",
#endif
        8)) {
        terminal_write(fb, "invalid executable\n");
        return;
    }
    unsigned slot = background ? 1 : 0;
    while (slot < TASK_SLOTS && instances[slot].space.code) ++slot;
    if (slot == TASK_SLOTS) {
        terminal_write(fb, "task table full\n");
        return;
    }
    struct instance *p = &instances[slot];
    p->space.code = pages_alloc(1, PAGE_CODE);
    p->space.stack = pages_alloc(4, PAGE_STACK);
    if (!p->space.code || !p->space.stack) {
        release(p);
        terminal_write(fb, "unable to allocate program memory\n");
        return;
    }
    memcpy(p->space.code, image, size);
    // The original flat ABI calls write(context, string) and returns to exit.
    // Both destinations now point to Ring 3 syscall stubs.
#if defined(__aarch64__)
    static const uint32_t write_stub[] = {0xd2800028, 0xd4000001, 0xd65f03c0};
    static const uint32_t exit_stub[] = {0xd2800008, 0xd4000001};
#else
    static const unsigned char write_stub[] = {0xb8,1,0,0,0,0xcd,0x80,0xc3};
    static const unsigned char exit_stub[] = {0x31,0xc0,0xcd,0x80};
    *(uintptr_t *)(p->space.stack + 4 * PAGE_SIZE - 8) = USER_EXIT;
#endif
    memcpy(p->space.code + (USER_WRITE - USER_CODE), write_stub, sizeof(write_stub));
    memcpy(p->space.code + (USER_EXIT - USER_CODE), exit_stub, sizeof(exit_stub));
    p->space.write = background ? scheduler_output : foreground_write;
    p->space.context = background ? NULL : fb;
    user_space_init(&p->space, slot);
    if (background) {
        size_t n = 0;
        while (n + 1 < sizeof(p->name) && name[n]) ++n;
        memcpy(p->name, name, n);
        p->name[n] = 0;
        unsigned id = scheduler_spawn(p->name, background_main, p, release);
        if (id) {
            scheduler_task_memory(id, 8192 + 5 * PAGE_SIZE);
            terminal_write(fb, "started ");
            terminal_write(fb, name);
            terminal_write(fb, " as task ");
            terminal_write_uint(fb, id);
            terminal_putchar(fb, '\n');
            return;
        }
        terminal_write(fb, "task table full\n");
    } else {
        unsigned fault = user_execute(&p->space);
        if (fault) {
            terminal_write(fb, name);
#if defined(__aarch64__)
            terminal_write(fb, " stopped: EL0 exception ");
#else
            terminal_write(fb, " stopped: ring 3 exception ");
#endif
            terminal_write_uint(fb, fault - 1);
            terminal_putchar(fb, '\n');
        }
    }
    release(p);
}
