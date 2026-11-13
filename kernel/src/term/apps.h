#pragma once
#include <limine.h>

enum app { APP_MENU, APP_STOPWATCH, APP_SYSTEM, APP_PROGRAMS, APP_MEMORY, APP_CLOCK, APP_CALC, APP_FILES, APP_HELP };
void app_open(struct limine_framebuffer *fb, enum app app);

void desktop_start(struct limine_framebuffer *fb);

void desktop_edit(struct limine_framebuffer *fb, char *args);
