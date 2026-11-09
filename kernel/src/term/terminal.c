#include "terminal.h"
#include "lib/memory.h"
#include "sched/scheduler.h"
#include "gfx/framebuffer.h"

#define TERM_BG 0x00000000
#define TERM_FG 0x00ffffff
#define MENUBAR_H 24

static size_t term_col = 0;
static size_t term_row = 0;


static unsigned displayed_minute = 24 * 60;

bool terminal_tick(struct limine_framebuffer *fb, struct clock_sample *sample) {
    scheduler_reap();
    struct clock_sample now;
    if (!clock_read(&now)) return false;
    if (sample) *sample = now;
    unsigned minute = now.hour * 60 + now.minute;
    size_t width = 5 * FONT_WIDTH;
    if (minute != displayed_minute && fb->width >= width + 12 && fb->height >= MENUBAR_H) {
        size_t x = fb->width - width - 6;
        char text[] = "00:00";
        text[0] += now.hour / 10;
        text[1] += now.hour % 10;
        text[3] += now.minute / 10;
        text[4] += now.minute % 10;
        draw_string(fb, text, x, 0, TERM_FG, 0x00222222);
        displayed_minute = minute;
    }
    return true;
}

static void draw_topbar(struct limine_framebuffer *fb) {
    draw_rect(fb, 0, 0, fb->width, MENUBAR_H, 0x00222222);

    draw_string(fb,
        "cronk   programs [F1]   mem [F2]   time [F3]   help",
        6, 0,
        TERM_FG, 0x00222222);
}
void terminal_draw_cursor(struct limine_framebuffer *fb){
    size_t char_w = FONT_WIDTH;
    size_t char_h = FONT_HEIGHT;

    draw_rect(fb, term_col * char_w, MENUBAR_H + term_row * char_h, char_w, char_h, TERM_FG);
}

void terminal_erase_cursor(struct limine_framebuffer *fb){
    size_t char_w = FONT_WIDTH;
    size_t char_h = FONT_HEIGHT; 

    draw_rect(fb, term_col * char_w, MENUBAR_H + term_row * char_h, char_w, char_h, TERM_BG);
}

// The live views stream a new layout from the top left on every refresh.
// Cache the common case; larger screens still paint uncached cells directly.
static char cells[8192];
static size_t position, previous_end;
static bool refreshing;

void terminal_begin_refresh(void) {
    position = 0;
    refreshing = true;
}

static void paint_cell(struct limine_framebuffer *fb, size_t cols, char c) {
    size_t cell = position++;
    if (cell < sizeof(cells)) {
        char stored = c == ' ' ? 0 : c;
        if (cells[cell] == stored) return;
        cells[cell] = stored;
    }
    draw_char(fb, c, (cell % cols) * FONT_WIDTH,
              MENUBAR_H + (cell / cols) * FONT_HEIGHT, TERM_FG, TERM_BG);
}

void terminal_end_refresh(struct limine_framebuffer *fb) {
    size_t end = position;
    while (position < previous_end) paint_cell(fb, fb->width / FONT_WIDTH, ' ');
    previous_end = end;
    refreshing = false;
}

void terminal_putchar(struct limine_framebuffer *fb, char c){
    size_t w = FONT_WIDTH;
    size_t h = FONT_HEIGHT;

    size_t cols = fb->width / w;
    size_t rows = (fb->height - MENUBAR_H) / h;

    if (refreshing) {
        if (!cols || position >= rows * cols) return;
        if (c == '\n') {
            size_t next = position + cols - position % cols;
            while (position < next && position < previous_end) paint_cell(fb, cols, ' ');
            position = next;
        } else {
            paint_cell(fb, cols, c);
        }
        return;
    }

    if (c == '\n'){
        term_col = 0;
        ++term_row;
    }
    else{
        draw_char(fb, c, term_col * w, MENUBAR_H + term_row * h, TERM_FG, TERM_BG);
        ++term_col;
        if (term_col >= cols){
            term_col = 0;
            ++term_row;
        }
    }
    if (term_row >= rows){
        terminal_clear(fb);
        term_col = 0;
        term_row = 0;
    }
}

void terminal_write(struct limine_framebuffer *fb, const char *s){
    while (*s) {
        terminal_putchar(fb, *s++);
    }
}


void terminal_clear(struct limine_framebuffer *fb) {
    previous_end = 0;
    memset(cells, 0, sizeof(cells));
    draw_rect(fb, 0, 0, fb->width, fb->height, TERM_BG);
    draw_topbar(fb);
    displayed_minute = 24 * 60;
    terminal_tick(fb, NULL);
    term_col = 0;
    term_row = 0;
}

void terminal_backspace(struct limine_framebuffer *fb) {
    if (term_col > 0) {
        --term_col;
    } else if (term_row > 0) {
        --term_row;
        term_col = fb->width / (FONT_WIDTH) - 1;
    } else {
        return;
    }
    terminal_erase_cursor(fb);
}

void terminal_write_uint(struct limine_framebuffer *fb, uint64_t value) {
    char text[21];
    char *p = text + sizeof(text) - 1;
    *p = 0;
    do {
        *--p = '0' + value % 10;
        value /= 10;
    } while (value);
    terminal_write(fb, p);
}
