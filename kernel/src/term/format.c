#include "format.h"
#include <stddef.h>

void format_uint(char text[32], uint64_t value) {
    char reverse[20];
    size_t n = 0, i = 0;
    do { reverse[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) text[i++] = reverse[--n];
    text[i] = 0;
}

void format_mb(char text[32], uint64_t bytes) {
    uint64_t hundredths = bytes / 10000 + (bytes % 10000 >= 5000);
    format_uint(text, hundredths / 100);
    size_t n = 0;
    while (text[n]) ++n;
    text[n++] = '.';
    text[n++] = '0' + hundredths / 10 % 10;
    text[n++] = '0' + hundredths % 10;
    text[n++] = ' '; text[n++] = 'M'; text[n++] = 'B'; text[n] = 0;
}
