#pragma once
#include <limine.h>
#include <stddef.h>
#include <stdbool.h>

void files_install_program(const char *name, unsigned marker, const void *data, size_t size);

enum file_action { FILE_LIST, FILE_READ, FILE_WRITE, FILE_REMOVE, FILE_RUN, FILE_RUNBG,
                   FILE_MOVE, FILE_COPY, FILE_MKDIR, FILE_RMDIR, FILE_CD, FILE_PWD };
void files_command(struct limine_framebuffer *fb, enum file_action action, char *args);

#define FILE_SLOTS 64
enum file_kind { TEXT_FILE, EXEC_FILE, FOLDER };
struct file_item { char name[32]; enum file_kind kind; unsigned slot; };
unsigned files_cwd(void);
int files_directory(unsigned dir, struct file_item items[FILE_SLOTS], unsigned *parent);
void files_print_path(struct limine_framebuffer *fb, unsigned dir);
void files_preview(struct limine_framebuffer *fb, unsigned slot);

#define FILE_TEXT_MAX 65535
// A missing file opens as an empty buffer; disk changes happen only on save.
const char *files_text_load(const char *path, char text[FILE_TEXT_MAX + 1], size_t *size);
const char *files_text_save(const char *path, const char *text, size_t size);
