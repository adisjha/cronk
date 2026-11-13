#pragma once
#include "files.h"
bool extentfs_active(void);
void extentfs_install(const char *, unsigned, const void *, size_t);
void extentfs_command(struct limine_framebuffer *, enum file_action, char *);
int extentfs_directory(unsigned, struct file_item *, unsigned *);
unsigned extentfs_cwd(void);
void extentfs_print_path(struct limine_framebuffer *, unsigned);
void extentfs_preview(struct limine_framebuffer *, unsigned);
const char *extentfs_load(const char *, char *, size_t *);
const char *extentfs_save(const char *, const char *, size_t);
bool extentfs_scratch(unsigned *sector, unsigned *sectors);
