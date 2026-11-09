#pragma once
#include <stdint.h>

#define KEY_UP 1
#define KEY_DOWN 2
#define KEY_PAGE_UP 3
#define KEY_PAGE_DOWN 4
#define KEY_LEFT 5
#define KEY_RIGHT 6
#define KEY_DELETE 7
#define KEY_ESC 27
#define KEY_APPS 20
#define KEY_HELP 21
#define KEY_F3 0x13
#define KEY_F1 0x11
#define KEY_F2 0x12

#include <stdbool.h>
bool keyboard_pending(void);
void keyboard_interrupt(void);
char keyboard_poll_char(void);
