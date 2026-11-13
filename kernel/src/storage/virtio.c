// Transitional VirtIO PCI block transport, split queue, one synchronous request.
// OASIS VirtIO 1.2 sections 2.7, 4.1.4.10 and 5.2 (legacy interface).
#include "virtio.h"
#include "block.h"
#include "arch/x86_64/io.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include "sched/scheduler.h"

#define FEATURE_RO (1u << 5)
#define FEATURE_FLUSH (1u << 9)
#define NEXT 1u
#define WRITE 2u

struct descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct used_element { uint32_t id, length; };
struct used_ring { uint16_t flags, index; struct used_element elements[]; };
struct request { uint32_t type, reserved; uint64_t sector; };
_Static_assert(sizeof(struct descriptor) == 16, "VirtIO descriptor layout");
_Static_assert(sizeof(struct request) == 16, "VirtIO request layout");

static uint16_t port, queue_size, available_index, used_index;
static uint64_t capacity;
static bool online, busy, readonly;
static struct descriptor *descriptors;
static volatile uint16_t *available;
static volatile struct used_ring *used;
static unsigned char *control, *bounce;

static uint32_t pci_address(unsigned bus, unsigned device, unsigned fn, unsigned offset) {
    return 0x80000000u | (bus << 16) | (device << 11) | (fn << 8) | (offset & ~3u);
}
static uint32_t pci_read(unsigned bus, unsigned device, unsigned fn, unsigned offset) {
    outl(0xcf8, pci_address(bus, device, fn, offset));
    return inl(0xcfc);
}
static bool discover(void) {
    for (unsigned bus = 0; bus < 256; ++bus) {
        for (unsigned device = 0; device < 32; ++device) {
            if ((pci_read(bus, device, 0, 0) & 0xffff) == 0xffff) continue;
            unsigned functions = pci_read(bus, device, 0, 12) & 0x800000 ? 8 : 1;
            for (unsigned fn = 0; fn < functions; ++fn) {
                if (pci_read(bus, device, fn, 0) != 0x10011af4) continue;
                uint32_t bar = pci_read(bus, device, fn, 0x10);
                if (!(bar & 1) || !(bar & ~3u) || (bar & ~3u) > 0xffc0) continue;
                port = bar & ~3u;
                outl(0xcf8, pci_address(bus, device, fn, 4));
                uint16_t command = inw(0xcfc);
                outw(0xcfc, command | 5); // I/O decoding and DMA bus mastering.
                return true;
            }
        }
    }
    return false;
}

int virtio_disk_init(void) {
    if (!discover()) return 0;
    outb(port + 18, 0);
    unsigned tries = 10000;
    while (inb(port + 18) && --tries) asm volatile("pause");
    if (!tries) return -1;
    outb(port + 18, 1); // ACKNOWLEDGE
    outb(port + 18, 3); // DRIVER
    uint32_t offered = inl(port);
    readonly = (offered & FEATURE_RO) != 0;
    // Refuse writable devices without flush support rather than weakening save.
    if (!(offered & FEATURE_FLUSH) && !readonly) goto failed;
    outl(port + 4, offered & (FEATURE_FLUSH | FEATURE_RO));
    outw(port + 14, 0);
    queue_size = inw(port + 12);
    if (queue_size < 3 || queue_size > 256 || (queue_size & (queue_size - 1))) goto failed;
    size_t used_offset = (queue_size * 16 + 6 + queue_size * 2 + 4095) & ~(size_t)4095;
    size_t queue_pages = (used_offset + 6 + queue_size * 8 + 4095) / 4096;
    unsigned char *queue = pages_alloc(queue_pages, PAGE_DEVICE);
    control = pages_alloc(1 + DISK_BATCH_MAX * 512 / PAGE_SIZE, PAGE_DEVICE);
    if (!queue || !control || pages_physical(queue) >> 44) {
        if (queue) pages_free(queue, queue_pages);
        if (control) pages_free(control, 1 + DISK_BATCH_MAX * 512 / PAGE_SIZE);
        control = NULL;
        goto failed;
    }
    descriptors = (void *)queue;
    available = (void *)(queue + queue_size * 16);
    used = (void *)(queue + used_offset);
    available[0] = 1; // NO_INTERRUPT; completion is polled.
    bounce = control + PAGE_SIZE;
    // No MSI-X is enabled, so legacy device config begins at byte 20.
    uint32_t high, low;
    tries = 8;
    do {
        high = inl(port + 24); low = inl(port + 20);
    } while (high != inl(port + 24) && --tries);
    if (!tries) {
        pages_free(queue, queue_pages);
        pages_free(control, 1 + DISK_BATCH_MAX * 512 / PAGE_SIZE);
        goto failed;
    }
    capacity = ((uint64_t)high << 32) | low;
    asm volatile("mfence" ::: "memory");
    outl(port + 8, pages_physical(queue) >> 12);
    outb(port + 18, 7); // DRIVER_OK; legacy does not use FEATURES_OK.
    online = true;
    return 1;
failed:
    outb(port + 18, 128);
    return -1;
}

static bool request(uint32_t type, uint32_t sector, unsigned sectors, void *buffer) {
    bool data = type != 4;
    if (!online || busy || (type == 1 && readonly) ||
        (data && (!buffer || !sectors || sectors > DISK_BATCH_MAX ||
                  sector >= capacity || sectors > capacity - sector))) return false;
    busy = true;
    size_t bytes = sectors * 512;
    struct request *header = (void *)control;
    *header = (struct request){.type = type, .sector = type == 4 ? 0 : sector};
    volatile uint8_t *status = control + sizeof(*header);
    *status = 0xff;
    if (type == 1) memcpy(bounce, buffer, bytes);
    descriptors[0] = (struct descriptor){pages_physical(header), sizeof(*header), NEXT, data ? 1 : 2};
    descriptors[1] = (struct descriptor){pages_physical(bounce), bytes, NEXT | (type == 0 ? WRITE : 0), 2};
    descriptors[2] = (struct descriptor){pages_physical((const void *)status), 1, WRITE, 0};
    available[2 + available_index % queue_size] = 0;
    asm volatile("mfence" ::: "memory");
    available[1] = ++available_index;
    asm volatile("mfence" ::: "memory");
    outw(port + 16, 0);
    uint64_t start = scheduler_ticks();
    unsigned spins = 0;
    while (used->index == used_index) {
        if (++spins == 100000000 || scheduler_ticks() - start > 200) {
            // Stop DMA and keep its buffers pinned. Never reuse memory that a
            // failed device might still access, and never retry on another disk.
            online = false;
            outb(port + 18, 0);
            busy = false;
            return false;
        }
        asm volatile("pause" ::: "memory");
    }
    asm volatile("lfence" ::: "memory");
    struct used_element completion = used->elements[used_index % queue_size];
    uint8_t result = *status;
    size_t expected = type == 0 ? bytes + 1 : 1;
    bool valid = used->index == (uint16_t)(used_index + 1) && completion.id == 0 &&
                 result <= 2 && completion.length >= 1 && completion.length <= expected &&
                 (result != 0 || completion.length == expected);
    ++used_index;
    (void)inb(port + 19); // Acknowledge any latched ISR status.
    if (!valid) { online = false; outb(port + 18, 0); }
    bool ok = valid && result == 0;
    if (ok && type == 0) memcpy(buffer, bounce, bytes);
    busy = false;
    return ok;
}

bool virtio_disk_read(uint32_t sector, unsigned count, void *buffer) {
    return request(0, sector, count, buffer);
}
bool virtio_disk_write(uint32_t sector, unsigned count, const void *buffer) {
    return request(1, sector, count, (void *)buffer);
}
bool virtio_disk_flush(void) {
    return readonly || request(4, 0, 0, NULL);
}
