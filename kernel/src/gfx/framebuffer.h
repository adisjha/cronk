#pragma once

#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#define FONT_WIDTH 12
#define FONT_HEIGHT 23

void draw_char(struct limine_framebuffer *fb, char c,
                      size_t x, size_t y, uint32_t color, uint32_t bg);
void draw_string(struct limine_framebuffer *fb, const char *s,
                        size_t x, size_t y, uint32_t color, uint32_t bg);
void draw_rect(struct limine_framebuffer *fb, size_t x, size_t y, size_t w, size_t h, uint32_t color);
