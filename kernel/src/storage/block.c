#include "block.h"
#include "sched/service.h"
#if !defined(__aarch64__)
#include "ata.h"
#endif
#include "virtio.h"

static int virtio;
static struct disk_counters counters;
void disk_init(void) {
    virtio = virtio_disk_init();
#if defined(__aarch64__)
    if (!virtio) virtio = -1;
#endif
}
const char *disk_backend(void) {
    return virtio == 1 ?
#if defined(__aarch64__)
    "VirtIO MMIO (DMA)"
#else
    "VirtIO PCI (DMA)"
#endif
    : virtio < 0 ? "unavailable VirtIO" : "ATA PIO";
}
struct disk_counters disk_counters(void) { return counters; }
static bool valid(uint32_t sector, unsigned count, const void *buffer) {
    return buffer && count && count <= DISK_BATCH_MAX && sector <= UINT32_MAX - count;
}
static bool read_direct(uint32_t sector, unsigned count, void *buffer) {
    if (!valid(sector, count, buffer) || virtio < 0) return false;
    ++counters.reads;
    #if defined(__aarch64__)
    return virtio_disk_read(sector, count, buffer);
#else
    return virtio ? virtio_disk_read(sector, count, buffer) : ata_read_many(sector, count, buffer);
#endif
}
static bool write_direct(uint32_t sector, unsigned count, const void *buffer) {
    if (!valid(sector, count, buffer) || virtio < 0) return false;
    ++counters.writes;
    #if defined(__aarch64__)
    return virtio_disk_write(sector, count, buffer);
#else
    return virtio ? virtio_disk_write(sector, count, buffer) : ata_write_many(sector, count, buffer);
#endif
}
static bool flush_direct(void) {
    if (virtio < 0) return false;
    ++counters.flushes;
    #if defined(__aarch64__)
    return virtio_disk_flush();
#else
    return virtio ? virtio_disk_flush() : ata_flush();
#endif
}

static struct kernel_service storage_service;
struct block_request { unsigned operation; uint32_t sector; unsigned count; void *buffer; bool result; };
static void transfer(void *context) {
    struct block_request *r = context;
    r->result = r->operation == 0 ? read_direct(r->sector, r->count, r->buffer) :
                r->operation == 1 ? write_direct(r->sector, r->count, r->buffer) : flush_direct();
}
void disk_service_start(uint64_t owned_bytes) { service_start(&storage_service, "storage", owned_bytes); }
bool disk_read_many(uint32_t sector, unsigned count, void *buffer) {
    struct block_request request = {0, sector, count, buffer, false};
    service_call(&storage_service, transfer, &request);
    return request.result;
}
bool disk_write_many(uint32_t sector, unsigned count, const void *buffer) {
    struct block_request request = {1, sector, count, (void *)buffer, false};
    service_call(&storage_service, transfer, &request);
    return request.result;
}
bool disk_flush(void) {
    struct block_request request = {.operation = 2};
    service_call(&storage_service, transfer, &request);
    return request.result;
}
bool disk_read(uint32_t sector, void *buffer) { return disk_read_many(sector, 1, buffer); }
bool disk_write(uint32_t sector, const void *buffer) {
    return disk_write_many(sector, 1, buffer) && disk_flush();
}
