// QEMU virt: modern VirtIO-MMIO transport (VirtIO 1.2 section 4.2).
#include "storage/virtio.h"
#include "storage/block.h"
#include "platform.h"
#include "arch/cpu.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include "sched/scheduler.h"

struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct element { uint32_t id, length; };
struct used_ring { uint16_t flags, index; struct element elements[]; };
struct virtqueue {
    volatile uint32_t *registers;
    struct descriptor *desc;
    volatile uint16_t *avail;
    volatile struct used_ring *used;
    uint16_t size, posted, consumed;
};
struct request { uint32_t type, reserved; uint64_t sector; };
struct input_event { uint16_t type, code; uint32_t value; };
static struct virtqueue disk, input, input_status;
static unsigned char *control, *bounce;
static struct input_event *events;
static bool ready, readonly, busy;
static uint64_t capacity;

static void barrier(void) { asm volatile("dsb sy" ::: "memory"); }
static void address(volatile uint32_t *reg, unsigned offset, uint64_t physical) {
    reg[offset / 4] = physical; reg[offset / 4 + 1] = physical >> 32;
}
static volatile uint32_t *find(unsigned id) {
    // MMIO transport slots on the explicitly supported QEMU virt board.
    for (unsigned i = 0; i < 32; ++i) {
        volatile uint32_t *reg = arm_mmio(0x0a000000 + i * 0x200);
        if (reg[0] == 0x74726976 && reg[1] == 2 && reg[2] == id) return reg;
    }
    return NULL;
}
static bool negotiate(volatile uint32_t *reg, uint32_t low) {
    reg[0x70 / 4] = 0; barrier();
    for (unsigned i = 0; reg[0x70 / 4]; ++i) if (i == 10000) return false;
    reg[0x70 / 4] = 3;
    reg[0x14 / 4] = 1;
    if (!(reg[0x10 / 4] & 1)) return false; // VERSION_1 is mandatory.
    reg[0x24 / 4] = 1; reg[0x20 / 4] = 1;
    reg[0x24 / 4] = 0; reg[0x20 / 4] = low;
    reg[0x70 / 4] = 11; barrier();
    return (reg[0x70 / 4] & 8) != 0;
}
static bool queue_init(struct virtqueue *q, volatile uint32_t *reg, unsigned number) {
    reg[0x30 / 4] = number;
    unsigned maximum = reg[0x34 / 4];
    if (maximum < 4 || reg[0x44 / 4]) return false;
    unsigned size = 64;
    while (size > maximum) size /= 2;
    unsigned char *page = pages_alloc(1, PAGE_DEVICE);
    if (!page) return false;
    *q = (struct virtqueue){.registers = reg, .desc = (void *)page, .size = size};
    q->avail = (void *)(page + 16 * size);
    q->used = (void *)(page + ((16 * size + 6 + 2 * size + 3) & ~3u));
    q->avail[0] = 1; // NO_INTERRUPT; the timer polls input and disk polls completion.
    reg[0x38 / 4] = size;
    address(reg, 0x80, pages_physical(q->desc));
    address(reg, 0x90, pages_physical((void *)q->avail));
    address(reg, 0xa0, pages_physical((void *)q->used));
    barrier(); reg[0x44 / 4] = 1;
    return true;
}
static void publish(struct virtqueue *q, unsigned id) {
    q->avail[2 + q->posted % q->size] = id;
    barrier(); q->avail[1] = ++q->posted; barrier();
}
static void notify(struct virtqueue *q, unsigned number) { q->registers[0x50 / 4] = number; }
static void acknowledge(volatile uint32_t *reg) {
    uint32_t status = reg[0x60 / 4];
    if (status) reg[0x64 / 4] = status;
}

int virtio_disk_init(void) {
    volatile uint32_t *reg = find(2);
    if (!reg) return 0;
    reg[0x14 / 4] = 0;
    uint32_t features = reg[0x10 / 4];
    readonly = (features & (1u << 5)) != 0;
    if ((!readonly && !(features & (1u << 9))) ||
        !negotiate(reg, features & ((1u << 5) | (1u << 9)))) return -1;
    if (!queue_init(&disk, reg, 0)) return -1;
    control = pages_alloc(9, PAGE_DEVICE);
    if (!control) { reg[0x70 / 4] = 128; return -1; }
    bounce = control + PAGE_SIZE;
    unsigned generation;
    for (unsigned tries = 0; ; ++tries) {
        generation = reg[0xfc / 4];
        uint32_t low = reg[0x100 / 4], high = reg[0x104 / 4];
        capacity = ((uint64_t)high << 32) | low;
        if (reg[0xfc / 4] == generation) break;
        if (tries == 8) { reg[0x70 / 4] = 128; return -1; }
    }
    reg[0x70 / 4] = 15; barrier(); ready = true;
    return 1;
}
static bool disk_request(unsigned type, uint32_t sector, unsigned count, void *buffer) {
    if (!ready || busy || (type == 1 && readonly) ||
        (type != 4 && (!buffer || !count || count > DISK_BATCH_MAX ||
                      sector >= capacity || count > capacity - sector))) return false;
    busy = true;
    unsigned bytes = count * 512;
    *(struct request *)control = (struct request){type, 0, type == 4 ? 0 : sector};
    volatile uint8_t *status = control + 16;
    *status = 0xff;
    if (type == 1) memcpy(bounce, buffer, bytes);
    disk.desc[0] = (struct descriptor){pages_physical(control), 16, 1, type == 4 ? 2 : 1};
    disk.desc[1] = (struct descriptor){pages_physical(bounce), bytes, type == 0 ? 3 : 1, 2};
    disk.desc[2] = (struct descriptor){pages_physical((const void *)status), 1, 2, 0};
    publish(&disk, 0); notify(&disk, 0);
    uint64_t start = arch_timestamp(), limit = arch_counter_frequency() * 2;
    while (disk.used->index == disk.consumed) {
        if (arch_timestamp() - start > limit) {
            ready = false; disk.registers[0x70 / 4] = 0; barrier();
            busy = false; return false; // Keep all DMA memory pinned after timeout.
        }
        arch_pause();
    }
    barrier();
    struct element used = disk.used->elements[disk.consumed % disk.size];
    unsigned result = *status, expected = type == 0 ? bytes + 1 : 1;
    bool valid = disk.used->index == (uint16_t)(disk.consumed + 1) && used.id == 0 &&
                 result <= 2 && used.length >= 1 && used.length <= expected &&
                 (result != 0 || used.length == expected);
    ++disk.consumed; acknowledge(disk.registers);
    if (!valid) { ready = false; disk.registers[0x70 / 4] = 0; barrier(); }
    bool ok = valid && !result;
    if (ok && type == 0) memcpy(buffer, bounce, bytes);
    busy = false; return ok;
}
bool virtio_disk_read(uint32_t sector, unsigned count, void *buffer) { return disk_request(0, sector, count, buffer); }
bool virtio_disk_write(uint32_t sector, unsigned count, const void *buffer) { return disk_request(1, sector, count, (void *)buffer); }
bool virtio_disk_flush(void) { return readonly || disk_request(4, 0, 0, NULL); }

void arm_input_init(void) {
    volatile uint32_t *reg = find(18);
    if (!reg || !negotiate(reg, 0)) return;
    if (!queue_init(&input, reg, 0) || !queue_init(&input_status, reg, 1)) {
        reg[0x70 / 4] = 128; return;
    }
    events = pages_alloc(1, PAGE_DEVICE);
    if (!events) { reg[0x70 / 4] = 128; return; }
    for (unsigned i = 0; i < input.size; ++i) {
        input.desc[i] = (struct descriptor){pages_physical(&events[i]), sizeof(*events), 2, 0};
        publish(&input, i);
    }
    reg[0x70 / 4] = 15; barrier(); notify(&input, 0);
}
void arm_input_poll(void) {
    if (!events) return;
    unsigned budget = input.size;
    while (input.used->index != input.consumed && budget--) {
        barrier();
        struct element completion = input.used->elements[input.consumed % input.size];
        if (completion.id >= input.size || completion.length != sizeof(*events)) {
            input.registers[0x70 / 4] = 0; events = NULL; return;
        }
        struct input_event event = events[completion.id];
        ++input.consumed;
        if (event.type == 1) arm_keyboard_event(event.code, event.value);
        publish(&input, completion.id);
    }
    notify(&input, 0); acknowledge(input.registers);
}
