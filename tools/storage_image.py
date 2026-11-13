"""Create CRONKFS2 volumes or atomically upgrade CRONKFS1, keeping a backup."""
import argparse
import fcntl
import os
from pathlib import Path
import struct
import tempfile
PAGE = 4096
FIRST_DATA = 5
MAX_FILE = 65535

def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value

def bank(entries, installed, generation=1):
    result = bytearray(8192)
    result[:8] = b'CRONKDIR'
    struct.pack_into('<Q', result, 8, generation)
    result[20:52] = installed[:32].ljust(32, b'\0')
    for slot, record in enumerate(entries):
        if record:
            name, parent, kind, size, start, pages, crc = record
            struct.pack_into('<32s8I', result, 512 + slot * 64,
                             name, parent, kind, size, start, pages, crc, 0, 0)
    struct.pack_into('<I', result, 16, checksum(result))
    return result

def image(size_mib=64, legacy=None):
    if size_mib < 1 or size_mib > 1024:
        raise ValueError('volume size must be 1–1024 MiB')
    result = bytearray(size_mib * 1024 * 1024)
    struct.pack_into('<8sIII', result, 0, b'CRONKFS2', 2, PAGE, len(result)//PAGE)
    entries = [None] * 64
    next_page = FIRST_DATA
    installed = bytes(32)
    if legacy is not None:
        if len(legacy) < 65 * 512 or legacy[:8] != b'CRONKFS1':
            raise ValueError('not a complete CRONKFS1 disk; original left untouched')
        installed = legacy[:32]
        for slot in range(64):
            record = legacy[(slot+1)*512:(slot+2)*512]
            if not record[0]:
                continue
            name = record[:32]
            if name[31] or b'/' in name.split(b'\0')[0]:
                raise ValueError(f'invalid legacy filename in slot {slot+1}')
            name=name.split(b'\0')[0].ljust(32,b'\0')
            parent, kind = divmod(record[511], 3)
            if parent > 64:
                raise ValueError('invalid legacy parent')
            if kind == 1:
                length = int.from_bytes(record[509:511], 'little')
                if length > 477:
                    raise ValueError('invalid executable length')
                payload = record[32:32+length]
            elif kind == 0:
                payload = record[32:511].split(b'\0', 1)[0]
            else:
                payload = b''
            pages = (len(payload)+4095)//4096
            start = next_page if pages else 0
            result[start*PAGE:start*PAGE+len(payload)] = payload
            entries[slot] = (name, parent, kind, len(payload), start, pages, checksum(payload))
            next_page += pages
        for index, entry in enumerate(entries):
            if not entry:
                continue
            seen = {index+1}
            parent = entry[1]
            while parent:
                if parent in seen or not entries[parent-1] or entries[parent-1][2] != 2:
                    raise ValueError('invalid legacy directory ancestry')
                seen.add(parent); parent = entries[parent-1][1]
            for other in entries[:index]:
                if other and other[1] == entry[1] and other[0].split(b'\0')[0] == entry[0].split(b'\0')[0]:
                    raise ValueError('duplicate legacy filename')
    result[PAGE:3*PAGE] = bank(entries, installed)
    result[3*PAGE:5*PAGE] = bank(entries, installed, 2)
    return result

def ensure(path, size_mib=64):
    path = Path(path)
    source = None
    try:
        try:
            source = path.open('r+b')
        except FileNotFoundError:
            pass
        if source:
            signature = source.read(8)
            if signature == b'CRONKFS2':
                return
            if signature != b'CRONKFS1':
                raise ValueError(f'{path}: unknown format; left untouched')
            try:
                fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise ValueError(f'{path}: disk is in use; stop its VM before upgrading') from None
            source.seek(0); original = source.read()
            data = image(size_mib, original)  # Validate before creating any backup or replacement.
            backup = path.with_name(path.name + '.v1.bak')
            suffix = 1
            while backup.exists():
                backup = path.with_name(path.name + f'.v1.{suffix}.bak'); suffix += 1
            with backup.open('xb') as saved:
                saved.write(original); saved.flush(); os.fsync(saved.fileno())
            directory=os.open(path.parent,os.O_RDONLY)
            try: os.fsync(directory)
            finally: os.close(directory)
            print(f'Backed up {path} to {backup}')
        else:
            data = image(size_mib)
        fd, name = tempfile.mkstemp(prefix=path.name+'.', dir=path.parent)
        try:
            with os.fdopen(fd, 'wb') as target:
                if source: os.fchmod(target.fileno(),os.fstat(source.fileno()).st_mode & 0o777)
                target.write(data); target.flush(); os.fsync(target.fileno())
            # A concurrently created disk must not be overwritten.
            if source:
                os.replace(name, path)
            else:
                os.link(name, path)
            directory = os.open(path.parent, os.O_RDONLY)
            try: os.fsync(directory)
            finally: os.close(directory)
        finally:
            Path(name).unlink(missing_ok=True)
        print(f'{path}: CRONKFS2, {size_mib} MiB, contiguous 4096-byte pages')
    finally:
        if source: source.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--size-mib', type=int, default=64)
    args = parser.parse_args()
    try:
        ensure(args.path, args.size_mib)
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
