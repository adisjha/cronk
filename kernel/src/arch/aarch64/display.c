// QEMU ramfb configuration via fw_cfg DMA, independent of firmware GOP modes.
// https://www.qemu.org/docs/master/specs/fw_cfg.html
#include "arch/cpu.h"
#include "platform.h"
#include "mm/pages.h"
#include "lib/memory.h"
#include <limine.h>
#include <stdbool.h>

struct ramfb_config {
    uint64_t address;
    uint32_t fourcc, flags, width, height, stride;
} __attribute__((packed));
struct dma_request { uint32_t control, length; uint64_t address; };
_Static_assert(sizeof(struct ramfb_config) == 28, "ramfb wire layout");
_Static_assert(sizeof(struct dma_request) == 16, "fw_cfg DMA layout");
static struct limine_framebuffer display;

static void select(volatile uint8_t *fw, uint16_t item) {
    *(volatile uint16_t *)(fw + 8) = __builtin_bswap16(item);
}
static uint32_t read_be(volatile uint8_t *fw, unsigned bytes) {
    uint32_t result = 0;
    while (bytes--) result = (result << 8) | *fw;
    return result;
}
static bool resolution(const char *text, unsigned *width, unsigned *height) {
    unsigned values[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            if (values[i] > 819) return false;
            values[i] = values[i] * 10 + *text++ - '0';
        }
        if (!i && *text++ != 'x') return false;
    }
    if (*text || values[0] < 640 || values[0] > 7680 || values[1] < 360 || values[1] > 4320)
        return false;
    *width = values[0]; *height = values[1]; return true;
}
struct limine_framebuffer *arch_display_init(struct limine_framebuffer *fallback) {
    volatile uint8_t *fw = arm_mmio(0x09020000);
    select(fw, 0);
    if (read_be(fw, 4) != 0x51454d55) return fallback; // "QEMU"
    select(fw, 1);
    unsigned features = *fw; // Feature bitmap is little-endian, DMA is bit 1.
    if (!(features & 2)) return fallback;
    select(fw, 0x19);
    unsigned count = read_be(fw, 4);
    if (count > 4096) return fallback;
    uint16_t ramfb = 0, setting = 0;
    uint32_t setting_size = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t size = read_be(fw, 4);
        uint16_t item = read_be(fw, 2);
        (void)read_be(fw, 2);
        char name[57];
        for (unsigned j = 0; j < 56; ++j) name[j] = *fw;
        name[56] = 0;
        if (!memcmp(name, "etc/ramfb", 10) && size == sizeof(struct ramfb_config)) ramfb = item;
        if (!memcmp(name, "opt/org.cronk/resolution", sizeof("opt/org.cronk/resolution")) && size && size < 32) {
            setting = item; setting_size = size;
        }
    }
    if (!ramfb) return fallback;
    unsigned width = 3840, height = 2160;
    if (setting) {
        char text[32] = {0};
        select(fw, setting);
        for (unsigned i = 0; i < setting_size; ++i) text[i] = *fw;
        if (!resolution(text, &width, &height)) {
            const char *error = "Invalid ARM_RESOLUTION; using 3840x2160\n";
            while (*error) arch_serial_putc(*error++);
        }
    }
    size_t size = (size_t)width * height * 4;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *pixels = pages_alloc(pages, PAGE_DISPLAY);
    unsigned char *command = pages_alloc(1, PAGE_DEVICE);
    if (!pixels || !command) {
        if (pixels) pages_free(pixels, pages);
        if (command) pages_free(command, 1);
        return fallback;
    }
    volatile struct dma_request *request = (void *)command;
    struct ramfb_config *config = (void *)(command + 64);
    *config = (struct ramfb_config){
        .address = __builtin_bswap64(pages_physical(pixels)),
        .fourcc = __builtin_bswap32(0x34325258), // DRM_FORMAT_XRGB8888
        .width = __builtin_bswap32(width), .height = __builtin_bswap32(height),
        .stride = __builtin_bswap32(width * 4),
    };
    request->length = __builtin_bswap32(sizeof(*config));
    request->address = __builtin_bswap64(pages_physical(config));
    request->control = __builtin_bswap32(((uint32_t)ramfb << 16) | 8 | 16);
    uint64_t physical = pages_physical(command);
    asm volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(fw + 16) = __builtin_bswap32(physical >> 32);
    *(volatile uint32_t *)(fw + 20) = __builtin_bswap32(physical);
    uint64_t start = arch_timestamp();
    while (request->control && !(request->control & __builtin_bswap32(1))) {
        if (arch_timestamp() - start > arch_counter_frequency()) return fallback;
        arch_pause();
    }
    asm volatile("dsb sy" ::: "memory");
    // DMA buffers stay pinned on failure; the host may still reference pixels.
    if (request->control) return fallback;
    pages_free(command, 1);
    display = *fallback;
    display.address = pixels;
    display.width = width; display.height = height; display.pitch = width * 4;
    display.bpp = 32;
    display.red_mask_size = display.green_mask_size = display.blue_mask_size = 8;
    display.red_mask_shift = 16; display.green_mask_shift = 8; display.blue_mask_shift = 0;
    display.mode_count = 0; display.modes = NULL;
    return &display;
}
