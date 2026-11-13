#pragma once
#include <stdint.h>
#include <stdbool.h>
#define USER_CODE 0x400000UL
#define USER_STACK (USER_CODE + 2 * 4096)
#define USER_TOP (USER_STACK + 4 * 4096)
#define USER_WRITE (USER_CODE + 4064)
#define USER_EXIT (USER_CODE + 4080)
struct user_space {
    uintptr_t root, resume;
    void *context;
    void (*write)(void *, const char *);
    unsigned char *code, *stack;
};
void user_cpu_init(void);
void user_space_init(struct user_space *, unsigned);
void user_switch(struct user_space *);
unsigned user_execute(struct user_space *);
