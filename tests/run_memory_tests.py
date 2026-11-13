"""Host checks for memory correctness; compile against the actual kernel sources."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cronk-memory-") as temp:
    temp = Path(temp)
    flags = ["cc", "-O2", "-fno-builtin", "-fsanitize=undefined", "-g", "-I", str(root / "kernel/src")]
    renamed = [f"-D{name}=kernel_{name}" for name in ("memcpy", "memset", "memmove", "memcmp")]
    subprocess.run(flags + renamed + ["-c", str(root / "kernel/src/lib/memory.c"), "-o", str(temp / "memory.o")], check=True)
    subprocess.run(flags + [str(root / "tests/memory_primitives.c"), str(temp / "memory.o"), "-o", str(temp / "memory")], check=True)
    subprocess.run([str(temp / "memory")], check=True, timeout=30)
    subprocess.run(flags + ["-I", str(root / "kernel/limine-protocol/include"),
                           str(root / "tests/page_allocator.c"), "-o", str(temp / "pages")], check=True)
    subprocess.run([str(temp / "pages")], check=True, timeout=30)
