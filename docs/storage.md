# Contiguous 4 KiB file storage

CRONKFS2 stores each nonempty file in one contiguous extent of 4096-byte pages.
Files contain an explicit byte length and checksum. Allocation rounds upward to
whole pages and reports fragmentation separately from directory-slot exhaustion.
The initial file/editor limit is **65,535 bytes** (up to 16 pages), with 64 directory
entries and 31-byte names. Folders, paths, copy/move/delete, program installation,
and persistence across reboot are supported. Executables still obey the existing
4 KiB user-code ABI; increasing data-file capacity does not enlarge that ABI.

New run-target data images default to 64 MiB. `tools/storage_image.py` upgrades
CRONKFS1 images before boot, preserving files, folders, executable payloads and
installation markers. It writes and fsyncs an exclusive `.v1.bak` backup before
atomically replacing the original image. Existing backups are never overwritten.
The old format remains readable/writable with its original 479-byte limit when
opened directly. Invalid volumes are never reformatted automatically. Close any
VM using a legacy disk before upgrading it.

```
python3 tools/storage_image.py cronk-arm64-data.img
```

The normal `gmake run-arm64` and x86 run targets perform this check automatically.
To create a separate disk, use a new path and optionally `--size-mib 128`.

## On-disk layout

All integer fields are little endian.

| 4 KiB pages | Content |
| --- | --- |
| 0 | CRONKFS2 superblock: magic, version 2, page size, volume page count |
| 1–2 | Directory bank A, 8192 bytes |
| 3–4 | Directory bank B, 8192 bytes |
| 5 onward | Contiguous file-data extents |

Each bank starts with a 512-byte header containing magic, generation, checksum and
installation markers, followed by 64 entries of 64 bytes and reserved padding.
Entries contain name, parent, kind, byte length, first page, page count and payload
checksum. Mount rejects cycles, duplicate siblings, invalid lengths/ranges and
overlapping live extents.

## Updates and recovery

A save finds a new extent not referenced by either valid directory bank. It writes
and flushes the complete payload, writes/flushes the spare directory body, then
writes/flushes its header last. Mount chooses the highest valid generation. A torn
new bank leaves the predecessor usable. Checksums also detect damaged payloads.
This is accidental-corruption detection, not cryptographic authentication.

The preceding bank keeps its referenced extents until another successful commit
replaces it. Rewrites therefore need contiguous spare space for copy-on-write;
free pages spread over small holes cannot satisfy a larger file. Copy allocates
an independent extent. No compactor or extent chaining is implemented.

Disk benchmarks choose scratch pages absent from both valid banks, restore every
byte afterward, and never write directory metadata. Filesystem operations remain
serialized on CPU 0 while the storage service executes actual block transfers.

## Validation

```
python3 tests/run_extent_tests.py
python3 tests/qemu_arm64.py --accel hvf extents
```

Host tests exercise maximum file size, directory operations, full/fragmented
volumes, replacement reuse, write/flush failure recovery, checksums and upgrade
backup preservation. QEMU loads/edits/copies a 9 KB file, truncates and moves files,
reboots, runs installed programs, checks editor cleanup, and verifies scratch
benchmarks preserve the volume exactly.
