#include "arch/cache.h"
#include <stdint.h>
extern uint64_t arm_cache_clidr(void), arm_cache_ccsidr(void), arm_cache_csselr(void), arm_cache_mmfr2(void);
extern uint64_t arm_cache_select(uint64_t selector);
extern char arm_cache_select_instruction[];

bool arm_cache_probe_fault(uint64_t *frame, uint64_t esr) {
    if ((esr >> 26) != 0 && (esr >> 26) != 0x18) return false;
    uintptr_t reads[] = {(uintptr_t)arm_cache_clidr, (uintptr_t)arm_cache_ccsidr,
                         (uintptr_t)arm_cache_csselr, (uintptr_t)arm_cache_mmfr2};
    for (unsigned i = 0; i < 4; ++i) if (frame[32] == reads[i]) {
        frame[0] = UINT64_MAX;
        frame[32] += 4;
        return true;
    }
    if (frame[32] == (uintptr_t)arm_cache_select_instruction) {
        frame[0] = 0;
        frame[32] += 4;
        return true;
    }
    return false;
}

unsigned arch_cache_info(struct cache_info info[CACHE_INFO_MAX]) {
    uint64_t clidr = arm_cache_clidr(), original = arm_cache_csselr(), mmfr2 = arm_cache_mmfr2();
    if (clidr == UINT64_MAX || original == UINT64_MAX || mmfr2 == UINT64_MAX) return 0;
    bool extended = ((mmfr2 >> 20) & 15) == 1; // FEAT_CCIDX
    unsigned count = 0;
    for (unsigned level = 0; level < 7 && count < CACHE_INFO_MAX; ++level) {
        unsigned type = (clidr >> (3 * level)) & 7;
        if (!type) break;
        if (type > 4) break;
        unsigned first = type == 1 ? 1 : 0;
        unsigned last = type == 3 ? 1 : first;
        for (unsigned instruction = first; instruction <= last && count < CACHE_INFO_MAX; ++instruction) {
            if (!arm_cache_select((level << 1) | instruction)) goto done;
            uint64_t ccsidr = arm_cache_ccsidr();
            if (ccsidr == UINT64_MAX) goto done;
            uint64_t sets = extended ? ((ccsidr >> 32) & 0xffffff) + 1 : ((ccsidr >> 13) & 0x7fff) + 1;
            unsigned ways = extended ? ((ccsidr >> 3) & 0x1fffff) + 1 : ((ccsidr >> 3) & 0x3ff) + 1;
            unsigned line = 1u << ((ccsidr & 7) + 4);
            uint64_t bytes = sets * ways * line;
            if (!bytes || bytes > (1ULL << 40)) continue;
            info[count++] = (struct cache_info){level + 1, type == 4 ? 3 : instruction ? 2 : 1, line, ways, bytes};
        }
    }
done:
    (void)arm_cache_select(original);
    return count;
}
