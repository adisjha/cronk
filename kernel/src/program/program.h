#pragma once

#include <limine.h>
#include <stddef.h>
#include <stdbool.h>

void program_install(void);
void program_run(struct limine_framebuffer *fb, const char *name, const void *image, size_t size, bool background);
