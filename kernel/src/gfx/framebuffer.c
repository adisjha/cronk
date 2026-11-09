#include "framebuffer.h"
#include "font.h"
#include <stdbool.h>

static unsigned scale = 1;

unsigned ui_scale(void) { return scale; }

bool ui_set_scale(const struct limine_framebuffer *fb, unsigned value) {
    if (value < 1 || value > 4 || (value > 1 &&
        (fb->width / value < 640 || fb->height / value < 360))) return false;
    scale = value;
    return true;
}

size_t ui_width(const struct limine_framebuffer *fb) { return (fb->width + scale - 1) / scale; }
size_t ui_height(const struct limine_framebuffer *fb) { return (fb->height + scale - 1) / scale; }

void draw_char(struct limine_framebuffer *fb, char c,
                      size_t x, size_t y, uint32_t color, uint32_t bg) {
    unsigned char code = (unsigned char)c;
    const uint16_t *glyph = font[(code >= 32 && code <= 126 ? code : '?') - 32];

    if (x >= ui_width(fb) || y >= ui_height(fb)) return;
    size_t left = x * scale, top = y * scale;
    size_t width = FONT_WIDTH * scale, height = FONT_HEIGHT * scale;
    if (width > fb->width - left) width = fb->width - left;
    if (height > fb->height - top) height = fb->height - top;
    for (size_t row = 0; row < height; ++row) {
        volatile uint32_t *dst = (volatile uint32_t *)fb->address + (top + row) * (fb->pitch / 4) + left;
        uint16_t bits = glyph[row / scale];
        for (size_t col = 0; col < width; ++col)
            dst[col] = bits & (1u << (FONT_WIDTH - 1 - col / scale)) ? color : bg;
    }
}

void draw_string(struct limine_framebuffer *fb, const char *s,
                        size_t x, size_t y, uint32_t color, uint32_t bg) {
    while (*s) {
        draw_char(fb, *s, x, y, color, bg);
        x += FONT_WIDTH;
        s++;
    }
}

void draw_rect(struct limine_framebuffer *fb, size_t x, size_t y, size_t w, size_t h, uint32_t color){
    size_t cols = ui_width(fb), rows = ui_height(fb);
    if (x >= cols || y >= rows) return;
    if (w > cols - x) w = cols - x;
    if (h > rows - y) h = rows - y;
    size_t left = x * scale, top = y * scale;
    size_t right = (x + w) * scale, bottom = (y + h) * scale;
    if (right > fb->width) right = fb->width;
    if (bottom > fb->height) bottom = fb->height;
    for (size_t yy = top; yy < bottom; ++yy) {
        volatile uint32_t *dst = (volatile uint32_t *)fb->address + yy * (fb->pitch / 4);
        for (size_t xx = left; xx < right; ++xx) dst[xx] = color;
    }
}
