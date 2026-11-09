#include "keyboard.h"
#include "arch/x86_64/io.h"

#define KEYBOARD_DATA_PORT      0x60
#define KEYBOARD_STATUS_PORT    0x64

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

// Single interrupt producer and one UI consumer on the boot CPU.
static char queue[128];
static volatile unsigned head, tail;

bool keyboard_pending(void) { return head != tail; }

void keyboard_interrupt(void) {
    if (!(inb(KEYBOARD_STATUS_PORT) & 1)) return;
    uint8_t code = inb(KEYBOARD_DATA_PORT);
    static bool extended;
    static unsigned shifts;
    static bool caps, caps_pressed;
    if (code == 0xe0) { extended = true; return; }
    char key = 0;
    if (extended) {
        extended = false;
        switch (code) {
        case 0x48: key = KEY_UP; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x4b: key = KEY_LEFT; break;
        case 0x4d: key = KEY_RIGHT; break;
        case 0x53: key = KEY_DELETE; break;
        case 0x49: key = KEY_PAGE_UP; break;
        case 0x51: key = KEY_PAGE_DOWN; break;
        case 0x1c: key = '\n'; break;
        case 0x35: key = '/'; break;
        default: return;
        }
    } else {
        unsigned scan = code & 0x7f;
        if (scan == 0x2a || scan == 0x36) {
            unsigned bit = scan == 0x2a ? 1 : 2;
            if (code & 0x80) shifts &= ~bit;
            else shifts |= bit;
            return;
        }
        if (scan == 0x3a) {
            if (!(code & 0x80) && !caps_pressed) caps = !caps;
            caps_pressed = !(code & 0x80);
            return;
        }
        if (code & 0x80) return;
        key = scancode_to_ascii[scan];
        if (key >= 'a' && key <= 'z') {
            if (caps != (shifts != 0)) key -= 'a' - 'A';
        } else if (shifts && shifted[scan]) key = shifted[scan];
    }
    if (!key) return;
    unsigned next = (head + 1) % sizeof(queue);
    if (next != tail) {
        queue[head] = key;
        head = next;
    }
}

char keyboard_poll_char(void) {
    if (!keyboard_pending()) return 0;
    char key = queue[tail];
    tail = (tail + 1) % sizeof(queue);
    return key;
}
