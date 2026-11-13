#pragma once

#include <limine.h>
#include "arch/clock.h"

bool terminal_tick(struct limine_framebuffer *fb, struct clock_sample *sample);

void terminal_draw_cursor(struct limine_framebuffer *fb);
void terminal_erase_cursor(struct limine_framebuffer *fb);
void terminal_putchar(struct limine_framebuffer *fb, char c);
void terminal_write(struct limine_framebuffer *fb, const char *s);
void terminal_clear(struct limine_framebuffer *fb);
void terminal_backspace(struct limine_framebuffer *fb);

void terminal_write_uint(struct limine_framebuffer *fb, uint64_t value);

void terminal_begin_refresh(void);
void terminal_end_refresh(struct limine_framebuffer *fb);

enum terminal_tab { TAB_SHELL, TAB_APPS, TAB_HELP };
void terminal_tab(struct limine_framebuffer *fb, enum terminal_tab tab);
void terminal_history(struct limine_framebuffer *fb, int direction);
bool terminal_at_bottom(void);
