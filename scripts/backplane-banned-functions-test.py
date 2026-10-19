#!/usr/bin/env python3
"""Run the real banned-function checker from a separate build directory."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1]
checker = source / "cmake/CheckBannedFunctions.cmake"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-root", type=Path)
parser.add_argument("--require-cross-drive", action="store_true")
args = parser.parse_args()
# On hosted Windows, checkout is D: and the normal temporary/build drive is C:.
# Keep the fixture on the source drive to reproduce the real failed layout.
with tempfile.TemporaryDirectory(prefix=".banned-functions-", dir=source) as fixture, \
        tempfile.TemporaryDirectory(prefix="banned-functions-build-", dir=args.build_root) as build:
    folder = Path(fixture)
    if args.require_cross_drive:
        assert os.name == "nt" and folder.drive.lower() != Path(build).drive.lower(), \
            "Cross-drive Windows regression requires distinct source and build drives"
    file = folder / "nested" / "space name.cpp"
    file.parent.mkdir()
    cases = [
        ("ordinary source", "void example() {}\n", True),
        ("banned locale format", 'printf("%f", 1.0);\n', False),
        ("explicit format allowance", 'printf("%f", 1.0); //format:allow\n', True),
    ]
    for label, content, success in cases:
        file.write_text(content, encoding="utf-8")
        result = subprocess.run(
            ["cmake", f"-DBANNED_DIRS={folder.as_posix()}", "-P", str(checker)],
            cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30,
        )
        assert (result.returncode == 0) == success, (label, result.stdout)
        if not success:
            assert "Banned format specifier" in result.stdout, result.stdout
        print("ok", label, flush=True)
    print(f"source-drive={folder.drive or '/'} build-drive={Path(build).drive or '/'}")
print("BANNED_FUNCTION_CHECKER_PATH_AND_REFUSAL_PASS")
