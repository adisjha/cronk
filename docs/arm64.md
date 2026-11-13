# Cronk ARM64 on Apple Silicon

Run from the repository root:

```sh
gmake run-arm64
```

On an ARM64 Mac this selects `qemu-system-aarch64 -accel hvf -cpu host`,
using Apple's hardware virtualization instead of translating x86 instructions.
This is a QEMU `virt` guest, not a bare-metal Apple hardware port. It uses one vCPU.
The existing x86 build and data disk remain available through the original targets.

Inside Cronk:

```text
bench
run hello-arm64.bin
runbg counter-arm64.bin
programs
edit notes.txt
```

The ARM demo programs use distinct names and a distinct executable header;
x86 machine-code programs cannot run on ARM. C shell, editor, file management,
calculator, clock, memory viewer, program manager, allocator, and benchmarks are
shared between builds. Foreground programs still share the shell task; foreground
cancellation is a separate, outstanding limitation.

## Display and fullscreen

```sh
gmake run-arm64-fullscreen
```

The ARM kernel configures QEMU's RAM framebuffer directly at **3840×2160**,
with UI scale **2**, instead of stretching the firmware's 800×600 mode. In the
shell, `scale 1`, `scale 2`, `scale 3`, and `scale 4` all fit at the default
resolution. Changing scale affects UI size; the framebuffer stays at 4K.

Override the framebuffer size at launch if needed:

```sh
gmake run-arm64-fullscreen ARM_RESOLUTION=5120x2880
```

The requested `WIDTHxHEIGHT` is passed through QEMU `fw_cfg`. Supported bounds
are 640–7680 pixels wide and 360–4320 high. Invalid requests use the 4K default;
if allocation or device configuration cannot succeed, the firmware framebuffer
is retained. 4K needs about **31.6 MiB** of guest RAM for display pixels; it is
tracked as device memory, not a program allocation. A new launch is required to
change resolution or load this fix; an already running VM keeps its old kernel.

The mode is configured through the standard [QEMU fw_cfg DMA interface](https://www.qemu.org/docs/master/specs/fw_cfg.html),
so it does not depend on firmware offering the requested GOP mode.

## Build and run options

```sh
gmake arm64                     # Build only
gmake run-arm64-fullscreen       # Cocoa fullscreen on macOS
gmake run-arm64 ARM_ACCEL=tcg    # Software emulation fallback, not native performance
```

Build dependencies: Clang, LLD, LLVM objcopy, GNU make, Python 3, and mtools.
On macOS, `brew install llvm lld qemu mtools` supplies the additional tools.
The C compiler targets `aarch64-none-elf`; this is an ELF kernel, not a macOS executable.
`ARM_CC`, `ARM_LD`, and `ARM_OBJCOPY` can override tool paths.

The first build fetches **Limine 12.9.6's ARM64 EFI loader**, verifies the archive's
pinned SHA-256, and caches only `BOOTAA64.EFI` in `build/limine-arm64/`.
The bundled older loader rejected the QEMU RAM framebuffer's memory layout;
the ARM loader is therefore versioned separately. No x86 bootloader is replaced.
Bootloader sources: [Limine releases](https://github.com/Limine-Bootloader/Limine/releases/tag/v12.9.6),
[protocol](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md).

`ARM_FIRMWARE` selects an AArch64 EDK2 firmware image. The default searches
Homebrew and common Linux package paths. Firmware and boot images are read-only
in QEMU. The generated FAT boot disk is `cronk-arm64-boot.img`; rebuilds replace
it atomically, without modifying an image already open by another VM.

A new `cronk-arm64-data.img` is created only if absent. Reboots and rebuilds
preserve it. The default does not open or modify `cronk-data.img`. The filesystem
format is shared; advanced users can select another compatible disk with
`ARM_DATA_DISK=...`, with that disk closed in any other VM. Run targets back up and atomically upgrade legacy data disks to CRONKFS2, with
contiguous 4 KiB extents and files up to 65,535 bytes. See [storage](storage.md).

## Hardware implemented

- Limine protocol revision 6, EL1 entry, 4 KiB pages, and a high-half kernel.
  An assembly entry selects the kernel stack before installing exception vectors.
- GICv2 interrupts and the ARM virtual timer at 100 scheduling ticks/second.
- EL0 user programs with private TTBR0 page tables, read-only executable code,
  non-executable writable stacks, instruction-cache synchronization, and bounded
  write/exit/sleep syscalls. Kernel mappings remain in TTBR1.
- Modern VirtIO-MMIO block requests with flush support, split queues, DMA bounce
  buffers, bounds checks, completion validation, timeout/reset, and pinned buffers
  after device failure. The boot disk uses firmware's PCI VirtIO support;
  the kernel's data disk uses MMIO, so the driver cannot mistake it for the boot disk.
- VirtIO-MMIO keyboard events polled by the input service, including arrows, modifiers,
  function keys, and editor controls. PL011 serial is also a basic text console.
- Firmware GOP for early boot, then a kernel-configured 4K RAM framebuffer for
  the graphical UI; PL031 RTC plus the architectural counter for timekeeping.

The launch target fixes `virt,gic-version=2,virtualization=off` and modern MMIO
transports. Other ARM boards, EL2 kernel execution, GICv3, USB host controllers,
and SIMD/FPU context switching are not implemented. ARM SMP scheduling is
implemented for up to eight CPUs; see [multicore](multicore.md). Both kernel builds
continue to compile without SIMD/FPU instructions, and EL0 FP use traps safely.

## What the benchmark means

The kernel reclaims bootloader RAM after switching to its own translation tables
and stacks. Firmware/device reservations are preserved. The memory total includes
framebuffers, owned task memory, allocator metadata and shared kernel storage.

ARM reads the architectural counter frequency directly. A result such as
**24 MHz** is the timer's tick rate, not the CPU core clock or a speed cap.
HVF runs ARM instructions on the host CPU, but scheduling and emulated devices
still add overhead. Cache effects matter: the RAM tests repeatedly access 8 MiB;
the disk test repeats a 32 KiB area. These are not physical DRAM/SSD peak tests.

Use `bench ram 128 KiB` for a smaller working set; units are case-insensitive,
and bare numbers mean MiB. Use `bench ram 256` to test a 256 MiB working set (copies allocate another 256 MiB).
Use `bench caches` for guest-reported cache geometry and a 4 KiB–256 MiB data
working-set sweep. It reports verified bulk read/fill MB/s and randomized dependent
load ns/access. Initialization, warmup, and verification are excluded from timing.
Cache descriptors may be hidden or virtualized; unreported L3 does not prove the
host lacks a shared cache. Instruction-cache speed is not measured. The sweep
includes scheduling, TLB, and loop overhead and cannot isolate a specific cache
level. On Apple Silicon, the VM thread can move between performance and efficiency
cores with different cache configurations. Host cache sizes can be inspected with
`sysctl -a | rg 'hw.perflevel.*(name|l1[di]cachesize|l2cachesize|cpusperl2)'`.

The benchmark chooses the largest available run of up to eight empty records for
scratch writes. Both single and batched paths flush before reporting completion,
verify readback, and restore every original scratch byte. No occupied record is
used. A full disk skips writes. A fragmented disk may still limit batch size.

References: [QEMU ARM virt platform and CPU/accelerator support](https://www.qemu.org/docs/master/system/arm/virt.html),
[VirtIO transport, block, and input specifications](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).

## Verification

```sh
python3 tests/run_memory_tests.py
python3 tests/qemu_arm64.py --accel hvf
python3 tests/qemu_arm64.py --accel hvf display
python3 tests/qemu_arm64.py --accel hvf ramunits ram256 caches ramskip isolation
python3 tests/qemu_arm64.py --accel tcg normal isolation
python3 tests/qemu_hardware_benchmark.py --driver both
```

All VM tests use disposable disks. ARM tests cover GUI keyboard input, editor
navigation/save, directories, reboot persistence, background scheduling, user-mode
fault containment, wrong-architecture rejection, benchmarks, scratch preservation,
RAM release, full/absent/invalid/read-only disks, injected write and flush failures,
and more than 65,536 VirtIO requests to exercise queue-index wrapping.
