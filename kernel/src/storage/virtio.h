#pragma once
#include <stdbool.h>
#include <stdint.h>

// 0 absent, 1 initialized, -1 present but unusable. Transitional PCI only.
int virtio_disk_init(void);
bool virtio_disk_read(uint32_t sector, unsigned count, void *buffer);
bool virtio_disk_write(uint32_t sector, unsigned count, const void *buffer);
bool virtio_disk_flush(void);
