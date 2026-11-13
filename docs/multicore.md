# Parallel ARM64 scheduling

ARM64 starts up to eight CPUs through Limine's MP protocol. Each CPU has a
permanent stack, CPU index in TPIDR_EL1, private timer deadline, banked GICv2 CPU
interface, current task, accounting counters, and active user address space.
Every online CPU runs the preemptive scheduler at 100 Hz. CPU 0 alone advances
global sleep time; other CPUs use that clock without multiplying its rate.

Background tasks and user programs are placed round-robin across online CPUs.
Tasks stay on their assigned CPU throughout their lifetime. There is no migration
or work stealing yet. Fixed placement also prevents a second CPU from entering a
stack while its old CPU is finishing exception return. Shell and system services
remain on CPU 0, preserving serialization of UI, storage and filesystem calls.
x86 retains its single-CPU PIC implementation.

The scheduler uses an interrupt-safe spinlock for publication, dispatch, wakeup,
accounting and task state changes. Stopping a running task marks it for retirement
on its own CPU. Only then can the memory service release its pages. Allocator
metadata uses a separate interrupt-safe spinlock; large allocation zeroing happens
after claiming the pages and releasing that lock. ARM user page tables and code
cache maintenance support programs executing on different cores.

`top` shows a CPU graph and percentage for each online core, plus each task's
assigned core, CPU use, and owned RAM. A task's 100% means one whole core. CPU
samples use the common architectural counter. Reserved boot/shared RAM has no
PID or CPU use. `ps` includes core assignment and accumulated runtime.

## Boot memory reclamation

The kernel clones the bootloader translation trees into kernel-owned pages before
releasing boot RAM. ARM's bootstrap stack is in the kernel image; x86 preserves
the live bootstrap stack range. Framebuffer descriptors and required response
values are copied. Secondary CPUs switch to owned tables/stacks and finish local
initialization before reclamation. Bootloader-reclaimable ranges then join the
page allocator, with allocator metadata charged normally. Module/executable pages,
firmware-reserved memory and device memory are not indiscriminately freed.

Missing MP support falls back to one CPU. Startup failure or more CPUs than the
eight-slot limit disables boot reclamation so parked bootloader code remains
valid. The serial log prints the reclaimed KiB count. See the official
[Limine memory-layout and MP specification](https://github.com/Limine-Bootloader/limine-protocol/blob/trunk/PROTOCOL.md).

## Try it

```
gmake run-arm64-fullscreen ARM_QEMU_FLAGS='-m 2G -smp 4'
```

Run `bench parallel` for four checksum-verified CPU jobs with per-job core IDs and
aggregate CPU capacity. Four cores can exceed 100% aggregate; a single core cannot.
Use `spawn busy` four times followed by `top` to inspect sustained per-core load.
Stop workers by PID using `kill` or the task manager.

## Checks

```
python3 tests/qemu_arm64.py --accel hvf parallel smp services extents normal isolation
python3 tests/qemu_arm64.py --accel tcg parallel
python3 tests/run_memory_tests.py
```

Parallel tests compare one/four-core checksums and capacity, exercise user
programs on separate CPUs, stop/reap remote tasks, and verify reclaimed RAM.
SMP checks boot with one, two, four and eight CPUs. Services check CPU/RAM ownership
and protected tasks. Desktop/storage service calls currently have one serialized
client; multiple independent callers would require a request queue.
