#include "program_view.h"
#include "terminal.h"
#include "sched/scheduler.h"
#include "mm/pages.h"
#include "drivers/keyboard/keyboard.h"

static void two_digits(struct limine_framebuffer *fb, unsigned value) {
    terminal_putchar(fb, '0' + value / 10);
    terminal_putchar(fb, '0' + value % 10);
}

static void percentage(struct limine_framebuffer *fb, uint64_t runtime, uint64_t period) {
    if (!period) {
        terminal_write(fb, "--");
        return;
    }
    uint64_t tenths = (runtime * 1000 + period / 2) / period;
    if (runtime && !tenths) {
        terminal_write(fb, "<0.1%");
        return;
    }
    terminal_write_uint(fb, tenths / 10);
    terminal_putchar(fb, '.');
    terminal_putchar(fb, '0' + tenths % 10);
    terminal_putchar(fb, '%');
}

void program_view(struct limine_framebuffer *fb) {
    struct scheduler_sample before, now;
    scheduler_sample(&now);
    before = now;
    unsigned selected = 1;
    bool redraw = true;
    terminal_clear(fb);
    for (;;) {
        terminal_tick(fb, NULL);
        uint64_t uptime = scheduler_seconds();
        if (uptime > now.seconds) {
            before = now;
            scheduler_sample(&now);
            redraw = true;
        }
        if (redraw) {
            terminal_begin_refresh();
            terminal_write(fb, "PROGRAM MANAGER     Uptime ");
            terminal_write_uint(fb, now.seconds / 3600);
            terminal_putchar(fb, ':');
            two_digits(fb, now.seconds / 60 % 60);
            terminal_putchar(fb, ':');
            two_digits(fb, now.seconds % 60);
            uint64_t period = now.timestamp - before.timestamp;
            terminal_write(fb, "     CPU idle ");
            percentage(fb, now.idle_runtime - before.idle_runtime, period);
            terminal_write(fb, "     Available RAM ");
            terminal_write_uint(fb, pages_stats().available * (PAGE_SIZE / 1024));
            terminal_write(fb, " KiB\n\n    ID    Ring    CPU share    Task / last output\n");
            bool present = false;
            for (unsigned i = 0; i < TASK_SLOTS; ++i)
                if (now.tasks[i].id == selected) present = true;
            if (!present) selected = 1;
            for (unsigned i = 0; i < TASK_SLOTS; ++i) {
                struct task_info *t = &now.tasks[i];
                if (!t->id) continue;
                terminal_write(fb, t->id == selected ? " >  " : "    ");
                terminal_write_uint(fb, t->id);
                terminal_write(fb, "     ");
                terminal_write_uint(fb, t->ring);
                terminal_write(fb, "       ");
                uint64_t prior = t->id == before.tasks[i].id ? before.tasks[i].runtime : 0;
                percentage(fb, t->runtime - prior, period);
                terminal_write(fb, "         ");
                terminal_write(fb, t->name);
                if (*t->output) {
                    terminal_write(fb, "   ");
                    terminal_write(fb, t->output);
                }
                terminal_putchar(fb, '\n');
            }
            terminal_write(fb, "\nCPU share = share of elapsed time on the guest CPU in the last sample.\n"
                               "N next task   K stop selected background task   Q/F1 shell\n");
            terminal_end_refresh(fb);
            redraw = false;
        }
        char key = keyboard_poll_char();
        if (key == 'q' || key == KEY_F1) break;
        if (key == 'n') {
            unsigned i = 0;
            while (i < TASK_SLOTS && now.tasks[i].id != selected) ++i;
            do { i = (i + 1) % TASK_SLOTS; } while (!now.tasks[i].id);
            selected = now.tasks[i].id;
            redraw = true;
        }
        if (key == 'k' && scheduler_stop(selected)) {
            selected = 1;
            scheduler_reap();
            scheduler_sample(&now);
            redraw = true;
        }
        if (!redraw) scheduler_wait_input();
    }
    terminal_clear(fb);
}
