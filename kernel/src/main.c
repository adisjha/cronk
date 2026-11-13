#include "gfx/framebuffer.h"
#include "program/program.h"
#include <stdbool.h>
#include <stddef.h>
#include <limine.h>
#include "term/terminal.h"
#include "term/shell.h"
#include "sched/scheduler.h"
#include "mm/pages.h"
#include "arch/cpu.h"
#include "storage/block.h"
#include "sched/service.h"
#include "term/apps.h"

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

// Make the boot task's reserved stack size explicit for task RAM reporting.
__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_request = {
    .id = LIMINE_STACK_SIZE_REQUEST_ID, .stack_size = 65536
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
        arch_idle();
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void kmain(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    // Ensure we got a framebuffer.
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    // Fetch the first framebuffer.
    static struct limine_framebuffer permanent_framebuffer;
    permanent_framebuffer = *framebuffer_request.response->framebuffers[0];
    permanent_framebuffer.modes = NULL; permanent_framebuffer.mode_count = 0;
    struct limine_framebuffer *framebuffer = &permanent_framebuffer;

    pages_init();
    arch_early_init();
    framebuffer = arch_display_init(framebuffer);
    if (framebuffer->width >= 3840 && framebuffer->height >= 2160) ui_set_scale(framebuffer, 2);
    clock_start();
    size_t storage_before = pages_breakdown().devices;
    disk_init();
    size_t storage_bytes = (pages_breakdown().devices - storage_before) * PAGE_SIZE;
    terminal_clear(framebuffer);
    terminal_write(framebuffer, "cronk booted.\n");
    terminal_write(framebuffer, "framebuffer initialized\n");
    terminal_write(framebuffer, "welcome to terminal\n");
    program_install();
    arch_start_secondary_cpus();
    size_t input_before = pages_breakdown().devices;
    scheduler_start();
    size_t input_bytes = (pages_breakdown().devices - input_before) * PAGE_SIZE;
    system_services_start(input_bytes, storage_bytes);
    desktop_start(framebuffer);
    arch_release_secondary_cpus();
    // APs already left bootloader stacks/tables before their readiness reports.
    if (arch_boot_reclaim_safe()) {
        size_t freed = pages_reclaim_boot();
        terminal_write(framebuffer, "Boot RAM reclaimed: ");
        terminal_write_uint(framebuffer, freed * 4); terminal_write(framebuffer, " KiB\n");
    }
    scheduler_task_memory(1002,SERVICE_STACK_SIZE+pages_breakdown().metadata*PAGE_SIZE);
    shell_run(framebuffer);
}
