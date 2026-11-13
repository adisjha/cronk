#include "service.h"
#include "scheduler.h"
#include "arch/cpu.h"
#include "drivers/keyboard/keyboard.h"
#include "mm/pages.h"
#include "storage/block.h"

static struct clock_sample clock_value;
static bool clock_valid;
static void service_main(void *context) {
    struct kernel_service *service = context;
    for (;;) {
        arch_irq_disable();
        if (!service->work) scheduler_block();
        arch_irq_enable();
        if (!service->work) continue;
        service->work(service->context);
        arch_irq_disable();
        service->work = NULL;
        scheduler_wake(service->caller);
        arch_irq_enable();
    }
}
unsigned service_start(struct kernel_service *service, const char *name, uint64_t owned_bytes) {
    service->id = scheduler_spawn_service(name, service_main, service, owned_bytes);
    return service->id;
}
void service_call(struct kernel_service *service, void (*work)(void *), void *context) {
    if (!service->id || service->id == scheduler_current_id()) { work(context); return; }
    // UI and shell serialize service requests; never call this from an IRQ.
    arch_irq_disable();
    service->caller = scheduler_current_id();
    service->context = context;
    service->work = work;
    scheduler_wake(service->id);
    while (service->work) scheduler_block();
    arch_irq_enable();
}
static void input_main(void *unused) {
    (void)unused;
    for (;;) {
#if defined(__aarch64__)
        keyboard_interrupt();
#endif
        scheduler_notify_input();
        scheduler_sleep(1);
    }
}
static void clock_main(void *unused) {
    (void)unused;
    for (;;) {
        arch_irq_disable();
        clock_valid = clock_read(&clock_value);
        arch_irq_enable();
        scheduler_sleep(1);
    }
}
static void memory_main(void *unused) {
    (void)unused;
    for (;;) { scheduler_reap(); scheduler_wait_reap(); }
}
bool system_clock_read(struct clock_sample *sample) {
    uint64_t flags = arch_irq_save();
    bool valid = clock_valid;
    *sample = clock_value;
    arch_irq_restore(flags);
    return valid;
}
void system_services_start(uint64_t input_bytes, uint64_t storage_bytes) {
    scheduler_spawn_service("input", input_main, NULL, input_bytes);
    scheduler_spawn_service("clock", clock_main, NULL, 0);
    scheduler_spawn_service("memory manager", memory_main, NULL, pages_breakdown().metadata * PAGE_SIZE);
    disk_service_start(storage_bytes);
}
