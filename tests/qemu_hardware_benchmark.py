"""Integration checks against isolated QEMU disks; never opens cronk-data.img.

Build first: gmake all TOOLCHAIN_PREFIX=x86_64-elf-
Run: python3 tests/qemu_hardware_benchmark.py
Requires qemu-system-x86_64 and x86_64-elf-nm on PATH.
"""
import json
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
            data[slot * 512 + 32:slot * 512 + 48] = b"PRESERVE EXACTLY"
    if not full:
        # An empty record may contain stale bytes: restoration must preserve them.
        data[4 * 512 + 32:5 * 512] = bytes(i * 17 % 256 for i in range(480))
    return data


def run_case(case):
    with tempfile.TemporaryDirectory(prefix="cronk-perf-") as temp:
        temp = Path(temp)
        disk = temp / "test.img"
        disk.write_bytes(image(case == "full", case == "invalid"))
        endpoint = temp / "qmp.sock"
        command = [
            "qemu-system-x86_64", "-M", "q35", "-m", "256M",
            "-display", "none", "-vga", "std", "-cdrom", str(ROOT / "template.iso"),
            "-boot", "d", "-qmp", f"unix:{endpoint},server=on,wait=off",
        ]
        if case != "absent":
            command += ["-device", "isa-ide,id=cronkide,iobase=0x1e8,iobase2=0x3ee,irq=11"]
            if case == "ioerror":
                command += ["-blockdev", json.dumps({
                    "driver": "blkdebug", "node-name": "data",
                    "image": {"driver": "raw", "file": {"driver": "file", "filename": str(disk)}},
                    # Six writes install three programs. Allow one scratch
                    # write, then fail the next one to exercise actual restoration.
                    "set-state": [{"event": "pwritev", "state": i, "new_state": i + 1}
                                  for i in range(1, 8)],
                    "inject-error": [{"event": "pwritev", "state": 8, "errno": 5,
                                      "once": True, "immediately": True}],
                })]
            else:
                command += ["-drive", f"if=none,id=data,format=raw,file={disk}"]
            command += ["-device", "ide-hd,bus=cronkide.0,drive=data,werror=report"]
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

                    def wait_for(text, seconds=30):
                        deadline = time.monotonic() + seconds
                        while time.monotonic() < deadline:
                            try:
                                output = history()
                            except BlockingIOError:
                                output = ""
                            if text in output:
                                return output
                            time.sleep(.1)
                        raise AssertionError(f"{case}: timeout waiting for {text!r}\n{output}")

                    def type_command(text):
                        for key in text + "\n":
                            monitor("sendkey " + {" ": "spc", "\n": "ret"}.get(key, key) + " 20")
                            time.sleep(.05)

                    qmp("qmp_capabilities")
                    wait_for("welcome to terminal")
                    baseline = disk.read_bytes()
                    stats_before = memory(symbols["stats"], 4 * 8)
                    type_command("bench" if case == "normal" else "bench disk")
                    output = wait_for("QEMU/host caching affects results; these are guest-path speeds.")
                    assert disk.read_bytes() == baseline, f"{case}: disk changed"
                    # managed, available, and boot counts must survive RAM allocation/free.
                    assert memory(symbols["stats"], 3 * 8) == stats_before[:3 * 8], "RAM leaked"
                    assert "FAILED" not in output, output
                    if case == "normal":
                        for expected in ["checksum OK", "RAM read:", "RAM write:", "RAM verified and released",
                                         "Disk read:", "Disk write:", "unused slot restored"]:
                            assert expected in output, output
                    elif case == "full":
                        assert "no unused file slot" in output, output
                    elif case in ("absent", "invalid"):
                        assert "data disk unavailable or invalid; skipped" in output, output
                    else:
                        assert "I/O or verification error; scratch restored" in output, output
                    print(f"PASS {case}: completed, disk preserved, no RAM leak")
                    if case == "normal":
                        print(output[output.index("HARDWARE BENCHMARK"):])
            finally:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    cases = ("normal", "full", "absent", "invalid", "ioerror")
    for case in sys.argv[1:] or cases:
        assert case in cases, f"Unknown case: {case}"
        run_case(case)
