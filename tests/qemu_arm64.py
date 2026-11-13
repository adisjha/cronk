"""ARM64 integration tests. Build with gmake arm64; all data disks are temporary.

python3 tests/qemu_arm64.py --accel hvf
python3 tests/qemu_arm64.py --accel tcg normal isolation
"""
import argparse
import json
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
LLVM = Path('/opt/homebrew/opt/llvm/bin')
SYMBOLS = {}
for line in subprocess.check_output([shutil.which('llvm-nm') or str(LLVM / 'llvm-nm'),
                                     str(ROOT / 'kernel/bin/aarch64/kernel')], text=True).splitlines():
    parts = line.split()
    if len(parts) == 3:
        SYMBOLS[parts[2]] = int(parts[0], 16)


def make_image(case):
    data = bytearray(65 * 512)
    data[:8] = b'INVALID!' if case == 'invalid' else b'CRONKFS1'
    for slot in range(1, 65):
        if case == 'full' or slot == 8:
            name = f'keep{slot}.txt'.encode()
            data[slot * 512:slot * 512 + len(name)] = name
            data[slot * 512 + 32:slot * 512 + 48] = b'preserve exactly'
    if case != 'full':
        data[4 * 512 + 32:5 * 512] = bytes(i * 17 % 256 for i in range(480))
    return data


class VM:
    def __init__(self, folder, accel, case, image=None, resolution='3840x2160', memory='256M', cpus=1):
        self.folder = Path(folder)
        self.disk = self.folder / 'data.img'
        self.disk.write_bytes(make_image(case) if image is None else image)
        self.serial = self.folder / 'serial.log'
        self.serial.write_text('')
        endpoint = self.folder / 'qmp.sock'
        firmware = next((p for p in [Path('/opt/homebrew/share/qemu/edk2-aarch64-code.fd'),
                                    Path('/usr/share/qemu/edk2-aarch64-code.fd'),
                                    Path('/usr/share/qemu-efi-aarch64/QEMU_EFI.fd')] if p.exists()), None)
        assert firmware, 'ARM EDK2 firmware not found'
        args = ['qemu-system-aarch64', '-machine', 'virt,gic-version=2,virtualization=off', '-accel', accel,
                '-cpu', 'host' if accel != 'tcg' else 'cortex-a72', '-m', memory, '-smp', str(cpus),
                '-bios', str(firmware), '-device', 'ramfb',
                '-fw_cfg', f'name=opt/org.cronk/resolution,string={resolution}',
                '-global', 'virtio-mmio.force-legacy=false',
                '-drive', f'if=none,id=boot,format=raw,file={ROOT}/cronk-arm64-boot.img,readonly=on',
                '-device', 'virtio-blk-pci,drive=boot,bootindex=0,romfile=',
                '-device', 'virtio-keyboard-device', '-display', 'none',
                '-serial', f'file:{self.serial}', '-qmp', f'unix:{endpoint},server=on,wait=off']
        if case != 'absent':
            if case in ('ioerror', 'flusherror'):
                state = 7 if case == 'flusherror' else 8
                args += ['-blockdev', json.dumps({
                    'driver': 'blkdebug', 'node-name': 'data',
                    'image': {'driver': 'raw', 'file': {'driver': 'file', 'filename': str(self.disk)}},
                    'set-state': [{'event': 'pwritev', 'state': i, 'new_state': i + 1} for i in range(1, state)],
                    'inject-error': [{'event': 'pwritev', 'state': state, 'errno': 5, 'once': True,
                                      'immediately': True, 'iotype': 'flush' if case == 'flusherror' else 'write'}],
                })]
            else:
                args += ['-drive', f'if=none,id=data,format=raw,file={self.disk}' + (',readonly=on' if case == 'readonly' else '')]
            args += ['-device', 'virtio-blk-device,drive=data,werror=report']
        self.errors = (self.folder / 'stderr.log').open('w+')
        self.process = subprocess.Popen(args, stderr=self.errors)
        self.connection = None
        try:
            deadline = time.monotonic() + 30
            while not endpoint.exists():
                if self.process.poll() is not None or time.monotonic() > deadline:
                    self.errors.seek(0)
                    raise RuntimeError(self.errors.read() or 'QEMU startup timeout')
                time.sleep(.05)
            self.connection = socket.socket(socket.AF_UNIX)
            self.connection.settimeout(10)
            while True:
                try:
                    self.connection.connect(str(endpoint)); break
                except (ConnectionRefusedError, FileNotFoundError):
                    if self.process.poll() is not None or time.monotonic() > deadline:
                        self.errors.seek(0)
                        raise RuntimeError(self.errors.read() or 'QMP connection timeout')
                    time.sleep(.05)
            self.stream = self.connection.makefile('rwb', buffering=0)
            self.stream.readline()
            self.qmp('qmp_capabilities')
            self.wait('welcome to terminal')
            self.wait('\n> ')
        except BaseException:
            self.close(); raise

    def close(self):
        self.process.terminate()
        self.process.wait(timeout=10)
        if self.connection:
            self.connection.close()
        self.errors.close()

    def qmp(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
        while True:
            result = json.loads(self.stream.readline())
            if 'return' in result: return result['return']
            if 'error' in result: raise RuntimeError(result['error'])

    def monitor(self, command):
        return self.qmp('human-monitor-command', {'command-line': command})

    def output(self):
        return self.serial.read_text(errors='replace').replace('\r', '')

    def wait(self, text, after=0, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            output = self.output()[after:]
            assert 'ARM64 kernel exception' not in output, output
            if text in output: return output
            if self.process.poll() is not None: break
            time.sleep(.05)
        self.monitor(f'screendump {self.folder}/failure.ppm')
        raise AssertionError(f'Timeout waiting for {text!r}\n{self.output()[-5000:]}')

    def key(self, key):
        self.monitor('sendkey ' + key + ' 20')
        time.sleep(.035)

    def type(self, text):
        names = {' ': 'spc', '\n': 'ret', '.': 'dot', '/': 'slash', '-': 'minus', '"': 'shift-apostrophe'}
        for char in text:
            self.key(names.get(char, 'shift-' + char.lower() if char.isupper() else char))

    def command(self, command, expected='\n> ', seconds=30):
        cursor = len(self.output())
        self.type(command + '\n')
        return self.wait(expected, cursor, seconds)

    def memory(self, address, size):
        output = self.monitor(f'x /{size}bx 0x{address:x}')
        values = []
        for line in output.splitlines():
            if ':' in line:
                values += re.findall(r'0x([0-9a-fA-F]{2})(?=\s|$)', line.split(':', 1)[1])
        result = bytes(int(value, 16) for value in values)
        assert len(result) == size, output[:300]
        return result


def display_case(accel):
    for requested, expected, initial in [('3840x2160', '3840x2160', 2),
                                         ('1920x1080', '1920x1080', 1),
                                         ('999999999999x2160', '3840x2160', 2)]:
        with tempfile.TemporaryDirectory(prefix='cronk-arm-display-') as folder:
            vm = VM(folder, accel, 'normal', resolution=requested)
            try:
                baseline = vm.disk.read_bytes()
                stats = vm.memory(SYMBOLS['stats'], 24)
                assert expected + '   UI scale ' + str(initial) + 'x' in vm.command('scale')
                scales = (1,2,3,4) if expected == '3840x2160' else (1,2)
                for scale in scales:
                    result = vm.command('scale ' + str(scale))
                    assert expected + '   UI scale ' + str(scale) + 'x' in result, result
                if expected == '1920x1080':
                    assert 'choose 1-4' in vm.command('scale 4')
                vm.command('scale 2')
                time.sleep(.25)
                image = Path(folder) / 'display.ppm'
                vm.monitor(f'screendump {image}')
                with image.open('rb') as pixels:
                    assert pixels.readline().strip() == b'P6'
                    assert pixels.readline().strip().decode() == expected.replace('x',' ')
                assert vm.disk.read_bytes() == baseline
                assert vm.memory(SYMBOLS['stats'],24) == stats
                print(f'PASS ARM64/{accel}/display: {requested} -> {expected}, host pixels verified, scale controls work')
            finally:
                vm.close()


def isolation_image(folder):
    code = {
        'kernel': 'ldr x9, =0xffffffff80000000; ldr x0, [x9]; ret',
        'code': 'mov x9, #0x400000; str xzr, [x9]; ret',
        'stack': 'mov x9, sp; br x9',
        'pointer': 'ldr x1, =0xffffffff80000000; mov x8, #1; svc #0; ret',
        'fp': 'fmov d0, xzr; ret',
        'syscall': 'mov x8, #99; svc #0; ret',
    }
    data = make_image('normal')
    for slot, (name, body) in enumerate(code.items(), 16):
        source = Path(folder) / (name + '.S')
        source.write_text('.text\n.ascii "CRONKA64"\n' + body + '\n.ltorg\n')
        obj = source.with_suffix('.o'); raw = source.with_suffix('.bin')
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([shutil.which('llvm-objcopy') or str(LLVM / 'llvm-objcopy'), '-O', 'binary',
                        '--only-section=.text', str(obj), str(raw)], check=True)
        payload = raw.read_bytes()
        encoded_name = (name + '.bin').encode()
        start = slot * 512
        data[start:start+len(encoded_name)] = encoded_name
        data[start+32:start+32+len(payload)] = payload
        data[start+509:start+511] = len(payload).to_bytes(2, 'little')
        data[start+511] = 1
    # A valid x86 executable must be rejected before entering ARM user mode.
    start = 24 * 512
    data[start:start+9] = b'wrong.bin'
    data[start+32:start+41] = b'CRONK001\xc3'
    data[start+509] = 9; data[start+511] = 1
    return data, code


def run_case(case, accel):
    with tempfile.TemporaryDirectory(prefix='cronk-arm64-') as folder:
        special, checks = isolation_image(folder) if case == 'isolation' else (None, {})
        vm = VM(folder, accel, case, special, memory='1G' if case in ('ram256', 'caches') else '256M', cpus=4 if case=='isolation' else 1)
        try:
            baseline = vm.disk.read_bytes()
            stats = vm.memory(SYMBOLS['stats'], 24)
            if case == 'ramunits':
                for command, label in [('bench ram 1 KiB', '1 KiB'),
                                       ('bench ram 128kib', '128 KiB'),
                                       ('bench ram 1537 KiB', '1537 KiB'),
                                       ('bench ram 1 MiB', '1 MiB'),
                                       ('bench ram 1', '1 MiB')]:
                    result = vm.command(command)
                    assert 'RAM: ' + label + ' working set' in result, result
                    assert 'RAM verified and released' in result and 'FAILED' not in result, result
                    assert 'RAM bulk copy:' in result, result
                    assert vm.memory(SYMBOLS['stats'],24) == stats, 'RAM benchmark leaked memory'
                for size in ('0 KiB', '262145KiB', '257 MiB', '999999999999999999', '2 KB', 'KiB', '1KiBjunk'):
                    result = vm.command('bench ram ' + size)
                    assert 'usage: bench ram' in result and 'HARDWARE BENCHMARK' not in result, result
                assert vm.disk.read_bytes() == baseline, 'RAM benchmark changed disk'
                print(f'PASS ARM64/{accel}/ramunits: KiB/MiB, unaligned sizes, legacy syntax, bounds, no RAM leak')
                return
            if case in ('ram256', 'caches', 'ramskip'):
                command = 'bench caches' if case == 'caches' else 'bench ram 256'
                result = vm.command(command, seconds=90)
                assert 'FAILED' not in result, result
                assert vm.disk.read_bytes() == baseline, 'RAM/cache benchmark changed disk'
                assert vm.memory(SYMBOLS['stats'],24) == stats, 'RAM/cache benchmark leaked memory'
                if case == 'ramskip':
                    assert '256 MiB of contiguous free RAM; skipped' in result, result
                elif case == 'ram256':
                    for expected in ('RAM: 256 MiB working set', 'RAM read:', 'RAM write:',
                                     'RAM bulk read:', 'RAM bulk fill:', 'RAM bulk copy:', 'RAM verified and released'):
                        assert expected in result, result
                else:
                    assert 'Guest-reported cache geometry' in result and 'Cache sweep verified and released' in result, result
                    rows = re.findall(r'^\s+(\d+) (KiB|MiB)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', result, re.M)
                    assert len(rows) == 17, result
                    assert all(float(value) > 0 for row in rows for value in row[2:]), result
                    assert rows[0][:2] == ('4','KiB') and rows[-1][:2] == ('256','MiB'), result
                assert 'usage: bench ram' in vm.command('bench ram 257')
                assert 'usage: bench ram' in vm.command('bench ram 0')
                assert '\nalive\n' in vm.command('echo alive')
                print(result)
                print(f'PASS ARM64/{accel}/{case}: bounds checked, disk preserved, no RAM leak')
                return
            if case == 'isolation':
                for name in checks:
                    result = vm.command('run ' + name + '.bin')
                    assert name + '.bin stopped: EL0 exception ' in result, result
                for name in checks:
                    result=vm.command('runbg '+name+'.bin')
                    assert 'as task' in result,result
                deadline=time.monotonic()+5
                while vm.memory(SYMBOLS['stats'],24)!=stats:
                    assert time.monotonic()<deadline,'remote user fault leaked memory'
                    time.sleep(.02)
                result = vm.command('run wrong.bin')
                assert 'invalid executable' in result, result
                assert '\nalive\n' in vm.command('echo alive')
                assert vm.disk.read_bytes() == baseline
                assert vm.memory(SYMBOLS['stats'],24) == stats
                print(f'PASS ARM64/{accel}/isolation: kernel/code/stack/pointer/FP/syscall blocked; wrong ISA rejected; shell survived')
                return
            if case == 'flusherror':
                result = vm.command('write keep8.txt changed')
                assert 'disk I/O error' in result and '\nsaved\n' not in result, result
                assert '\nsaved\n' in vm.command('write keep8.txt preserve exactly')
                assert vm.disk.read_bytes() == baseline
                print(f'PASS ARM64/{accel}/flusherror: failed flush never reported saved')
                return
            if case == 'wrap':
                for _ in range(32):
                    result = vm.command('bench disk')
                    assert 'FAILED' not in result and 'scratch run restored' in result, result
                    counts = vm.memory(SYMBOLS['counters'], 24)
                    total = sum(int.from_bytes(counts[i:i+8], 'little') for i in range(0,24,8))
                    if total > 70000: break
                assert total > 70000
                assert vm.disk.read_bytes() == baseline
                assert vm.memory(SYMBOLS['stats'],24) == stats
                print(f'PASS ARM64/{accel}/wrap: {total} requests, disk preserved, no RAM leak')
                return
            if case == 'normal':
                assert '3840x2160   UI scale 2x' in vm.command('scale')
                assert 'hello-arm64.bin' in vm.command('ls')
                assert 'Hello from an ARM64 Cronk program!' in vm.command('run hello-arm64.bin')
                assert 'EL0 exception' in vm.command('run crash-arm64.bin')
                assert 'started counter-arm64.bin as task 2' in vm.command('runbg counter-arm64.bin')
                assert 'started worker 3' in vm.command('spawn')
                time.sleep(.15)
                listing = vm.command('ps')
                assert re.search(r'2\s+0\s+counter-arm64.bin', listing), listing
                cursor = len(vm.output()); vm.type('top\n'); time.sleep(1.2)
                vm.monitor(f'screendump {vm.folder}/manager.ppm')
                vm.key('esc'); vm.wait('\n> ', cursor)
                assert 'stopped' in vm.command('kill 2')
                assert 'stopped' in vm.command('kill 3')
                cursor = len(vm.output()); vm.type('edit notes.txt\n'); time.sleep(.2)
                vm.type('hello\nworld'); vm.key('left'); vm.key('backspace'); vm.type('L'); vm.key('f2'); vm.key('esc')
                vm.wait('\n> ', cursor)
                assert 'hello\nworLd' in vm.command('cat notes.txt')
                assert 'folder created' in vm.command('mkdir docs')
                vm.command('cd docs')
                assert 'saved' in vm.command('write nested.txt durable')
                assert '/docs' in vm.command('pwd')
                vm.command('cd /')
                baseline = vm.disk.read_bytes()
                stats = vm.memory(SYMBOLS['stats'], 24)
            result = vm.command('bench' if case == 'normal' else 'bench disk', seconds=45)
            assert 'FAILED' not in result, result
            assert vm.disk.read_bytes() == baseline, 'Benchmark changed disk records'
            assert vm.memory(SYMBOLS['stats'],24) == stats, 'Benchmark leaked RAM'
            if case == 'normal':
                for expected in ['checksum OK', 'RAM bulk read:', 'RAM bulk fill:', 'RAM bulk copy:',
                                 'VirtIO MMIO', 'Disk batch read:', 'Disk batch write:', 'scratch run restored']:
                    assert expected in result, result
                print(result)
                cursor = len(vm.output())
                vm.qmp('system_reset')
                vm.wait('welcome to terminal', after=cursor)
                vm.wait('\n> ', after=cursor)
                output = vm.command('cat /docs/nested.txt')
                assert 'durable' in output, output
                output = vm.command('cat notes.txt')
                assert 'hello\nworLd' in output, output
            elif case == 'full': assert 'no unused file slot' in result, result
            elif case in ('absent','invalid'): assert 'data disk unavailable or invalid' in result, result
            else: assert 'I/O or verification error; scratch restored' in result, result
            print(f'PASS ARM64/{accel}/{case}: disk preserved, no RAM leak')
        finally:
            vm.close()


def parallel_case(accel):
    checksums=[]
    for cores in (1,4):
        with tempfile.TemporaryDirectory(prefix='cronk-parallel-') as folder:
            vm=VM(folder,accel,'normal',cpus=cores,resolution='1280x720')
            try:
                boot=vm.output()
                reclaimed=re.search(r'Boot RAM reclaimed: (\d+) KiB',boot)
                assert reclaimed and int(reclaimed[1])>4096,boot
                result=vm.command('bench parallel',seconds=40)
                jobs=re.findall(r'job (\d+) core (\d+) checksum (\d+)',result)
                assert len(jobs)==4 and 'FAILED' not in result,result
                assert {int(row[1]) for row in jobs}==set(range(cores)),result
                capacity=int(re.search(r'capacity used: (\d+)%',result)[1])
                assert capacity<=101 if cores==1 else capacity>150,result
                checksums.append([row[2] for row in jobs])
                stats=vm.memory(SYMBOLS['stats'],24)
                for number in range(6,10):
                    assert f'as task {number}' in vm.command('runbg counter-arm64.bin')
                listing=vm.command('ps')
                assert len(re.findall(r'counter-arm64.bin',listing))==4,listing
                cursor=len(vm.output());vm.type('top\n');time.sleep(1.2)
                vm.monitor(f'screendump {vm.folder}/cores.ppm')
                vm.key('esc');vm.wait('\n> ',cursor)
                for number in range(6,10):assert 'stopped' in vm.command(f'kill {number}')
                deadline=time.monotonic()+5
                while vm.memory(SYMBOLS['stats'],24)!=stats:
                    assert time.monotonic()<deadline,'cross-core stop/reap leaked pages'
                    time.sleep(.02)
                assert '\nalive\n' in vm.command('echo alive')
                print(f'PASS ARM64/{accel}/parallel/{cores}: {capacity}% CPU, pinned tasks, user syscalls, remote kill/reap, boot reclaim',flush=True)
            finally:vm.close()
    assert checksums[0]==checksums[1],checksums


def extent_case(accel):
    import importlib.util
    spec=importlib.util.spec_from_file_location('storage_image',ROOT/'tools/storage_image.py')
    storage=importlib.util.module_from_spec(spec);spec.loader.exec_module(storage)
    data=bytearray(storage.image(4));payload=(b'abcdefghijklmnopqrstuvwxyz'*347)[:9000]
    entries=[None]*64
    entries[9]=(b'large.txt',0,0,len(payload),100,3,storage.checksum(payload))
    data[100*4096:100*4096+len(payload)]=payload
    data[4096:3*4096]=storage.bank(entries,b'')
    data[3*4096:5*4096]=storage.bank(entries,b'',2)
    with tempfile.TemporaryDirectory(prefix='cronk-extents-') as folder:
        vm=VM(folder,accel,'normal',image=data,cpus=4,resolution='1280x720')
        try:
            assert payload.decode() in vm.command('cat large.txt')
            assert 'copied' in vm.command('cp large.txt copy.txt')
            stats=vm.memory(SYMBOLS['stats'],24)
            cursor=len(vm.output());vm.type('edit large.txt\n');time.sleep(.2)
            vm.key('delete');vm.type('A');vm.key('f2');vm.key('esc');vm.wait('\n> ',cursor)
            expected='A'+payload[1:].decode()
            assert expected in vm.command('cat large.txt')
            assert payload.decode() in vm.command('cat copy.txt'),'copy aliased the original extent'
            assert vm.memory(SYMBOLS['stats'],24)==stats,'editor buffers leaked'
            assert 'saved' in vm.command('write copy.txt small')
            assert 'folder created' in vm.command('mkdir docs')
            assert 'moved' in vm.command('mv large.txt docs/large.txt')
            before=vm.disk.read_bytes()
            result=vm.command('bench disk')
            assert 'scratch run restored' in result,result
            assert vm.disk.read_bytes()==before,'extent benchmark changed disk'
            persisted=vm.disk.read_bytes()
        finally:vm.close()
    with tempfile.TemporaryDirectory(prefix='cronk-extents-reboot-') as folder:
        vm=VM(folder,accel,'normal',image=persisted,cpus=4,resolution='1280x720')
        try:
            assert expected in vm.command('cat docs/large.txt')
            assert '\nsmall\n' in vm.command('cat copy.txt')
            assert 'Hello from an ARM64 Cronk program!' in vm.command('run hello-arm64.bin')
            print(f'PASS ARM64/{accel}/extents: 9 KB file/edit/copy/truncate/move, reboot, programs, safe scratch benchmark',flush=True)
        finally:vm.close()


def services_case(accel):
    with tempfile.TemporaryDirectory(prefix='cronk-services-') as folder:
        vm = VM(folder, accel, 'normal', resolution='1280x720')
        def processes():
            result = {}
            for line in vm.command('ps').splitlines():
                match = re.match(r'^(\d+)\s+(\d+)\s+(.+?)\s+(\d+)\s+(\d+)\s+(\d+)\s*(.*)$', line)
                if match:
                    pid, level, name, slices, ram, runtime, output = match.groups()
                    result[int(pid)] = (name, int(ram), int(runtime))
            return result
        try:
            initial = processes()
            expected = {1: 'shell', 1000: 'input', 1001: 'clock', 1002: 'memory manager',
                        1003: 'storage', 1004: 'desktop'}
            assert {pid: initial[pid][0] for pid in expected} == expected, initial
            assert initial[1][1] == 132 and initial[1000][1] >= 64, initial
            assert initial[1004][1] == 64 + 1280 * 720 * 4 // 1024, initial
            for pid in expected:
                assert 'unknown or protected task' in vm.command(f'kill {pid}')
            baseline = vm.memory(SYMBOLS['stats'], 24)
            assert 'as task 2' in vm.command('runbg counter-arm64.bin')
            running = processes()
            assert running[2][1] == 28, running
            cursor = len(vm.output()); vm.type('top\n'); time.sleep(2.2)
            vm.monitor(f'screendump {vm.folder}/manager.ppm')
            vm.key('esc'); vm.wait('\n> ', cursor)
            after = processes()
            assert after[1004][2] > running[1004][2], 'UI CPU was not charged to desktop'
            assert after[1000][2] > running[1000][2], 'input service never ran'
            assert after[1001][2] > running[1001][2], 'clock service never ran'
            assert 'stopped' in vm.command('kill 2')
            deadline = time.monotonic() + 5
            while vm.memory(SYMBOLS['stats'], 24) != baseline:
                assert time.monotonic() < deadline, 'memory service failed to reap program pages'
                time.sleep(.05)
            vm.command('scale 2')  # 640x360 logical: all rows must be reachable by scrolling.
            cursor = len(vm.output()); vm.type('top\n'); time.sleep(.2)
            for _ in range(7): vm.key('down')
            vm.key('k'); vm.key('esc'); vm.wait('\n> ', cursor)
            assert '\nalive\n' in vm.command('echo alive')
            assert set(processes()) == set(expected), 'protected service disappeared'
            print(f'PASS ARM64/{accel}/services: real tasks, private RAM, CPU attribution, protection, reaping, small-screen scrolling', flush=True)
        finally:
            vm.close()


def smp_case(accel):
    for cpus in (1, 2, 4, 8):
        with tempfile.TemporaryDirectory(prefix='cronk-smp-') as folder:
            vm = VM(folder, accel, 'normal', cpus=cpus)
            try:
                output = vm.output()
                assert f'SMP: {cpus} CPU slots; starting parallel scheduler' in output, output
                for index in range(1, cpus):
                    assert f'CPU {index} online (private stack)' in output, output
                assert 'timeout' not in output and 'STACK ERROR' not in output, output
                assert 'Hello from an ARM64 Cronk program!' in vm.command('run hello-arm64.bin')
                assert 'started' in vm.command('spawn')
                assert 'stopped' in vm.command('kill 2')
                assert '\nalive\n' in vm.command('echo alive')
                print(f'PASS ARM64/{accel}/smp/{cpus}: private stacks, CPU readiness, shell/tasks alive', flush=True)
            finally:
                vm.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--accel', choices=('hvf','tcg','kvm'), default='hvf' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'tcg')
    parser.add_argument('cases', nargs='*')
    args = parser.parse_args()
    for case in args.cases or ['display','normal','isolation','full','absent','invalid','readonly','ioerror','flusherror','wrap']:
        assert case in ('normal','isolation','full','absent','invalid','readonly','ioerror','flusherror','wrap','display','ram256','caches','ramskip','ramunits','smp','services','parallel','extents')
        if case == 'parallel': parallel_case(args.accel)
        elif case == 'extents': extent_case(args.accel)
        elif case == 'services': services_case(args.accel)
        elif case == 'smp': smp_case(args.accel)
        elif case == 'display': display_case(args.accel)
        else: run_case(case, args.accel)
