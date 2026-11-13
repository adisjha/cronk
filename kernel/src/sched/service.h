#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "arch/clock.h"
struct kernel_service {
    unsigned id, caller;
    void (*work)(void *);
    void *context;
};
// Synchronous single-client calls: caller sleeps while the service owns the work.
unsigned service_start(struct kernel_service *service, const char *name, uint64_t owned_bytes);
void service_call(struct kernel_service *service, void (*work)(void *), void *context);
void system_services_start(uint64_t input_bytes, uint64_t storage_bytes);
bool system_clock_read(struct clock_sample *sample);
