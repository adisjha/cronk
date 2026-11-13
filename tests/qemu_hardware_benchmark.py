"""Integration checks against isolated QEMU disks; never opens cronk-data.img.

Build first: gmake all TOOLCHAIN_PREFIX=x86_64-elf-
Run: python3 tests/qemu_hardware_benchmark.py
Requires qemu-system-x86_64 and x86_64-elf-nm on PATH.
"""
import json
import argparse
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
symbols = {}
for line in subprocess.check_output(
    ["x86_64-elf-nm", str(ROOT / "kernel/bin/kernel")], text=True
).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)


def image(full=False, invalid=False):
    data = bytearray(65 * 512)
    data[:8] = b"INVALID!" if invalid else b"CRONKFS1"
    for slot in range(1, 65):
        if full or slot == 8:
            name = f"keep{slot}.txt".encode()
            data[slot * 512:slot * 512 + len(name)] = name
            data[slot * 512 + 32:slot * 512 + 48] = b"preserve exactly"
    if not full:
        # An empty record may contain stale bytes: restoration must preserve them.
        data[4 * 512 + 32:5 * 512] = bytes(i * 17 % 256 for i in range(480))
    return data


def run_case(case, driver="ata"):
    if driver == "ata" and case in ("readonly", "wrap"):
        return
    with tempfile.TemporaryDirectory(prefix="cronk-perf-") as temp:
        temp = Path(temp)
        disk = temp / "test.img"
        disk.write_bytes(image(case == "full", case == "invalid"))
        endpoint = temp / "qmp.sock"
        command = [
            "qemu-system-x86_64", "-M", "q35", "-m", "1G" if case == "caches" else "256M",
            "-display", "none", "-vga", "std", "-cdrom", str(ROOT / "template.iso"),
            "-boot", "d", "-qmp", f"unix:{endpoint},server=on,wait=off",
        ]
        if case != "absent":
            if driver == "ata":
                command += ["-device", "isa-ide,id=cronkide,iobase=0x1e8,iobase2=0x3ee,irq=11"]
            if case in ("ioerror", "flusherror"):
                event = "pwritev"
                failure_state = 7 if case == "flusherror" else 8
                command += ["-blockdev", json.dumps({
                    "driver": "blkdebug", "node-name": "data",
                    "image": {"driver": "raw", "file": {"driver": "file", "filename": str(disk)}},
                    # Six writes install three programs. Allow one scratch
                    # write, then fail the next one to exercise actual restoration.
                    "set-state": [{"event": event, "state": i, "new_state": i + 1}
                                  for i in range(1, failure_state)],
                    "inject-error": [{"event": event, "state": failure_state, "errno": 5,
                                      "iotype": "flush" if case == "flusherror" else "write",
                                      "once": True, "immediately": True}],
                })]
            else:
                command += ["-drive", f"if=none,id=data,format=raw,file={disk}" +
                            (",readonly=on" if case == "readonly" else "")]
            command += ["-device", "virtio-blk-pci,drive=data,disable-modern=on,werror=report" if driver == "virtio" else
                        "ide-hd,bus=cronkide.0,drive=data,werror=report"]
        with (temp / "qemu.log").open("w+") as errors:
            process = subprocess.Popen(command, stderr=errors)
            try:
                deadline = time.monotonic() + 15
                while not endpoint.exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        errors.seek(0)
                        raise RuntimeError(errors.read() or "QEMU startup timeout")
                    time.sleep(.05)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    while True:
                        try:
                            connection.connect(str(endpoint))
                            break
                        except (ConnectionRefusedError, FileNotFoundError):
                            if process.poll() is not None or time.monotonic() > deadline:
                                errors.seek(0)
                                raise RuntimeError(errors.read() or "QMP connection timeout")
                            time.sleep(.05)
                    stream = connection.makefile("rwb", buffering=0)
                    stream.readline()

                    def qmp(name, arguments=None):
                        stream.write((json.dumps({"execute": name, "arguments": arguments or {}}) + "\n").encode())
                        while True:
                            result = json.loads(stream.readline())
                            if "return" in result:
                                return result["return"]
                            if "error" in result:
                                raise RuntimeError(result["error"])

                    def monitor(command):
                        return qmp("human-monitor-command", {"command-line": command})

                    def memory(address, length):
                        output = monitor(f"x /{length}bx 0x{address:x}")
                        if "Cannot access memory" in output:
                            raise BlockingIOError("Kernel mapping not installed yet")
                        values = []
                        for line in output.splitlines():
                            if ":" in line:
                                values += re.findall(r"0x([0-9a-fA-F]{2})(?=\s|$)", line.split(":", 1)[1])
                        result = bytes(int(value, 16) for value in values)
                        assert len(result) == length, output[:300]
                        return result

                    def history():
                        length = int.from_bytes(memory(symbols["history_length"], 8), "little")
                        assert length < 65536
                        return memory(symbols["history"], length).decode("ascii", errors="replace") if length else ""

                    def wait_for(text, seconds=30, after=0):
                        deadline = time.monotonic() + seconds
                        while time.monotonic() < deadline:
                            try:
                                output = history()
                            except BlockingIOError:
                                output = ""
                            if text in output[after:]:
                                return output[after:]
                            time.sleep(.1)
                        raise AssertionError(f"{case}: timeout waiting for {text!r}\n{output}")

                    def type_command(text):
                        for key in text + "\n":
                            monitor("sendkey " + {" ": "spc", "\n": "ret", ".": "dot"}.get(key, key) + " 20")
                            time.sleep(.05)

                    qmp("qmp_capabilities")
                    wait_for("\n> ")
                    baseline = disk.read_bytes()
                    stats_before = memory(symbols["stats"], 4 * 8)
                    if case == "caches":
                        cursor = len(history())
                        type_command("bench caches")
                        output = wait_for("these are guest-path speeds.", seconds=90, after=cursor)
                        assert "FAILED" not in output and "Cache sweep verified and released" in output, output
                        rows = re.findall(r'^\s+(\d+) (KiB|MiB)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', output, re.M)
                        assert len(rows) == 17 and all(float(v) > 0 for row in rows for v in row[2:]), output
                        assert disk.read_bytes() == baseline, "Cache sweep changed disk"
                        assert memory(symbols["stats"], 24) == stats_before[:24], "Cache sweep leaked RAM"
                        print(output)
                        print(f"PASS {driver}/caches: 17 verified working sets, disk preserved, no RAM leak")
                        return
                    if case == "flusherror":
                        cursor = len(history())
                        type_command("write keep8.txt changed")
                        output = wait_for("disk I/O error", after=cursor)
                        assert "\nsaved\n" not in output, output
                        cursor = len(history())
                        type_command("write keep8.txt preserve exactly")
                        wait_for("\nsaved\n", after=cursor)
                        assert disk.read_bytes() == baseline, "Restored filesystem save differs"
                        print(f"PASS {driver}/flusherror: failed flush never reported saved; recovery works")
                        return
                    if case == "wrap":
                        for _ in range(32):
                            cursor = len(history())
                            type_command("bench disk")
                            output = wait_for("these are guest-path speeds.", after=cursor)
                            assert "FAILED" not in output and "scratch run restored" in output, output
                            commands = memory(symbols["counters"], 24)
                            total = sum(int.from_bytes(commands[i:i+8], "little") for i in range(0, 24, 8))
                            if total > 70000:
                                break
                        assert total > 70000, "Did not exercise the 16-bit queue index wrap"
                        assert disk.read_bytes() == baseline, "Disk changed during queue wrap"
                        assert memory(symbols["stats"], 3 * 8) == stats_before[:3 * 8]
                        print(f"PASS virtio/wrap: {total} requests, disk preserved, no RAM leak")
                        return
                    type_command("bench" if case == "normal" else "bench disk")
                    output = wait_for("QEMU/host caching affects results; these are guest-path speeds.")
                    assert disk.read_bytes() == baseline, f"{case}: disk changed"
                    # managed, available, and boot counts must survive RAM allocation/free.
                    assert memory(symbols["stats"], 3 * 8) == stats_before[:3 * 8], "RAM leaked"
                    assert "FAILED" not in output, output
                    if case == "normal":
                        for expected in ["checksum OK", "RAM read:", "RAM write:", "RAM verified and released",
                                         "Disk read:", "Disk write:", "Disk batch read:", "Disk batch write:", "scratch run restored",
                                         "RAM bulk read:", "RAM bulk fill:", "RAM bulk copy:"]:
                            assert expected in output, output
                        assert ("VirtIO PCI" if driver == "virtio" else "ATA PIO") in output, output
                        before = memory(symbols["counters"], 24)
                        cursor = len(history())
                        type_command("write persisted.txt durable")
                        wait_for("\nsaved\n", after=cursor)
                        after = memory(symbols["counters"], 24)
                        for field in (8, 16):
                            assert int.from_bytes(after[field:field+8], "little") - int.from_bytes(before[field:field+8], "little") == 1
                        qmp("system_reset")
                        time.sleep(2)
                        wait_for("\n> ")
                        cursor = len(history())
                        type_command("cat persisted.txt")
                        wait_for("\ndurable\n", after=cursor)
                    elif case == "full":
                        assert "no unused file slot" in output, output
                    elif case in ("absent", "invalid"):
                        assert "data disk unavailable or invalid; skipped" in output, output
                    else:
                        assert "I/O or verification error; scratch restored" in output, output
                    print(f"PASS {driver}/{case}: completed, disk preserved, no RAM leak")
                    if case == "normal":
                        print(output[output.index("HARDWARE BENCHMARK"):])
            finally:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    cases = ("normal", "full", "absent", "invalid", "ioerror", "flusherror", "readonly", "wrap")
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", choices=("ata", "virtio", "both"), default="ata")
    parser.add_argument("cases", nargs="*", metavar="CASE")
    args = parser.parse_args()
    for driver in (("ata", "virtio") if args.driver == "both" else (args.driver,)):
        for case in args.cases or cases:
            assert case in cases + ("caches",), f"Unknown case: {case}"
            run_case(case, driver)
