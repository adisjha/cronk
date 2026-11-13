# Limine C Template

### ARM64 on Apple Silicon

Run **`gmake run-arm64`** for the native ARM64 guest using Apple's HVF hardware
virtualization. Inside the OS, run `bench` or `run hello-arm64.bin`.
The graphical shell, editor, file management, memory tools, program manager,
background tasks, and isolated EL0 programs work on ARM. The x86 build remains
available through the original targets.

The ARM build uses a separate `cronk-arm64-data.img`; existing x86 files are
untouched. `gmake arm64` builds without launching, and
`gmake run-arm64 ARM_ACCEL=tcg` selects software emulation for debugging.
Use `gmake run-arm64-fullscreen` for fullscreen. The ARM display now uses a real
3840×2160 framebuffer with UI scale 2; `scale 4` also fits.
See [ARM64 build, architecture, and tests](docs/arm64.md) for dependencies and details.


### Storage and memory performance

QEMU run targets now use VirtIO by default, with the same `cronk-data.img` and
filesystem format. Run `gmake run TOOLCHAIN_PREFIX=x86_64-elf-`, or choose the
legacy driver with `gmake run DISK_DRIVER=ata TOOLCHAIN_PREFIX=x86_64-elf-`.
The driver implements the transitional PCI I/O interface, split virtqueue,
polling completion, and DMA bounce buffers. The QEMU target explicitly uses
`virtio-blk-pci,disable-modern=on`; modern-only VirtIO devices are not supported.
The implementation follows the [OASIS VirtIO specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
particularly legacy PCI layout, split queues, and block flush semantics.

The block API supports transfers of 1–64 sectors (up to 32 KiB). CRONKFS2 uses
contiguous 4 KiB file-data pages and two checksummed directory banks.
ATA still transfers each sector through PIO; VirtIO transfers the data through
DMA. A buffered multi-sector write must end with `disk_flush()`. The filesystem's
`disk_write()` wrapper always flushes before reporting success. New volumes support files up to 65,535 bytes. Existing legacy disks are backed up
and upgraded atomically by run targets. See [storage format and recovery](docs/storage.md).

Memory copies and fills use unrolled, unaligned-safe 64-bit scalar operations;
`memmove` handles overlap in either direction. No SIMD/FPU state support is needed.
The page allocator retains first-fit behavior, with per-region free counts and a
lowest-free-page hint to avoid scanning occupied prefixes. Allocations are still
zeroed. Benchmark buffers and device DMA have distinct ownership labels and do
not count toward peak program allocation. Device buffers remain pinned if a
VirtIO request times out, to avoid reusing memory that might still receive DMA.

### Hardware benchmarks

Run `bench` for CPU, RAM, and disk tests (normally about 3–7 seconds).
Use `bench cpu`, `bench ram`, or `bench disk` to run one section.
Use `bench ram 256` for a 256 MiB working set, or `bench caches` for a
4 KiB–256 MiB sweep of bulk reads, fills, and random dependent loads.
The original two-job scheduler comparison is available as `bench sched`.

x86 CPU results show advertised base frequency when available, a roughly one-second
PIT calibration of the TSC timer, and checksum-verified xorshift throughput.
ARM reads its architectural generic-counter frequency directly. Neither timer
frequency is a measurement of live core frequency.

RAM tests compare scalar sequential reads/writes against bulk reads and fills
over an 8 MiB working set. Copies compare a byte loop against bulk `memcpy`,
using a second 8 MiB buffer; reported copy bandwidth counts payload bytes, not
combined read-plus-write bus traffic. Every result is verified and buffers freed.
`bench ram <size> [KiB|MiB]` accepts 1 KiB–256 MiB; bare numbers mean MiB.
For example, `bench ram 128 KiB` tests 128 KiB (`128kib` also works).
Copies need a second equally sized buffer.
Insufficient contiguous free memory skips the affected test safely.

`bench caches` also lists guest-reported cache sizes, line sizes, and associativity
when available. Virtualized descriptors may differ from host hardware; missing
levels mean unknown. The sweep measures data access, not instruction-cache speed.
Warm read/fill bandwidth and dependent-load nanoseconds reveal working-set effects,
but do not isolate individual cache levels. Loads include TLB and VM overhead;
fills use ordinary cached stores. Setup and result verification are not timed.

Disk tests compare 512-byte reads with 32 KiB reads, and per-sector flushed writes
with multi-sector writes flushed once per batch. R/W/flush command counts show
the difference in overhead. Scratch space is a run of up to eight empty file slots;
filenames remain empty, readback is verified, and original record bytes restored.
No occupied file slot or filesystem header is written. A full disk skips writes;
an unavailable or invalid disk skips disk tests. Writes are flushed on both paths.

Rates use decimal MB/s and elapsed guest time, including other running tasks.
RAM caching and QEMU/host disk caching affect results. These measure the current
guest access paths over a small working set, not peak physical RAM/SSD bandwidth.

Checks (after building the ISO):

- `python3 tests/run_memory_tests.py`: alignment, overlap, guard pages, undefined
  behavior checks, allocator zeroing, reuse, fragmentation, and accounting.
- `python3 tests/qemu_hardware_benchmark.py --driver both`: temporary-disk tests
  for ATA and VirtIO, disk preservation, RAM release, saves across reboot,
  write/flush failures, full/invalid/missing disks, read-only VirtIO, and queue wrap.

### Task manager

Run `top` for one table containing every live task, its CPU usage, and its owned
RAM. The shell, input polling, clock sampling, memory cleanup, storage I/O, and
desktop run as separate scheduled kernel tasks. Opening an app or editor blocks
the shell and executes it in the desktop task, named after the active app.
Storage requests block their caller while the storage task performs the I/O.
System services are protected from `kill`.

RAM includes each task's reserved stack and owned buffers: the desktop owns the
framebuffer, input/storage own their device allocations, and the memory manager
owns allocator metadata. Background programs include their code and user stack.
Shared code and page tables stay in `kernel / shared`. Retained bootloader memory
is a separate `boot memory` reservation. Reservations show no PID or CPU usage;
they are not processes. Task rows plus reservation rows reconcile with RAM used.

`ps` also reports per-task reserved RAM in KiB and accumulated CPU runtime.
Stack reservations are not measurements of how much of each stack was touched.
ARM background tasks execute across up to eight cores; `top` shows per-core CPU
graphs and each task's core assignment. UI/device services stay on CPU 0. Run
`bench parallel` to verify parallel execution. Bootloader RAM is reclaimed after
the kernel owns its tables/stacks. See [multicore details](docs/multicore.md).

### Text editor

In the OS shell, run `edit notes.txt` to create or open a text file.
Paths work relative to the current folder or from `/`; quote paths containing
spaces, for example `edit "my notes.txt"`.

- Arrow keys move the cursor; Enter inserts a new line.
- Backspace and Delete remove text.
- F2 saves to disk. Esc quits, with a discard prompt for unsaved changes.
- A `*` marks unsaved changes. The editor shows usage against the filesystem's
  65,535-byte text-file limit on CRONKFS2 and refuses to edit folders or executable files.

This repository will demonstrate how to set up a basic x86-64 kernel in C using Limine.

It is recommended to cross reference the contents of this repository with [the Limine Bare Bones](https://osdev.wiki/wiki/Limine_Bare_Bones) OSDev wiki page.

## How to use this?

### Dependencies

Any `make` command depends on GNU make (`gmake`) and is expected to be run using it. This usually means using `make` on most GNU/Linux distros, or `gmake` on other non-GNU systems.

All `make all*` targets depend on a GNU-compatible C toolchain capable of generating x86-64 ELF objects. Usually `gcc/binutils` or `clang/llvm/lld` provided by any x86-64 UNIX like (including Linux) distribution will suffice.

Additionally, building an ISO with `make all` requires `xorriso`, and building a HDD/USB image with `make all-hdd` requires `sgdisk` (usually from `gdisk` or `gptfdisk` packages) and `mtools`.

### Toolchain selection

The `TOOLCHAIN` and `TOOLCHAIN_PREFIX` `make` variables can be used to set the toolchain. `TOOLCHAIN` can be set to `llvm` to use Clang/LLVM.

For example:
```
make TOOLCHAIN=llvm
```
or:
```
make TOOLCHAIN_PREFIX=x86_64-elf-
```

### Makefile targets

Running `make all` will compile the kernel (from the `kernel/` directory) and then generate a bootable ISO image.

Running `make all-hdd` will compile the kernel and then generate a raw image suitable to be flashed onto a USB stick or hard drive/SSD.

Running `make run` will build the kernel and a bootable ISO (equivalent to make all) and then run it using `qemu` (if installed).

Running `make run-hdd` will build the kernel and a raw HDD image (equivalent to make all-hdd) and then run it using `qemu` (if installed).

The `run-uefi` and `run-hdd-uefi` targets are equivalent to their non `-uefi` counterparts except that they boot `qemu` using a UEFI-compatible firmware.
