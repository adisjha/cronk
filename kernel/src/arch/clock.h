#pragma once
#include <stdbool.h>
#include <stdint.h>

struct clock_sample {
    unsigned hour, minute, second, millisecond;
};

void clock_start(void);
bool clock_read(struct clock_sample *sample);
