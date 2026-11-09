# Limine C Template

### Hardware benchmarks

Run `bench` for CPU, RAM, and disk tests (normally about 2–5 seconds).
Use `bench cpu`, `bench ram`, or `bench disk` to run one section.
The original two-job scheduler comparison is available as `bench sched`.

CPU results show the advertised base frequency when available, a roughly
one-second PIT calibration of the TSC timer, and checksum-verified xorshift
throughput. TSC frequency is not a measurement of live core frequency.

RAM results measure scalar sequential reads and writes over an allocated
8 MiB working set, verify contents, and release the allocation afterward.
Disk results measure 512-byte ATA PIO reads over the 32 KiB file area and
writes with a cache flush after every sector. Writes use an unused file slot,
keep its filename empty, verify readback, and restore its original bytes.
No occupied file slot or filesystem header is written. A full disk skips
the write test; an unavailable or invalid disk skips both disk tests.

Rates use decimal MB/s and elapsed guest time, including other running tasks.
RAM caching and QEMU/host disk caching affect results. These tests measure
the current guest access paths, not bare-metal peak RAM/SSD performance.

Integration checks use temporary disks and verify exact disk preservation,
RAM release, and error handling:
`python3 tests/qemu_hardware_benchmark.py` (after building the ISO).

### Text editor

In the OS shell, run `edit notes.txt` to create or open a text file.
Paths work relative to the current folder or from `/`; quote paths containing
spaces, for example `edit "my notes.txt"`.

- Arrow keys move the cursor; Enter inserts a new line.
- Backspace and Delete remove text.
- F2 saves to disk. Esc quits, with a discard prompt for unsaved changes.
- A `*` marks unsaved changes. The editor shows usage against the filesystem's
  479-byte text-file limit and refuses to edit folders or executable files.

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
