#include "clock_view.h"
#include "analog_clock.h"
#include "terminal.h"
#include "arch/x86_64/clock.h"
#include "drivers/keyboard/keyboard.h"
#include "gfx/framebuffer.h"

static const char *const digits[11][3] = {
    {" _ ", "| |", "|_|"}, {"   ", "  |", "  |"}, {" _ ", " _|", "|_ "},
    {" _ ", " _|", " _|"}, {"   ", "|_|", "  |"}, {" _ ", "|_ ", " _|"},
    {" _ ", "|_ ", "|_|"}, {" _ ", "  |", "  |"}, {" _ ", "|_|", "|_|"},
    {" _ ", "|_|", " _|"}, {" ", ".", "."}
};

void clock_view(struct limine_framebuffer *fb) {
    terminal_clear(fb);
    terminal_write(fb, "Clock (UTC)    Q/Esc back\n");
    const size_t width = 28 * FONT_WIDTH;
    size_t x = ui_width(fb) > width ? (ui_width(fb) - width) / 2 : 0;
    // Reserve room for the title, face numerals and five rows of digital time.
    int available = (int)ui_height(fb) - 60 - 7 * FONT_HEIGHT;
    int radius = available / 2;
    int horizontal_limit = (int)ui_width(fb) / 2 - 2 * FONT_WIDTH;
    if (radius > horizontal_limit) radius = horizontal_limit;
    struct clock_face face = {(int)ui_width(fb) / 2, 60 + FONT_HEIGHT / 2 + radius, radius};
    bool show_face = radius >= 45;
    size_t y = show_face ? (size_t)(face.y + radius + FONT_HEIGHT) : ui_height(fb) / 2;
    if (show_face) clock_face_draw(fb, face);
    struct clock_sample previous = {0};
    unsigned last_second = 60, last_ms = 1000;
    for (;;) {
        char key = keyboard_poll_char();
        if (key == KEY_ESC || key == 'q') break;
        struct clock_sample t;
        if (!terminal_tick(fb, &t) || (last_ms == t.millisecond && last_second == t.second)) {
            asm volatile("pause");
            continue;
        }
        if (last_second != t.second) {
            if (show_face) {
                if (last_second != 60) clock_hands_draw(fb, face, &previous, true);
                clock_hands_draw(fb, face, &t, false);
                previous = t;
            }
            unsigned values[] = {t.hour/10, t.hour%10, 10, t.minute/10, t.minute%10,
                                 10, t.second/10, t.second%10};
            for (unsigned row = 0; row < 3; ++row) {
                char line[32];
                unsigned pos = 0;
                for (unsigned i = 0; i < 8; ++i) {
                    const char *p = digits[values[i]][row];
                    while (*p) line[pos++] = *p++;
                    line[pos++] = ' ';
                }
                line[pos] = 0;
                draw_string(fb, line, x, y + row * FONT_HEIGHT, 0xffffff, 0);
            }
        }
        char fraction[] = ".000 s";
        fraction[1] += t.millisecond / 100;
        fraction[2] += t.millisecond / 10 % 10;
        fraction[3] += t.millisecond % 10;
        size_t fy = y + 4 * FONT_HEIGHT;
        size_t fx = ui_width(fb) >= 6 * FONT_WIDTH ? (ui_width(fb) - 6 * FONT_WIDTH) / 2 : 0;
        if (fy + FONT_HEIGHT <= ui_height(fb) && fx + 6 * FONT_WIDTH <= ui_width(fb)) {
            draw_string(fb, fraction, fx, fy, 0xffffff, 0);
        }
        last_second = t.second;
        last_ms = t.millisecond;
    }
    terminal_clear(fb);
}
