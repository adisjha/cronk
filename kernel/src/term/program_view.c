#include "program_view.h"
#include "arch/cpu.h"
#include "drivers/keyboard/keyboard.h"
#include "format.h"
#include "gfx/framebuffer.h"
#include "mm/pages.h"
#include "sched/scheduler.h"
#include "terminal.h"

#define BG 0x101820
#define PANEL 0x1b2935
#define MUTED 0x9eafbf
#define WHITE 0xf1f5fa
#define BLUE 0x65b9ff
#define GREEN 0x71dfac
#define HISTORY 32

static void label(struct limine_framebuffer *fb, const char *s, size_t x, size_t y, size_t width,
                  uint32_t fg, uint32_t bg) {
    for (size_t i = 0; s[i] && (i + 1) * FONT_WIDTH <= width; ++i)
        draw_char(fb, s[i], x + i * FONT_WIDTH, y, fg, bg);
}

static unsigned share(uint64_t runtime, uint64_t period) {
    if (!period)
        return 0;
    uint64_t value = (runtime * 1000 + period / 2) / period;
    return value > 1000 ? 1000 : value;
}

static void percent(char text[32], unsigned tenths) {
    format_uint(text, tenths / 10);
    size_t n = 0;
    while (text[n])
        ++n;
    text[n++] = '.';
    text[n++] = '0' + tenths % 10;
    text[n++] = '%';
    text[n] = 0;
}

static void memory_size(char text[32], size_t bytes) {
    if (bytes >= 1024 * 1024) {
        format_mb(text, bytes);
        return;
    }
    format_uint(text, (bytes + 1023) / 1024);
    size_t n = 0;
    while (text[n])
        ++n;
    const char *unit = " KiB";
    while (*unit)
        text[n++] = *unit++;
    text[n] = 0;
}

static void bar(struct limine_framebuffer *fb, size_t x, size_t y, size_t w, size_t h,
                unsigned value, uint32_t color) {
    draw_rect(fb, x, y, w, h, 0x304454);
    draw_rect(fb, x, y, w * value / 1000, h, color);
}

struct manager_row {
    unsigned id, cpu, core;
    const char *name, *kind, *detail;
    uint64_t bytes;
};

void program_view(struct limine_framebuffer *fb) {
    struct scheduler_sample before, now;
    scheduler_sample(&now);
    unsigned history[HISTORY] = {0}, count = 0;
    unsigned shares[TASK_SLOTS] = {0}, core_shares[SCHEDULER_MAX_CPUS] = {0};
    unsigned core_history[SCHEDULER_MAX_CPUS][HISTORY] = {{0}};
    unsigned selected = 0;
    size_t scroll = 0;
    bool redraw = true, sampled = false;
    const char *status = "Up/Down select   K stop task   Q/Esc back";
    size_t width = ui_width(fb), height = ui_height(fb);
    size_t margin = 12, gap = 12, card_w = (width - 2 * margin - gap) / 2;
    size_t card_h = now.cpu_count > 4 && height >= 450 ? 170 : 110;
    size_t table_y = 245 + card_h - 110, row_h = FONT_HEIGHT + 9;
    size_t visible =
        height > table_y + 2 * FONT_HEIGHT ? (height - table_y - 2 * FONT_HEIGHT) / row_h : 1;
    if (!visible)
        visible = 1;
    struct manager_row rows[TASK_SLOTS + 2];
    unsigned row_count = 0;
    terminal_clear(fb);
    draw_rect(fb, 0, 24, width, height - 24, BG);
    label(fb, "PROGRAM MANAGER", margin, 39, width - 24, WHITE, BG);
    label(fb, "Live tasks and system memory / CPU and RAM per row", margin, 66, width - 24, MUTED,
          BG);
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
            uint64_t busy = 0;
            for (unsigned c = 0; c < SCHEDULER_MAX_CPUS; ++c)
                if (now.cpus[c].online) {
                    uint64_t delta = now.cpus[c].busy - before.cpus[c].busy;
                    core_shares[c] = share(delta, period);
                    busy += delta;
                }
            unsigned usage = share(busy, period * now.cpu_count);
            if (count == HISTORY) {
                for (unsigned i = 1; i < HISTORY; ++i) {
                    history[i - 1] = history[i];
                    for (unsigned c = 0; c < SCHEDULER_MAX_CPUS; ++c)
                        core_history[c][i - 1] = core_history[c][i];
                }
            } else
                ++count;
            history[count - 1] = usage;
            for (unsigned c = 0; c < SCHEDULER_MAX_CPUS; ++c)
                core_history[c][count - 1] = core_shares[c];
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
            struct memory_breakdown parts = pages_breakdown();
            size_t used = ram.boot + ram.managed - ram.available;
            size_t total = ram.boot + ram.managed;
            row_count = 0;
            uint64_t attributed = 0;
            for (unsigned i = 0; i < TASK_SLOTS; ++i) {
                struct task_info *t = &now.tasks[i];
                if (!t->id)
                    continue;
                const char *kind = t->service                   ? "service"
                                   : t->ring == ARCH_USER_LEVEL ? "user"
                                                                : "task";
                rows[row_count++] = (struct manager_row){
                    t->id,
                    shares[i],
                    t->cpu,
                    t->name,
                    kind,
                    t->service ? "Protected service. RAM includes its stack and owned buffers."
                               : "RAM includes the task stack and private program pages; shared "
                                 "kernel memory is listed separately.",
                    t->ram_bytes};
                if (t->id == 1002)
                    rows[row_count - 1].bytes = SERVICE_STACK_SIZE + parts.metadata * PAGE_SIZE;
                attributed += rows[row_count - 1].bytes;
            }
            // The shell's explicit 64 KiB boot stack belongs to it, not this reserve.
            uint64_t boot = parts.bootloader * PAGE_SIZE;
#if !defined(__aarch64__)
            boot = boot > 65536 ? boot - 65536 : 0;
#endif
            rows[row_count++] = (struct manager_row){0,
                                                     0,
                                                     0,
                                                     "boot memory",
                                                     "reserve",
                                                     "Retained bootloader tables, stacks and boot "
                                                     "allocations. This is not a running process.",
                                                     boot};
            uint64_t used_bytes = used * PAGE_SIZE;
            uint64_t shared = used_bytes > attributed + boot ? used_bytes - attributed - boot : 0;
            rows[row_count++] =
                (struct manager_row){0,
                                     0,
                                     0,
                                     "kernel / shared",
                                     "reserve",
                                     "Shared kernel code/data, inactive stack slots, page tables "
                                     "and remaining unassigned allocations.",
                                     shared};
            if (selected >= row_count)
                selected = row_count - 1;
            if (selected < scroll)
                scroll = selected;
            if (selected >= scroll + visible)
                scroll = selected - visible + 1;
            for (unsigned card = 0; card < 2; ++card)
                draw_rect(fb, margin + card * (card_w + gap), 101, card_w, card_h, PANEL);
            size_t x = margin + 12;
            label(fb, "CPU / PER CORE", x, 108, card_w - 24, MUTED, PANEL);
            if (now.cpu_count == 1) {
                if (sampled)
                    percent(text, history[count - 1]);
                label(fb, sampled ? text : "Sampling...", x, 134, card_w - 24, WHITE, PANEL);
                bar(fb, x, 174, card_w - 24, 7, history[count ? count - 1 : 0], BLUE);
            } else {
                unsigned columns = now.cpu_count > 4 && height < 450 ? 4 : 2;
                for (unsigned c = 0, shown = 0; c < SCHEDULER_MAX_CPUS; ++c) {
                    if (!now.cpus[c].online)
                        continue;
                    size_t col = shown % columns, y = 134 + (shown / columns) * 32,
                           left = x + col * (card_w - 24) / columns;
                    ++shown;
                    char name[32];
                    format_uint(name, c);
                    if (columns == 4) {
                        label(fb, name, left, y, FONT_WIDTH, MUTED, PANEL);
                        format_uint(text, core_shares[c] / 10);
                        label(fb, sampled ? text : "--", left + 2 * FONT_WIDTH, y, 3 * FONT_WIDTH,
                              WHITE, PANEL);
                    } else {
                        label(fb, "C", left, y, FONT_WIDTH, MUTED, PANEL);
                        label(fb, name, left + FONT_WIDTH, y, 2 * FONT_WIDTH, MUTED, PANEL);
                        if (sampled)
                            percent(text, core_shares[c]);
                        label(fb, sampled ? text : "--", left + 3 * FONT_WIDTH, y, 7 * FONT_WIDTH,
                              WHITE, PANEL);
                    }
                    size_t graph_w = (card_w - 24) / columns - 6, graph_y = y + 24;
                    draw_rect(fb, left, graph_y, graph_w, 7, 0x304454);
                    for (unsigned h = 0; h < count; ++h) {
                        size_t gx = (HISTORY - count + h) * graph_w / HISTORY;
                        size_t end = (HISTORY - count + h + 1) * graph_w / HISTORY;
                        size_t gh = core_history[c][h] * 7 / 1000;
                        if (!gh)
                            gh = 1;
                        draw_rect(fb, left + gx, graph_y + 7 - gh, end - gx > 1 ? end - gx - 1 : 1,
                                  gh, BLUE);
                    }
                }
            }
            x = margin + card_w + gap + 12;
            label(fb, "RAM USED", x, 108, card_w - 24, MUTED, PANEL);
            format_mb(text, used_bytes);
            label(fb, text, x, 134, card_w - 24, WHITE, PANEL);
            bar(fb, x, 163, card_w - 24, 7, total ? used * 1000 / total : 0, GREEN);
            format_mb(text, ram.available * PAGE_SIZE);
            label(fb, text, x, 178, card_w - 24 - 5 * FONT_WIDTH, MUTED, PANEL);
            label(fb, "free", margin + 2 * card_w + gap - 4 * FONT_WIDTH, 178, 4 * FONT_WIDTH,
                  MUTED, PANEL);
            size_t kind_x = width - 354, cpu_x = width - 240, ram_x = width - 120;
            draw_rect(fb, margin, table_y - FONT_HEIGHT, width - 2 * margin,
                      height - FONT_HEIGHT - table_y, BG);
            label(fb, "PID / NAME", margin + 8, table_y - FONT_HEIGHT, kind_x - margin, MUTED, BG);
            label(fb, "CORE", kind_x, table_y - FONT_HEIGHT, 7 * FONT_WIDTH, MUTED, BG);
            label(fb, "CPU", cpu_x, table_y - FONT_HEIGHT, 8 * FONT_WIDTH, MUTED, BG);
            label(fb, "RAM", ram_x, table_y - FONT_HEIGHT, 9 * FONT_WIDTH, MUTED, BG);
            for (size_t row = 0; row < visible && scroll + row < row_count; ++row) {
                struct manager_row *r = &rows[scroll + row];
                size_t y = table_y + row * row_h;
                uint32_t bg = scroll + row == selected ? 0x29465d : PANEL;
                draw_rect(fb, margin, y, width - 2 * margin, row_h - 3, bg);
                if (r->id)
                    format_uint(text, r->id);
                label(fb, r->id ? text : "--", margin + 8, y + 2, 5 * FONT_WIDTH, MUTED, bg);
                label(fb, r->name, margin + 7 * FONT_WIDTH, y + 2, kind_x - margin - 8 * FONT_WIDTH,
                      WHITE, bg);
                if (r->id)
                    format_uint(text, r->core);
                label(fb, r->id ? text : "--", kind_x, y + 2, 7 * FONT_WIDTH, MUTED, bg);
                if (sampled)
                    percent(text, r->cpu);
                label(fb, r->id && sampled ? text : "--", cpu_x, y + 2, 7 * FONT_WIDTH, WHITE, bg);
                memory_size(text, r->bytes);
                label(fb, text, ram_x, y + 2, 9 * FONT_WIDTH, r->id ? GREEN : MUTED, bg);
            }
            draw_rect(fb, 0, height - 2 * FONT_HEIGHT, width, 2 * FONT_HEIGHT, BG);
            label(fb, rows[selected].detail, margin, height - 2 * FONT_HEIGHT, width - 2 * margin,
                  MUTED, BG);
            label(fb, status, margin, height - FONT_HEIGHT, width - 2 * margin, MUTED, BG);
            redraw = false;
        }
        char key = keyboard_poll_char();
        if (key == KEY_ESC || key == 'q')
            break;
        if (key == KEY_DOWN || key == KEY_UP || key == 'n') {
            selected = (selected + (key == KEY_UP ? row_count - 1 : 1)) % row_count;
            status = "Up/Down select   K stop task   Q/Esc back";
            redraw = true;
        }
        if (key == 'k') {
            if (scheduler_stop(rows[selected].id)) {
                scheduler_sample(&now);
                status = "Task stopped. Up/Down select   K stop task   Q/Esc back";
            } else
                status = "System services and memory reservations are protected.";
            redraw = true;
        }
        if (!redraw)
            scheduler_wait_input();
    }
    terminal_clear(fb);
}
