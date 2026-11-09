#pragma once
#include <stdint.h>

#define KEY_F3 0x13
#define KEY_F1 0x11
#define KEY_F2 0x12

#include <stdbool.h>
bool keyboard_pending(void);
void keyboard_interrupt(void);
char keyboard_poll_char(void);
