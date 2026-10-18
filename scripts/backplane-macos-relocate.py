#!/usr/bin/env python3
"""Make the staged native bundle independent of Homebrew's install paths."""
import argparse
from collections import deque
from pathlib import Path
import shutil
import subprocess


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def macho(path):
    return path.is_file() and not path.is_symlink() and 'Mach-O' in output('file', '-b', str(path))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('bundle', type=Path)
    bundle = ap.parse_args().bundle.resolve(strict=True)
    frameworks = bundle / 'Contents/Frameworks'
    frameworks.mkdir(parents=True, exist_ok=True)
    binaries = [p for p in bundle.rglob('*') if macho(p)]
    queue = deque(binaries)
    done = set()
    while queue:
        binary = queue.popleft()
        if binary in done:
            continue
        done.add(binary)
        # The first line names the input, the rest are dependency install names.
        for line in output('otool', '-L', str(binary)).splitlines()[1:]:
            dep = line.strip().split(' (compatibility version', 1)[0]
            if not dep.startswith(('/opt/homebrew/', '/usr/local/')):
                continue
            source = Path(dep).resolve(strict=True)
            target = frameworks / source.name
            if not target.exists():
                shutil.copy2(source, target)
                target.chmod(0o755)
                queue.append(target)
            elif target.read_bytes() != source.read_bytes() and target not in done:
                raise RuntimeError(f'conflicting dependency basename: {target.name}')
            relative = Path(__import__('os').path.relpath(target, binary.parent)).as_posix()
            subprocess.run(['install_name_tool', '-change', dep, '@loader_path/' + relative, str(binary)], check=True)
        if binary.parent == frameworks and binary.suffix == '.dylib':
            subprocess.run(['install_name_tool', '-id', '@rpath/' + binary.name, str(binary)], check=True)
    # Sign after every Mach-O modification, inside out; no signing identity needed.
    for binary in sorted(done, key=lambda p: len(p.parts), reverse=True):
        subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
    for binary in done:
        for line in output('otool', '-L', str(binary)).splitlines()[1:]:
            dep = line.strip().split(' (compatibility version', 1)[0]
            if dep.startswith(('/opt/homebrew/', '/usr/local/')):
                raise RuntimeError(f'non-relocatable dependency: {binary}: {dep}')
    print(f'checked {len(done)} native objects')


if __name__ == '__main__':
    main()
