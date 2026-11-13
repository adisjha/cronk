#pragma once
#include <stdint.h>
#include <stddef.h>
void *arm_mmio(uintptr_t physical);
void arm_input_poll(void);
int arm_serial_getc(void);
void arm_keyboard_event(unsigned code, unsigned value);
uintptr_t arm_sync(uint64_t *frame);
uintptr_t arm_irq(uint64_t *frame);
