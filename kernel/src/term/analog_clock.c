#include "analog_clock.h"
#include "gfx/framebuffer.h"

// Quarter-wave samples at six-degree intervals, in units of 1/1024.
static const int quarter[] = {0,107,213,316,417,512,602,685,761,828,887,935,974,1002,1018,1024};

static int sine(int tick) {
    tick %= 60;
    if (tick >= 30) return -sine(tick - 30);
    return quarter[tick > 15 ? 30 - tick : tick];
}

struct point { int x, y; };

// One revolution is 3600 units; interpolate between adjacent minute marks.
static struct point polar(struct clock_face f, int angle, int length) {
    int i = angle / 60, frac = angle % 60;
    int dx = sine(i) * (60 - frac) + sine(i + 1) * frac;
    int dy = sine(i + 15) * (60 - frac) + sine(i + 16) * frac;
    return (struct point){f.x + dx * length / 61440, f.y - dy * length / 61440};
}

static void stroke(struct limine_framebuffer *fb, struct point a, struct point b,
                   int width, uint32_t color) {
    int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int dx = (b.x - a.x) * sx, dy = (a.y - b.y) * sy;
    int error = dx + dy;
    for (;;) {
        draw_rect(fb, a.x - width/2, a.y - width/2, width, width, color);
        if (a.x == b.x && a.y == b.y) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; a.x += sx; }
        if (twice <= dx) { error += dx; a.y += sy; }
    }
}

void clock_face_draw(struct limine_framebuffer *fb, struct clock_face f) {
    for (int i = 0; i < 60; ++i) {
        bool hour = i % 5 == 0;
        int start = f.radius * (hour ? 79 : 84) / 100;
        stroke(fb, polar(f, i * 60, start), polar(f, i * 60, f.radius * 88 / 100),
               hour ? 3 : 1, hour ? 0xd4dce5 : 0x667684);
        if (hour && (f.radius >= 100 || i % 15 == 0)) {
            int n = i ? i / 5 : 12;
            char label[3];
            unsigned len = 0;
            if (n >= 10) label[len++] = '1';
            label[len++] = '0' + n % 10;
            label[len] = 0;
            struct point p = polar(f, i * 60, f.radius);
            draw_string(fb, label, p.x - (int)len * FONT_WIDTH / 2,
                        p.y - FONT_HEIGHT / 2, 0xe9edf1, 0);
        }
    }
}

void clock_hands_draw(struct limine_framebuffer *fb, struct clock_face f,
                      const struct clock_sample *t, bool erase) {
    struct point center = {f.x, f.y};
    int hour = (t->hour % 12) * 300 + t->minute * 5 + t->second / 12;
    stroke(fb, center, polar(f, hour, f.radius / 2), f.radius >= 130 ? 7 : 3,
           erase ? 0 : 0xf3f5f7);
    stroke(fb, center, polar(f, t->minute * 60 + t->second, f.radius * 67 / 100),
           f.radius >= 130 ? 4 : 2, erase ? 0 : 0xd0d9e1);
    stroke(fb, polar(f, t->second * 60 + 1800, f.radius / 9),
           polar(f, t->second * 60, f.radius * 74 / 100), 2, erase ? 0 : 0xff815c);
}
