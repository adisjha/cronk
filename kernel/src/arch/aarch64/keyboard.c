#include "drivers/keyboard/keyboard.h"
#include "platform.h"
#include "arch/cpu.h"
static const char scancode_to_ascii[128] = {
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

    [0x3b] = KEY_APPS,
    [0x3c] = KEY_F2,


    [0x01] = KEY_ESC,
    [0x35] = '/', [0x0d] = '=', [0x37] = '*', [0x4e] = '+',
    [0x1a] = '[', [0x1b] = ']', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2b] = '\\', [0x33] = ',',
    [0x34] = '.', [0x0c] = '-',
    [0x39] = ' ',
    [0x1C] = '\n',
    [0x0E] = '\b',
};

static const char shifted[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0a]='(', [0x0b]=')',
    [0x0c]='_', [0x0d]='+', [0x1a]='{', [0x1b]='}', [0x27]=':',
    [0x28]='"', [0x29]='~', [0x2b]='|', [0x33]='<', [0x34]='>', [0x35]='?'
};


static char queue[128];
static volatile unsigned head, tail;
static unsigned shifts;
static bool caps, caps_pressed;
static void enqueue(char key) {
    unsigned next = (head + 1) % sizeof(queue);
    if (key && next != tail) { queue[head] = key; head = next; }
}
void arm_keyboard_event(unsigned code, unsigned value) {
    if (code == 42 || code == 54) {
        unsigned bit = code == 42 ? 1 : 2;
        if (!value) shifts &= ~bit; else shifts |= bit;
        return;
    }
    if (code == 58) {
        if (value == 1 && !caps_pressed) caps = !caps;
        caps_pressed = value != 0; return;
    }
    if (!value) return;
    char key;
    switch (code) {
    case 103: key = KEY_UP; break;
    case 108: key = KEY_DOWN; break;
    case 105: key = KEY_LEFT; break;
    case 106: key = KEY_RIGHT; break;
    case 104: key = KEY_PAGE_UP; break;
    case 109: key = KEY_PAGE_DOWN; break;
    case 111: key = KEY_DELETE; break;
    default:
        if (code >= 128) return;
        key = scancode_to_ascii[code];
        if (key >= 'a' && key <= 'z') {
            if (caps != (shifts != 0)) key -= 'a' - 'A';
        } else if (shifts && shifted[code]) key = shifted[code];
    }
    enqueue(key);
}
bool keyboard_pending(void) { return head != tail; }
void keyboard_interrupt(void) {
    arm_input_poll();
    int c;
    // Serial is also a shell console. GUI keys come through VirtIO input.
    while ((c = arm_serial_getc()) >= 0) enqueue(c == '\r' ? '\n' : c == 127 ? '\b' : c);
}
char keyboard_poll_char(void) {
    if (!keyboard_pending()) return 0;
    char key = queue[tail]; tail = (tail + 1) % sizeof(queue); return key;
}
