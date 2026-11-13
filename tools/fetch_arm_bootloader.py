"""Fetch the pinned ARM EFI loader; leave the existing x86 loader unchanged."""
import hashlib
import io
from pathlib import Path
import tarfile
import urllib.request

VERSION = "12.9.6"
DIGEST = "b38aabb042b0c0f9f562b7741be3c1204838d7bbacfa78bb411c674bd5b984ee"
ROOT = Path(__file__).resolve().parents[1]
URL = f"https://github.com/Limine-Bootloader/Limine/releases/download/v{VERSION}/limine-binary.tar.gz"
request = urllib.request.Request(URL, headers={"User-Agent": "Cronk-ARM64-build"})
with urllib.request.urlopen(request, timeout=60) as response:
    data = response.read(32 * 1024 * 1024 + 1)
if len(data) > 32 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != DIGEST:
    raise SystemExit("ARM64 Limine archive checksum mismatch")
with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
    entries = [entry for entry in archive.getmembers() if Path(entry.name).name == "BOOTAA64.EFI" and entry.isfile()]
    if len(entries) != 1:
        raise SystemExit("Expected exactly one ARM64 EFI loader")
    payload = archive.extractfile(entries[0]).read()
destination = ROOT / "build/limine-arm64/BOOTAA64.EFI"
destination.parent.mkdir(parents=True, exist_ok=True)
temporary = destination.with_suffix(".tmp")
temporary.write_bytes(payload)
temporary.replace(destination)
print(f"Fetched and SHA-256 verified Limine {VERSION} ARM64 loader")
