#include <limine.h>
#include "arch/cpu.h"
#include "sched/scheduler.h"
#include "arch/user.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_request = {.id = LIMINE_MP_REQUEST_ID};

// Published before goto_address; secondary CPUs only write their own record.
static struct {
    struct limine_mp_info *info;
    unsigned online;
    uintptr_t observed_sp;
} secondary[SCHEDULER_MAX_CPUS];
_Static_assert(offsetof(struct limine_mp_info, extra_argument) == 32,
               "secondary entry assembly requires extra_argument at offset 32");
extern void arm_secondary_entry(struct limine_mp_info *);
extern char arm_vectors[];
static unsigned released, expected_cpus = 1;
static bool reclaim_safe = true;

static void serial_number(unsigned n) {
    if (n >= 10) serial_number(n / 10);
    arch_serial_putc('0' + n % 10);
}
static void serial_text(const char *s) { while (*s) arch_serial_putc(*s++); }

void arm_secondary_main(struct limine_mp_info *info) {
    arch_irq_disable();
    unsigned index;
    for (index = 1; index < SCHEDULER_MAX_CPUS; ++index)
        if (secondary[index].info == info) break;
    if (index == SCHEDULER_MAX_CPUS) for (;;) arch_idle();
    arch_bind_cpu(index);
    arch_use_kernel_tables();
    asm volatile("msr vbar_el1, %0; msr cntv_ctl_el0, xzr; isb"
                 :: "r"(arm_vectors) : "memory");
    uintptr_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    secondary[index].observed_sp = sp;
    __atomic_store_n(&secondary[index].online, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&released, __ATOMIC_ACQUIRE)) arch_pause();
    user_cpu_init();
    arch_cpu_scheduler_init();
    scheduler_secondary_start();
}

void arch_start_secondary_cpus(void) {
    struct limine_mp_response *response = mp_request.response;
    unsigned count = 1;
    arch_use_kernel_tables();
    if (response) {
        if (response->cpu_count > SCHEDULER_MAX_CPUS) reclaim_safe = false;
        for (uint64_t i = 0; i < response->cpu_count && count < SCHEDULER_MAX_CPUS; ++i) {
            struct limine_mp_info *info = response->cpus[i];
            if (info->mpidr == response->bsp_mpidr) continue;
            secondary[count].info = info;
            info->extra_argument = scheduler_prepare_cpu(count);
            __atomic_store_n(&info->goto_address, arm_secondary_entry, __ATOMIC_RELEASE);
            uint64_t deadline = arch_timestamp() + arch_counter_frequency() * 2;
            while (!__atomic_load_n(&secondary[count].online, __ATOMIC_ACQUIRE) && arch_timestamp() < deadline)
                arch_pause();
            serial_text("CPU "); serial_number(count);
            if (__atomic_load_n(&secondary[count].online, __ATOMIC_ACQUIRE)) {
                // Verify the entry trampoline actually selected this CPU's stack.
                uintptr_t top = info->extra_argument, sp = secondary[count].observed_sp;
                serial_text(sp <= top && sp > top - 8192 ? " online (private stack)\n" : " STACK ERROR\n");
            } else { reclaim_safe = false; serial_text(" startup timeout\n"); }
            ++count;
        }
    }
    expected_cpus = count;
    serial_text("SMP: "); serial_number(count);
    serial_text(" CPU slots; starting parallel scheduler\n");
}

void arch_release_secondary_cpus(void) {
    __atomic_store_n(&released, 1, __ATOMIC_RELEASE);
    uint64_t deadline=arch_timestamp()+2*arch_counter_frequency();
    struct scheduler_sample sample;
    do {
        scheduler_sample(&sample);
        if(sample.cpu_count==expected_cpus) return;
        arch_pause();
    } while(arch_timestamp()<deadline);
    reclaim_safe=false;
}
bool arch_boot_reclaim_safe(void) { return reclaim_safe; }
