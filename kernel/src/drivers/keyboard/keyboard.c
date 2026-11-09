#include "keyboard.h"
#include "arch/x86_64/io.h"

#define KEYBOARD_DATA_PORT      0x60
#define KEYBOARD_STATUS_PORT    0x64

static char scancode_to_ascii[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0',

    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p',

    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l',

    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',

    [0x3b] = KEY_F1,
    [0x3c] = KEY_F2,
    [0x3d] = KEY_F3,
    [0x34] = '.', [0x0c] = '-',
    [0x39] = ' ',
    [0x1C] = '\n',
    [0x0E] = '\b',
};

// Single interrupt producer and one UI consumer on the boot CPU.
static char queue[128];
static volatile unsigned head, tail;

bool keyboard_pending(void) { return head != tail; }

void keyboard_interrupt(void) {
    if (!(inb(KEYBOARD_STATUS_PORT) & 1)) return;
    uint8_t code = inb(KEYBOARD_DATA_PORT);
    if (code >= 128 || !scancode_to_ascii[code]) return;
    unsigned next = (head + 1) % sizeof(queue);
    if (next != tail) {
        queue[head] = scancode_to_ascii[code];
        head = next;
    }
}

char keyboard_poll_char(void) {
    if (!keyboard_pending()) return 0;
    char key = queue[tail];
    tail = (tail + 1) % sizeof(queue);
    return key;
}
