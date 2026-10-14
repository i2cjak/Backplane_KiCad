#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check regional zone filling on a copied rigid-flex exemplar.

The fixture places an F.Cu zone over the complete 80 mm board outline.  The
sample's flex stack has no F.Cu layer, so the fork's zone filler must emit two
conductors (the left and right rigid islands) with a gap over the flex bridge.
The check uses only the standard library and the Gerber output, so it does not
depend on a CAM Python package.  The interactive PNS router has no headless
CLI entry point; the script records that limitation while exercising the same
board through DRC/refill.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


SOURCE = Path(__file__).resolve().parents[1] / "demos/backplane-structure/rigid-flex-bridge.kicad_pcb"


def run(cli, *args):
    env = os.environ.copy()
    result = subprocess.run([str(cli), *map(str, args)], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180)
    if result.returncode:
        raise AssertionError(f"{cli} {' '.join(map(str, args))}: expected 0, "
                             f"got {result.returncode}\n{result.stdout}")
    return result.stdout


def add_full_board_zone(board):
    """Insert a normal netted zone covering the exemplar's complete outline."""
    text = board.read_text()
    zone = """\t(zone
		(net \"SIGNAL_1\")
		(layer \"F.Cu\")
		(uuid \"00000000-0000-4000-0000-000000000099\")
		(hatch edge 0.5)
		(connect_pads (clearance 0))
		(min_thickness 0.25)
		(fill yes (thermal_gap 0.3) (thermal_bridge_width 0.3))
		(polygon
			(pts (xy 20 20) (xy 100 20) (xy 100 40) (xy 20 40))
		)
	)
"""
    marker = "\t(embedded_fonts no)\n)"
    if text.count(marker) != 1:
        raise AssertionError("board trailer marker was not unique")
    board.write_text(text.replace(marker, zone + marker))


def gerber_region_bounds(path):
    """Read G36/G37 filled contours from a Gerber in millimetres."""
    # The exporter declares %FSLAX46Y46%, so integer coordinates are millionths of a millimetre.
    coordinate = re.compile(r"X(-?\d+)Y(-?\d+)D0[12]\*")
    regions = []
    current = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if line == "G36*":
            if current is not None:
                raise AssertionError(f"nested Gerber region in {path}")
            current = []
            continue
        if line == "G37*":
            if current is None or not current:
                raise AssertionError(f"empty Gerber region in {path}")
            regions.append(current)
            current = None
            continue
        if current is not None:
            match = coordinate.fullmatch(line)
            if match:
                current.append((int(match.group(1)) / 1e6,
                                -int(match.group(2)) / 1e6))
    if current is not None:
        raise AssertionError(f"unterminated Gerber region in {path}")
    return [((min(x for x, _ in points), min(y for _, y in points)),
             (max(x for x, _ in points), max(y for _, y in points)))
            for points in regions]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not SOURCE.with_name(SOURCE.name + ".backplane.json").exists():
        raise AssertionError(f"missing structure companion for {SOURCE}")
    args.output.mkdir(parents=True, exist_ok=True)
    board = args.output / SOURCE.name
    shutil.copy2(SOURCE, board)
    if SOURCE.with_suffix(".kicad_pro").exists():
        shutil.copy2(SOURCE.with_suffix(".kicad_pro"), board.with_suffix(".kicad_pro"))
    shutil.copy2(SOURCE.with_name(SOURCE.name + ".backplane.json"),
                 board.with_name(board.name + ".backplane.json"))
    add_full_board_zone(board)

    # Refill through the fork's DRC path.  This also proves the regional DRC
    # provider accepts the resulting clipped fill (zero violations expected).
    drc = args.output / "drc.json"
    run(args.kicad_cli, "pcb", "drc", board, "--format", "json", "--output", drc,
        "--refill-zones")
    report = json.loads(drc.read_text())
    if report.get("violations"):
        raise AssertionError(f"zone/refill DRC violations: {report['violations']}")

    gerbers = args.output / "gerbers"
    run(args.kicad_cli, "pcb", "export", "gerbers", board, "--output", gerbers,
        "--layers", "F.Cu", "--check-zones")
    candidates = sorted(gerbers.glob("*-F_Cu.gtl"))
    if len(candidates) != 1:
        raise AssertionError(f"expected one F.Cu Gerber, found {candidates}")
    bounds = gerber_region_bounds(candidates[0])
    if len(bounds) < 2:
        raise AssertionError(f"expected left and right rigid fills, got {bounds}")

    left = []
    right = []
    for lower, upper in bounds:
        min_x, min_y = lower
        max_x, max_y = upper
        if min_y < 20 - 1e-6 or max_y > 40 + 1e-6 or max_y <= min_y:
            raise AssertionError(f"fill escaped board Y extent: {(lower, upper)}")
        # A contour crossing 45..75 mm would put F.Cu on the flex stack.
        if min_x < 45.0 and max_x > 45.0:
            raise AssertionError(f"fill crosses left flex boundary: {(lower, upper)}")
        if min_x < 75.0 and max_x > 75.0:
            raise AssertionError(f"fill crosses right flex boundary: {(lower, upper)}")
        if max_x <= 45.0:
            left.append((lower, upper))
        elif min_x >= 75.0:
            right.append((lower, upper))
        else:
            raise AssertionError(f"fill is not in a rigid island: {(lower, upper)}")
    if not left or not right:
        raise AssertionError(f"missing rigid island fill: left={left}, right={right}")

    result = {
        "board": str(board),
        "zone": {"layer": "F.Cu", "outline_mm": [[20, 20], [100, 20],
                                                        [100, 40], [20, 40]],
                 "net": "SIGNAL_1"},
        "gerber": str(candidates[0]),
        "filled_region_bounds_mm": bounds,
        "regional_clip": True,
        "drc_refill": True,
        "drc_violations": len(report.get("violations", [])),
        "router": {"headless_tested": False,
                    "reason": "kicad-cli exposes no interactive PNS router command"},
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS: F.Cu zone clipped to {len(left)} left and {len(right)} right "
          f"rigid fill region(s); evidence at {args.output}")


if __name__ == "__main__":
    main()
