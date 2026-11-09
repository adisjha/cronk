#include "memory_view.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include "sched/scheduler.h"
#include "gfx/framebuffer.h"
#include "drivers/keyboard/keyboard.h"
#include "terminal.h"

static const char digits[] = "0123456789abcdef";

static void number(struct limine_framebuffer *fb, uint64_t value, unsigned base, unsigned width) {
    char text[32];
    char *p = text + sizeof(text) - 1;
    *p = 0;
    do {
        *--p = digits[value % base];
        value /= base;
    } while (value || (size_t)(text + sizeof(text) - 1 - p) < width);
    terminal_write(fb, p);
}

static void metric(struct limine_framebuffer *fb, const char *name, size_t pages) {
    terminal_write(fb, name);
    number(fb, pages * (PAGE_SIZE / 1024), 10, 1);
    terminal_write(fb, " KiB  (");
    number(fb, pages, 10, 1);
    terminal_write(fb, " pages)\n");
}

void memory_view(struct limine_framebuffer *fb) {
    size_t index = 0, offset = 0;
    // Adapt dump dimensions to the actual framebuffer.
    size_t cols = ui_width(fb) / FONT_WIDTH;
    size_t bytes_per_row = cols >= 76 ? 16 : 8;
    size_t rows = ui_height(fb) > 24 ? (ui_height(fb) - 24) / FONT_HEIGHT : 0;
    size_t lines = rows > 13 ? rows - 13 : 1;
    if (lines > 16) lines = 16;
    size_t step = lines * bytes_per_row;
    terminal_clear(fb);
    for (;;) {
        struct page_stats s = pages_stats();
        enum page_owner owner = PAGE_FREE;
        const unsigned char *page = pages_inspect(index, &owner);
        unsigned char snapshot[256]; // At most 16 rows of 16 bytes.
        size_t length = step < PAGE_SIZE - offset ? step : PAGE_SIZE - offset;
        if (page) memcpy(snapshot, page + offset, length);
        terminal_begin_refresh();
        terminal_write(fb, "MEMORY   /   Physical RAM\n\n");
        metric(fb, "Used       ", s.boot + s.managed - s.available);
        metric(fb, "Available  ", s.available);
        metric(fb, "Cached     ", 0);
        terminal_write(fb, "Used includes kernel, modules, bootloader and allocator. No page cache yet.\n");
        metric(fb, "Peak program allocation   ", s.peak_program);
        terminal_write(fb, "\nManaged page ");
        if (page) {
            static const char *const names[] = {"free", "allocator metadata", "program code", "stack"};
            number(fb, index + 1, 10, 1);
            terminal_write(fb, " / ");
            number(fb, s.managed, 10, 1);
            terminal_write(fb, "   ");
            terminal_write(fb, names[owner]);
            terminal_write(fb, "   physical 0x");
            number(fb, pages_physical(page), 16, 8);
            terminal_write(fb, "\n");
        } else {
            terminal_write(fb, "unavailable\n");
        }
        terminal_write(fb, "N/P page   J/K bytes   Q/Esc back\n\n");
        for (size_t i = offset; page && i < PAGE_SIZE && i < offset + step; i += bytes_per_row) {
            number(fb, i, 16, 3);
            terminal_write(fb, "  ");
            char ascii[17];
            for (size_t j = 0; j < bytes_per_row; ++j) {
                unsigned char byte = snapshot[i - offset + j];
                number(fb, byte, 16, 2);
                terminal_putchar(fb, ' ');
                ascii[j] = byte >= 32 && byte < 127 ? byte : '.';
            }
            ascii[bytes_per_row] = 0;
            terminal_write(fb, " ");
            terminal_write(fb, ascii);
            terminal_putchar(fb, '\n');
        }
        terminal_end_refresh(fb);
        uint64_t last_check = scheduler_seconds();
        char key;
        do {

            terminal_tick(fb, NULL);
            key = keyboard_poll_char();
            uint64_t now = scheduler_seconds();
            if (now != last_check) {
                last_check = now;
                struct page_stats latest = pages_stats();
                enum page_owner latest_owner = PAGE_FREE;
                pages_inspect(index, &latest_owner);
                if (latest.available != s.available || latest.peak_program != s.peak_program ||
                    latest_owner != owner || (page && memcmp(snapshot, page + offset, length))) break;
            }
            if (!key) scheduler_wait_input();
        } while (!key);
        if (key == KEY_ESC || key == 'q') break;
        if (key == 'n' && index + 1 < s.managed) { ++index; offset = 0; }
        if (key == 'p' && index) { --index; offset = 0; }
        if (key == 'j' && offset + step < PAGE_SIZE) offset += step;
        if (key == 'k' && offset >= step) offset -= step;
    }
    terminal_clear(fb);
}
