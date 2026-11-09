#include "framebuffer.h"
#include "font.h"
#include <stdbool.h>

static void put_pixel(struct limine_framebuffer *fb, size_t x, size_t y, uint32_t color){
    volatile uint32_t *fb_ptr = fb->address;
    fb_ptr[y * (fb->pitch / 4) + x] = color;
}

void draw_char(struct limine_framebuffer *fb, char c,
                      size_t x, size_t y, uint32_t color, uint32_t bg) {
    unsigned char code = (unsigned char)c;
    const uint16_t *glyph = font[(code >= 32 && code <= 126 ? code : '?') - 32];

    for (size_t row = 0; row < FONT_HEIGHT && y + row < fb->height; row++) {
        for (size_t col = 0; col < FONT_WIDTH && x + col < fb->width; col++) {
            bool ink = glyph[row] & (1u << (FONT_WIDTH - 1 - col));
            put_pixel(fb, x + col, y + row, ink ? color : bg);
        }
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
    for (size_t yy = 0; yy < h; ++yy){
        for (size_t xx = 0; xx < w; ++xx){
            put_pixel(fb, x + xx, y + yy, color);
        }
    }
}
