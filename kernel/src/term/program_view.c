#include "program_view.h"
#include "terminal.h"
#include "format.h"
#include "gfx/framebuffer.h"
#include "sched/scheduler.h"
#include "mm/pages.h"
#include "drivers/keyboard/keyboard.h"

#define BG 0x101820
#define PANEL 0x1b2935
#define MUTED 0x9eafbf
#define WHITE 0xf1f5fa
#define BLUE 0x65b9ff
#define GREEN 0x71dfac
#define HISTORY 32

static void label(struct limine_framebuffer *fb, const char *s, size_t x, size_t y,
                  size_t width, uint32_t fg, uint32_t bg) {
    for (size_t i = 0; s[i] && (i + 1) * FONT_WIDTH <= width; ++i)
        draw_char(fb, s[i], x + i * FONT_WIDTH, y, fg, bg);
}

static unsigned share(uint64_t runtime, uint64_t period) {
    if (!period) return 0;
    uint64_t value = (runtime * 1000 + period / 2) / period;
    return value > 1000 ? 1000 : value;
}

static void percent(char text[32], unsigned tenths) {
    format_uint(text, tenths / 10);
    size_t n = 0;
    while (text[n]) ++n;
    text[n++] = '.'; text[n++] = '0' + tenths % 10;
    text[n++] = '%'; text[n] = 0;
}

static void bar(struct limine_framebuffer *fb, size_t x, size_t y, size_t w,
                size_t h, unsigned value, uint32_t color) {
    draw_rect(fb, x, y, w, h, 0x304454);
    draw_rect(fb, x, y, w * value / 1000, h, color);
}

static void select_task(struct scheduler_sample *sample, unsigned *selected, bool back) {
    unsigned i = 0;
    while (i < TASK_SLOTS && sample->tasks[i].id != *selected) ++i;
    if (i == TASK_SLOTS) i = 0;
    for (unsigned n = 0; n < TASK_SLOTS; ++n) {
        i = (i + (back ? TASK_SLOTS - 1 : 1)) % TASK_SLOTS;
        if (sample->tasks[i].id) { *selected = sample->tasks[i].id; return; }
    }
}

void program_view(struct limine_framebuffer *fb) {
    struct scheduler_sample before, now;
    unsigned cpu[HISTORY] = {0}, count = 0, selected = 1;
    unsigned shares[TASK_SLOTS] = {0};
    bool redraw = true, sampled = false;
    const char *status = "Up/Down select   K stop   Q/Esc back";
    size_t width = ui_width(fb), height = ui_height(fb);
    size_t margin = 12, gap = 12, card_w = (width - 2 * margin - gap) / 2;
    size_t table_y = 229, row_h = FONT_HEIGHT + 9;
    size_t visible = height > table_y + 2 * FONT_HEIGHT ?
        (height - table_y - 2 * FONT_HEIGHT) / row_h : 1;
    if (visible > TASK_SLOTS) visible = TASK_SLOTS;
    size_t scroll = 0;
    terminal_clear(fb);
    draw_rect(fb, 0, 24, width, height - 24, BG);
    label(fb, "PROGRAM MANAGER", margin, 39, width - 24, WHITE, BG);
    label(fb, "Guest CPU / one-second samples", margin, 66, width - 24, MUTED, BG);
    scheduler_sample(&now);
    before = now;
    uint64_t last_tick = scheduler_ticks();
    for (;;) {
        terminal_tick(fb, NULL);
        uint64_t tick = scheduler_ticks();
        if (tick - last_tick >= 100) {
            before = now;
            scheduler_sample(&now);
            last_tick = tick;
            uint64_t period = now.timestamp - before.timestamp;
            unsigned idle = share(now.idle_runtime - before.idle_runtime, period);
            if (count == HISTORY) {
                for (unsigned i = 1; i < HISTORY; ++i) cpu[i-1] = cpu[i];
            } else ++count;
            cpu[count-1] = 1000 - idle;
            for (unsigned i = 0; i < TASK_SLOTS; ++i) {
                struct task_info *t = &now.tasks[i];
                uint64_t prior = t->id == before.tasks[i].id ? before.tasks[i].runtime : 0;
                shares[i] = share(t->runtime - prior, period);
            }
            sampled = true;
            redraw = true;
        }
        if (redraw) {
            char text[32];
            struct page_stats ram = pages_stats();
            size_t used = ram.boot + ram.managed - ram.available;
            size_t total = ram.boot + ram.managed;
            for (unsigned card = 0; card < 2; ++card)
                draw_rect(fb, margin + card * (card_w + gap), 101, card_w, 110, PANEL);
            size_t x = margin + 12;
            label(fb, "CPU", x, 108, card_w - 24, MUTED, PANEL);
            if (sampled) percent(text, cpu[count-1]);
            label(fb, sampled ? text : "Sampling...", x, 134, card_w - 24, WHITE, PANEL);
            size_t graph_x = x, graph_y = 169, graph_w = card_w - 24, graph_h = 30;
            draw_rect(fb, graph_x, graph_y, graph_w, graph_h, 0x12202b);
            for (unsigned i = 0; i < count; ++i) {
                size_t left = (HISTORY - count + i) * graph_w / HISTORY;
                size_t right = (HISTORY - count + i + 1) * graph_w / HISTORY;
                size_t h = cpu[i] * graph_h / 1000;
                if (!h) h = 1;
                draw_rect(fb, graph_x + left, graph_y + graph_h - h,
                          right - left > 1 ? right - left - 1 : 1, h, BLUE);
            }
            x = margin + card_w + gap + 12;
            label(fb, "RAM USED", x, 108, card_w - 24, MUTED, PANEL);
            format_mb(text, used * PAGE_SIZE);
            label(fb, text, x, 134, card_w - 24, WHITE, PANEL);
            bar(fb, x, 163, card_w - 24, 7, total ? used * 1000 / total : 0, GREEN);
            format_mb(text, ram.available * PAGE_SIZE);
            label(fb, text, x, 178, card_w - 24 - 5 * FONT_WIDTH, MUTED, PANEL);
            label(fb, "free", margin + 2 * card_w + gap - 4 * FONT_WIDTH,
                  178, 4 * FONT_WIDTH, MUTED, PANEL);
            unsigned slots[TASK_SLOTS], tasks = 0, selected_row = 0;
            bool present = false;
            for (unsigned i = 0; i < TASK_SLOTS; ++i) if (now.tasks[i].id) {
                if (now.tasks[i].id == selected) { selected_row = tasks; present = true; }
                slots[tasks++] = i;
            }
            if (!present && tasks) { selected = now.tasks[slots[0]].id; selected_row = 0; }
            if (selected_row < scroll) scroll = selected_row;
            if (selected_row >= scroll + visible) scroll = selected_row - visible + 1;
            size_t cpu_x = width - 168;
            draw_rect(fb, margin, table_y - 16, width - 2 * margin,
                      height - FONT_HEIGHT - table_y + 16, BG);
            for (size_t row = 0; row < visible && scroll + row < tasks; ++row) {
                unsigned slot = slots[scroll + row];
                struct task_info *t = &now.tasks[slot];
                size_t y = table_y + row * row_h;
                uint32_t bg = t->id == selected ? 0x29465d : PANEL;
                draw_rect(fb, margin, y, width - 2 * margin, row_h - 3, bg);
                format_uint(text, t->id);
                label(fb, text, margin + 8, y + 2, 5 * FONT_WIDTH, MUTED, bg);
                label(fb, t->name, margin + 7 * FONT_WIDTH, y + 2,
                      cpu_x - margin - 8 * FONT_WIDTH, WHITE, bg);
                label(fb, t->ring == 3 ? "user" : "kernel", cpu_x - 8 * FONT_WIDTH,
                      y + 2, 6 * FONT_WIDTH, MUTED, bg);
                if (sampled) percent(text, shares[slot]);
                label(fb, sampled ? text : "--", cpu_x, y + 2, 7 * FONT_WIDTH, WHITE, bg);
                bar(fb, cpu_x + 7 * FONT_WIDTH, y + 9, 56, 7, shares[slot], BLUE);
            }
            size_t detail_y = table_y + visible * row_h + 4;
            if (detail_y + FONT_HEIGHT < height - FONT_HEIGHT) {
                for (unsigned i = 0; i < TASK_SLOTS; ++i) if (now.tasks[i].id == selected)
                    label(fb, *now.tasks[i].output ? now.tasks[i].output : "No recent output",
                          margin, detail_y, width - 2 * margin, MUTED, BG);
            }
            draw_rect(fb, 0, height - FONT_HEIGHT, width, FONT_HEIGHT, BG);
            label(fb, status, margin, height - FONT_HEIGHT, width - 2 * margin, MUTED, BG);
            redraw = false;
        }
        char key = keyboard_poll_char();
        if (key == KEY_ESC || key == 'q') break;
        if (key == KEY_DOWN || key == KEY_UP || key == 'n') {
            select_task(&now, &selected, key == KEY_UP);
            status = "Up/Down select   K stop   Q/Esc back";
            redraw = true;
        }
        if (key == 'k') {
            if (scheduler_stop(selected)) {
                for (unsigned i = 0; i < TASK_SLOTS; ++i)
                    if (now.tasks[i].id == selected) now.tasks[i].id = 0;
                selected = 1;
                scheduler_reap();
                status = "Task stopped. Up/Down select   K stop   Q/Esc back";
            } else status = "Kernel shell is protected. Select a background task.";
            redraw = true;
        }
        if (!redraw) scheduler_wait_input();
    }
    terminal_clear(fb);
}
