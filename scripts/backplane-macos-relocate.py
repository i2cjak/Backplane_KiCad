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


def library_identity(path):
    # CMake's install step already rewrites install names and signs its copies.
    # Those bytes differ from Homebrew; LC_UUID still identifies each linked slice.
    return tuple(sorted(line.split()[1] for line in output('otool', '-l', str(path)).splitlines()
                        if line.strip().startswith('uuid ')))


def require_same_library(source, target):
    source_id = library_identity(source)
    if not source_id or source_id != library_identity(target):
        raise RuntimeError(f'conflicting dependency basename: {target.name}')


def external_dependency(name):
    # The copied CLI can still name the build tree, as well as Homebrew.
    return name.startswith('/') and not name.startswith(('/usr/lib/', '/System/Library/'))


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
            if not external_dependency(dep):
                continue
            source = Path(dep).resolve(strict=True)
            target = frameworks / source.name
            if not target.exists():
                shutil.copy2(source, target)
                target.chmod(0o755)
                queue.append(target)
            else:
                require_same_library(source, target)
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
            if external_dependency(dep):
                raise RuntimeError(f'non-relocatable dependency: {binary}: {dep}')
    print(f'checked {len(done)} native objects')


if __name__ == '__main__':
    main()
