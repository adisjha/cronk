#include "editor.h"
#include "mm/pages.h"
#include "format.h"
#include "terminal.h"
#include "gfx/framebuffer.h"
#include "drivers/keyboard/keyboard.h"
#include "storage/files.h"
#include "sched/scheduler.h"
#include "lib/memory.h"

// Every character occupies one display cell; newline advances to the next row.
static void location(const char *text, size_t pos, size_t cols, size_t *row, size_t *col) {
    *row = *col = 0;
    for (size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') { ++*row; *col = 0; }
        else if (++*col == cols) { ++*row; *col = 0; }
    }
}

static size_t vertical(const char *text, size_t len, size_t cols, size_t row, size_t col) {
    size_t best = 0;
    for (size_t i = 0; i <= len; ++i) {
        size_t r, c;
        location(text, i, cols, &r, &c);
        if (r > row || (r == row && c > col)) break;
        best = i;
    }
    return best;
}

void editor_open(struct limine_framebuffer *fb, char *args) {
    while (*args == ' ') ++args;
    char *name = args, *end = args;
    if (*args == '"') {
        name = ++end;
        while (*end && *end != '"') ++end;
        if (*end != '"') { terminal_write(fb, "missing closing quote\n"); return; }
        *end++ = 0;
    } else {
        while (*end && *end != ' ') ++end;
        if (*end) *end++ = 0;
    }
    while (*end == ' ') ++end;
    if (!*name || *end) { terminal_write(fb, "usage: edit PATH (quote paths containing spaces)\n"); return; }
    unsigned buffer_pages = 2 * (FILE_TEXT_MAX + 1) / PAGE_SIZE;
    char *text = pages_alloc(buffer_pages, PAGE_BUFFER);
    if(!text){terminal_write(fb,"unable to allocate editor buffers\n");return;}
    char *saved = text + FILE_TEXT_MAX + 1;
    scheduler_charge_current_memory((int64_t)buffer_pages * PAGE_SIZE);
    size_t len, saved_len;
    const char *error = files_text_load(name, text, &len);
    if (error) { terminal_write(fb, error); terminal_putchar(fb, '\n'); goto cleanup; }
    memcpy(saved, text, len + 1);
    saved_len = len;
    size_t pos = 0, top = 0;
    size_t cols = ui_width(fb) / FONT_WIDTH;
    size_t rows = (ui_height(fb) - 24) / FONT_HEIGHT;
    if (!cols || rows < 4) goto cleanup;
    rows -= 3;
    const char *status = "F2 save   Esc quit   Arrows move   Backspace/Delete erase";
    bool confirm = false;
    terminal_clear(fb);
    for (;;) {
        bool dirty = len != saved_len || memcmp(text, saved, len);
        size_t row, col;
        location(text, pos, cols, &row, &col);
        if (row < top) top = row;
        if (row >= top + rows) top = row - rows + 1;
        draw_rect(fb, 0, 24, ui_width(fb), ui_height(fb) - 24, 0);
        // Clip the path and status to the screen width.
        const char *title = "EDIT ";
        for (size_t i = 0; title[i] && i < cols; ++i)
            draw_char(fb, title[i], i * FONT_WIDTH, 24, 0xffffff, 0);
        for (size_t i = 0; name[i] && i + 6 < cols; ++i)
            draw_char(fb, name[i], (i + 5) * FONT_WIDTH, 24, 0xffffff, 0);
        if (dirty) draw_char(fb, '*', (cols - 1) * FONT_WIDTH, 24, 0xffcc66, 0);
        size_t r = 0, c = 0;
        for (size_t i = 0; i < len; ++i) {
            if (text[i] == '\n') { ++r; c = 0; continue; }
            if (r >= top && r < top + rows)
                draw_char(fb, text[i] >= 32 && text[i] < 127 ? text[i] : '?',
                          c * FONT_WIDTH, 24 + (r - top + 1) * FONT_HEIGHT, 0xffffff, 0);
            if (++c == cols) { ++r; c = 0; }
        }
        char cursor = pos < len && text[pos] >= 32 && text[pos] < 127 ? text[pos] : ' ';
        draw_char(fb, cursor, col * FONT_WIDTH, 24 + (row - top + 1) * FONT_HEIGHT, 0, 0xffffff);
        for (size_t i = 0; status[i] && i < cols; ++i)
            draw_char(fb, status[i], i * FONT_WIDTH, 24 + (rows + 1) * FONT_HEIGHT, 0xffcc66, 0);
        char usage[32]; format_uint(usage,len);
        size_t usage_len=0;while(usage[usage_len])++usage_len;
        const char *suffix=" / 65535 bytes";while(*suffix)usage[usage_len++]=*suffix++;usage[usage_len]=0;
        draw_string(fb, usage, 0, 24 + (rows + 2) * FONT_HEIGHT, 0xaaaaaa, 0);
        char key;
        while (!(key = keyboard_poll_char())) {
            terminal_tick(fb, NULL);
            scheduler_wait_input();
        }
        if (confirm) {
            confirm = false;
            if (key == 'y' || key == 'Y') break;
            status = "F2 save   Esc quit   Arrows move   Backspace/Delete erase";
            continue;
        }
        if (key == KEY_ESC) {
            if (!dirty) break;
            confirm = true;
            status = "Discard unsaved changes? Y discard / any other key cancel";
        } else if (key == KEY_F2) {
            error = files_text_save(name, text, len);
            status = error ? error : "Saved. F2 save   Esc quit";
            if (!error) { memcpy(saved, text, len + 1); saved_len = len; }
        } else if (key == KEY_LEFT) { if (pos) --pos; }
        else if (key == KEY_RIGHT) { if (pos < len) ++pos; }
        else if (key == KEY_UP) { if (row) pos = vertical(text, len, cols, row - 1, col); }
        else if (key == KEY_DOWN) {
            size_t last_row, last_col;
            location(text, len, cols, &last_row, &last_col);
            if (row < last_row) pos = vertical(text, len, cols, row + 1, col);
        } else if (key == '\b' || key == KEY_DELETE) {
            if (key == '\b' && pos) {
                memmove(text + pos - 1, text + pos, len - pos + 1);
                --pos; --len;
            } else if (key == KEY_DELETE && pos < len) {
                memmove(text + pos, text + pos + 1, len - pos);
                --len;
            }
        } else if (key == '\n' || (key >= 32 && key < 127)) {
            if (len == FILE_TEXT_MAX) status = "File full (65535 bytes). Delete text before typing more.";
            else {
                memmove(text + pos + 1, text + pos, len - pos + 1);
                text[pos++] = key;
                ++len;
            }
        }
    }
cleanup:
    scheduler_charge_current_memory(-(int64_t)buffer_pages * PAGE_SIZE);
    pages_free(text,buffer_pages);
    terminal_clear(fb);
}
