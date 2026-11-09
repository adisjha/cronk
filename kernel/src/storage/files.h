#pragma once
#include <limine.h>
#include <stddef.h>

void files_install_program(const char *name, unsigned marker, const void *data, size_t size);

enum file_action { FILE_LIST, FILE_READ, FILE_WRITE, FILE_REMOVE, FILE_RUN, FILE_RUNBG };
void files_command(struct limine_framebuffer *fb, enum file_action action, char *args);
