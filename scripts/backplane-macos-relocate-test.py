#!/usr/bin/env python3
"""Native regression for already-installed dylibs and real basename collisions."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

if sys.platform != 'darwin':
    raise SystemExit('requires actual macOS')

spec = importlib.util.spec_from_file_location('relocate', Path(__file__).with_name('backplane-macos-relocate.py'))
relocate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relocate)

with tempfile.TemporaryDirectory(prefix='backplane-dylib-test-') as directory:
    root = Path(directory)
    for name in ('source', 'installed', 'different'):
        (root / name).mkdir()
    source = root / 'source/libsame.dylib'
    target = root / 'installed/libsame.dylib'
    different = root / 'different/libsame.dylib'
    for path, value in ((source, 1), (different, 2)):
        code = path.with_suffix('.c')
        code.write_text(f'int test_marker(void) {{ return {value}; }}\n')
        subprocess.run(['clang', '-dynamiclib', '-Wl,-headerpad_max_install_names', str(code), '-o', str(path)], check=True)
    shutil.copy2(source, target)
    subprocess.run(['install_name_tool', '-id', '@rpath/libsame.dylib', str(target)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)
    assert source.read_bytes() != target.read_bytes(), 'fixture must reproduce CMake relocation/signing'
    relocate.require_same_library(source, target)
    print('PASS already-relocated and signed library retains native identity')
    try:
        relocate.require_same_library(different, target)
    except RuntimeError as error:
        assert 'conflicting dependency basename' in str(error)
    else:
        raise AssertionError('different library with the same basename was accepted')
    print('PASS different native library with same basename is rejected')
    # Exercise the complete relocator with an absolute build-tree dependency,
    # including the already-rewritten copy that triggered the real CI failure.
    bundle = root / 'Test.app'
    frameworks = bundle / 'Contents/Frameworks'
    macos = bundle / 'Contents/MacOS'
    frameworks.mkdir(parents=True)
    macos.mkdir(parents=True)
    shutil.copy2(target, frameworks / target.name)
    code = root / 'main.c'
    code.write_text('int test_marker(void); int main(void) { return test_marker() == 1 ? 0 : 1; }\n')
    probe = macos / 'probe'
    subprocess.run(['clang', '-Wl,-headerpad_max_install_names', str(code), str(source), '-o', str(probe)], check=True)
    subprocess.run([sys.executable, str(Path(__file__).with_name('backplane-macos-relocate.py')), str(bundle)], check=True)
    source.parent.rename(root / 'removed-source')
    subprocess.run([str(probe)], check=True)
    print('PASS relocated executable runs after its original build libraries disappear')
print('MAC_NATIVE_RELOCATION_IDENTITY_PASS')
