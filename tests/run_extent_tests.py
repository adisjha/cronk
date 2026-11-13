from pathlib import Path
import importlib.util
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cronk-extents-') as folder:
    binary=Path(folder)/'extents'
    subprocess.run(['cc','-O2','-g','-fsanitize=undefined','-I',str(root/'kernel/src'),'-I',str(root/'kernel/limine-protocol/include'),str(root/'tests/extent_filesystem.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
    spec=importlib.util.spec_from_file_location('image',root/'tools/storage_image.py')
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    legacy=bytearray(65*512);legacy[:8]=b'CRONKFS1';legacy[11]=1
    legacy[512:517]=b'notes';legacy[544:549]=b'hello'
    path=Path(folder)/'data.img';path.write_bytes(legacy)
    mod.ensure(path,1)
    assert path.read_bytes()[:8]==b'CRONKFS2'
    assert path.with_name('data.img.v1.bak').read_bytes()==legacy
    snapshot=path.read_bytes();mod.ensure(path);assert path.read_bytes()==snapshot
    invalid=Path(folder)/'invalid.img';invalid.write_bytes(b'not a filesystem')
    try: mod.ensure(invalid)
    except ValueError: pass
    else: raise AssertionError('invalid disk was reformatted')
    assert invalid.read_bytes()==b'not a filesystem'
    print('PASS disk upgrade: backup preserved, atomic replacement, idempotence, invalid disk untouched')
