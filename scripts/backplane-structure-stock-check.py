#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local end-to-end structure regression with stock KiCad and independent CAM/STEP readers.

Dependencies: gerbonara, cadquery-ocp. Run from a dedicated uv environment.
All mutable boards and outputs are copied into --output; source examples are read-only.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct


def run(cli, *args, expected=0, stock=False):
    env = os.environ.copy()
    if stock:
        for key in ("APPDIR", "APPIMAGE", "LD_LIBRARY_PATH", "KICAD_RUN_FROM_BUILD_DIR", "KICAD_KIFACE_HOME"):
            env.pop(key, None)
    result = subprocess.run([str(cli), *map(str, args)], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    if result.returncode != expected:
        raise AssertionError(f"{cli} {' '.join(map(str,args))}: expected {expected}, got {result.returncode}\n{result.stdout}")
    return result.stdout


def step_solids(path):
    from OCP.STEPControl import STEPControl_Reader
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.TopAbs import TopAbs_SOLID
    from OCP.TopExp import TopExp_Explorer
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.Bnd import Bnd_Box
    from OCP.BRepBndLib import BRepBndLib
    reader = STEPControl_Reader()
    assert reader.ReadFile(str(path)) == IFSelect_RetDone
    assert reader.TransferRoots() > 0
    shape = reader.OneShape()
    assert not shape.IsNull()
    explorer = TopExp_Explorer(shape, TopAbs_SOLID)
    solids = []
    while explorer.More():
        solid = explorer.Current()
        assert BRepCheck_Analyzer(solid).IsValid(), "Invalid STEP solid"
        bounds = Bnd_Box()
        BRepBndLib.Add_s(solid, bounds)
        solids.append((*bounds.CornerMin().Coord(), *bounds.CornerMax().Coord()))
        explorer.Next()
    assert solids, "STEP contains no solids"
    return solids



def assert_open_drills(path, points):
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TopAbs import TopAbs_SOLID, TopAbs_IN
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS
    from OCP.gp import gp_Pnt
    from OCP.BRepClass3d import BRepClass3d_SolidClassifier
    reader = STEPControl_Reader()
    reader.ReadFile(str(path))
    reader.TransferRoots()
    explorer = TopExp_Explorer(reader.OneShape(), TopAbs_SOLID)
    while explorer.More():
        solid = TopoDS.Solid(explorer.Current())
        for point in points:
            classifier = BRepClass3d_SolidClassifier(solid, gp_Pnt(*point), 1e-6)
            assert classifier.State() != TopAbs_IN, f"Drill filled with material at {point}"
        explorer.Next()

def gerber_bounds(path):
    from gerbonara import GerberFile
    from gerbonara.utils import MM
    gerber = GerberFile.open(path)
    assert gerber.objects, f"Empty Gerber {path}"
    bounds = gerber.bounding_box(unit=MM)
    assert bounds is not None
    return bounds


def board_structure(metadata):
    """Return the structure object from a companion metadata document."""
    return metadata["extensions" if metadata["version"] == 2 else "items"]["root"]["board_structure"]


def write_companion(path, metadata):
    path.write_text(json.dumps(metadata, indent=2) + "\n")


def drc_descriptions(cli, board, output):
    """Run fork DRC and return its report descriptions, requiring violations."""
    run(cli, "pcb", "drc", board, "--format", "json", "--output", output,
        "--exit-code-violations", expected=5)
    report = json.loads(output.read_text())
    return [item.get("description", item.get("message", ""))
            for item in report.get("violations", [])]


def insert_bend_via(board):
    """Add a valid top-level via at the sample bend axis.

    The source examples intentionally contain no vias.  Inserting this small
    S-expression keeps the fixture independent of the stock Python bindings
    while exercising the native board loader and the DRC provider together.
    """
    text = board.read_text()
    via = """\t(via
\t\t(at 55 30)
\t\t(size 1)
\t\t(drill 0.5)
\t\t(layers "F.Cu" "B.Cu")
\t\t(uuid "00000000-0000-4000-8000-000000000001")
\t)
"""
    marker = "\t(embedded_fonts no)"
    index = text.rfind(marker)
    assert index >= 0, f"Could not find board trailer in {board}"
    board.write_text(text[:index] + via + text[index:])


def move_pad_to_bend(board):
    """Move one existing PTH terminal onto the sample bend strip."""
    text = board.read_text()
    marker = "\t\t(at 95 25)"
    assert text.count(marker) == 1, f"Expected one terminal at 95,25 in {board}"
    board.write_text(text.replace(marker, "\t\t(at 55 30)", 1))


def run_negative_drc_fixtures(cli, source_board, source_companion, metadata,
                               destination):
    """Exercise structure DRC against independent malformed board copies."""
    destination.mkdir(parents=True, exist_ok=True)

    def fixture(name):
        root = destination / name
        root.mkdir()
        board = root / source_board.name
        companion = root / source_companion.name
        shutil.copy2(source_board, board)
        fixture_metadata = copy.deepcopy(metadata)
        write_companion(companion, fixture_metadata)
        return board, companion, fixture_metadata, board_structure(fixture_metadata)

    # A route on In1.Cu crosses the lower part of the flex bridge.  Removing
    # that part of the physical region must report both the uncovered board
    # area and copper outside the regional layer domain.
    board, companion, fixture_metadata, structure = fixture("gap-and-absent-copper")
    for region in structure["regions"]:
        if region["id"] == "flex":
            for point in region["shape"]["outline"]:
                if point[1] == 40000000:
                    point[1] = 30000000
    write_companion(companion, fixture_metadata)
    descriptions = drc_descriptions(cli, board, board.parent / "drc.json")
    assert any("must cover the board outline" in text for text in descriptions), descriptions
    assert any("Copper extends outside regional layer" in text for text in descriptions), descriptions

    # Regions with positive-area overlap must be rejected while preserving
    # shared-edge behavior (the normal exemplar remains a zero-violation case).
    board, companion, fixture_metadata, structure = fixture("overlapping-regions")
    if len(structure["regions"]) > 1:
        for region in structure["regions"]:
            if region["id"] == "flex":
                for point in region["shape"]["outline"]:
                    if point[0] == 75000000:
                        point[0] = 80000000
    else:
        duplicate = copy.deepcopy(structure["regions"][0])
        duplicate["id"] = "overlap-region"
        duplicate["name"] = "Intentional overlap"
        structure["regions"].append(duplicate)
    write_companion(companion, fixture_metadata)
    descriptions = drc_descriptions(cli, board, board.parent / "drc.json")
    assert any("Physical region overlaps another region" in text for text in descriptions), descriptions

    # A hole is a genuine absent physical domain, even though it is fully
    # enclosed by the outline.  Place it over the lower route and require both
    # the board coverage and copper-domain diagnostics.
    board, companion, fixture_metadata, structure = fixture("hole-through-copper")
    for region in structure["regions"]:
        if region["id"] == "flex":
            region["shape"]["holes"] = [[[50000000, 32000000],
                                           [60000000, 32000000],
                                           [60000000, 38000000],
                                           [50000000, 38000000]]]
    write_companion(companion, fixture_metadata)
    descriptions = drc_descriptions(cli, board, board.parent / "drc.json")
    assert any("must cover the board outline" in text for text in descriptions), descriptions
    assert any("Copper extends outside regional layer" in text for text in descriptions), descriptions

    # PTH pads and vias span the copper layer set.  A via at the active bend
    # must be reported even when its surrounding region is otherwise valid.
    board, companion, fixture_metadata, structure = fixture("via-in-bend")
    insert_bend_via(board)
    descriptions = drc_descriptions(cli, board, board.parent / "drc.json")
    assert any("Pad or via intersects an active bend strip" in text for text in descriptions), descriptions

    board, companion, fixture_metadata, structure = fixture("pad-in-bend")
    move_pad_to_bend(board)
    descriptions = drc_descriptions(cli, board, board.parent / "drc.json")
    assert any("Pad or via intersects an active bend strip" in text for text in descriptions), descriptions

    return {
        "gap_and_absent_copper": True,
        "overlapping_regions": True,
        "hole_through_copper": True,
        "via_in_bend": True,
        "pad_in_bend": True,
    }





def check_folded_datum(cli, original, output):
    """Moving the common stack datum must translate every folded solid equally."""
    import math
    fixture = output / "neutral-datum"
    fixture.mkdir(exist_ok=True)
    board = fixture / original.name
    shutil.copy2(original, board)
    metadata = json.loads(Path(str(original)+".backplane.json").read_text())
    structure = board_structure(metadata)
    structure["bends"][0]["neutral_axis_offset_mm"] = .04
    results = []
    for shift in (0.0, 1.5):
        data = copy.deepcopy(metadata)
        for stack in board_structure(data)["stacks"]:
            for layer in stack["layers"]:
                layer["z_offset_nm"] += round(shift * 1e6)
        write_companion(Path(str(board)+".backplane.json"), data)
        step = fixture / f"datum-{shift}.step"
        run(cli, "pcb", "export", "step", board, "--output", step, "--force",
            "--no-components", "--folded")
        solids = step_solids(step)
        results.append((tuple(min(b[k] for b in solids) for k in range(3)),
                        tuple(max(b[k+3] for b in solids) for k in range(3))))
        radius = 2.0 + .04
        neutral_z = shift + .075 - .04
        drill_z = neutral_z + radius + 40 - radius * math.pi / 2
        assert_open_drills(step, [(57.075, -25, drill_z), (57.075, -35, drill_z)])
    for side in range(2):
        for axis in range(3):
            expected_delta = 1.5 if axis == 2 else 0.0
            assert abs(results[1][side][axis] - results[0][side][axis] - expected_delta) < 1e-5, results


def check_coverlay_opening(cli, original, output):
    """Clear coverlay windows must appear in both CAM and mechanical exports."""
    from gerbonara import GerberFile
    fixture = output / "coverlay-opening"
    fixture.mkdir(exist_ok=True)
    board = fixture / original.name
    shutil.copy2(original, board)
    metadata = json.loads(Path(str(original)+".backplane.json").read_text())
    structure = board_structure(metadata)
    structure["coverlays"][0]["openings"] = [{
        "outline": [[50000000,28000000],[52000000,28000000],
                    [52000000,32000000],[50000000,32000000]], "holes": []}]
    write_companion(Path(str(board)+".backplane.json"), metadata)
    fab = fixture / "gerbers"
    run(cli, "pcb", "structure", board, "--export-fabrication", "--output", fab)
    manifest = json.loads((fab/"board-construction.json").read_text())
    coverlay = next(entry for entry in manifest["files"] if "coverlay" in entry["role"])
    gerber = GerberFile.open(fab / coverlay["path"])
    assert any(not obj.polarity_dark for obj in gerber.objects), "Coverlay opening lost clear polarity"
    step = fixture / "opening.step"
    run(cli, "pcb", "export", "step", board, "--output", step, "--force", "--no-components")
    step_solids(step)
    assert_open_drills(step, [(51, -30, .09), (51, -30, .115)])


def check_folded_component(cli, original, output):
    """A non-cubic model must rotate with the moving rigid island."""
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox
    from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
    from OCP.IFSelect import IFSelect_RetDone
    model = output / "orientation-box.step"
    writer = STEPControl_Writer()
    writer.Transfer(BRepPrimAPI_MakeBox(2.0, 1.0, 0.2).Shape(), STEPControl_AsIs)
    assert writer.Write(str(model)) == IFSelect_RetDone
    board = output / "component-orientation.kicad_pcb"
    # Stock bindings create a real component model attachment, independent of
    # the fork's metadata parser and exporter implementation.
    script = """import pcbnew as p, sys
b = p.LoadBoard(sys.argv[1])
f = next(f for f in b.GetFootprints() if f.GetReference() == 'J3')
m = p.FP_3DMODEL()
m.m_Filename = sys.argv[3]
f.Models().push_back(m)
p.SaveBoard(sys.argv[2], b)
"""
    env = os.environ.copy()
    for key in ("APPDIR", "APPIMAGE", "LD_LIBRARY_PATH"):
        env.pop(key, None)
    subprocess.run(["/usr/bin/python3", "-c", script, str(original), str(board),
                    str(model.resolve())], env=env, check=True, timeout=60)
    shutil.copy2(Path(str(original)+".backplane.json"), Path(str(board)+".backplane.json"))
    folded = output / "component-folded.step"
    run(cli, "pcb", "export", "step", board, "--output", folded, "--force", "--folded")
    boxes = step_solids(folded)
    component = next((b for b in boxes if abs(b[3]-b[0]-0.2) < 1e-5
                      and abs(b[4]-b[1]-1.0) < 1e-5
                      and abs(b[5]-b[2]-2.0) < 1e-5), None)
    assert component is not None, "Component did not rotate 90 degrees"
    assert abs(component[1] + 25.0) < 1e-5, "Fold mirrored the component across its local Y axis"
    radius = 2.0 + .15 / 2
    expected_z = radius + 40 - radius * 3.141592653589793 / 2
    assert abs(component[2] - expected_z) < 1e-5, "Component used the wrong bend radius"
    flat = output / "component-flat.step"
    run(cli, "pcb", "export", "step", board, "--output", flat, "--force")
    assert any(abs(b[3]-b[0]-2.0) < 1e-5 and abs(b[5]-b[2]-.2) < 1e-5
               and .8 <= b[2] <= .9 for b in step_solids(flat)), "Component missed its regional surface"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--folded", action="store_true", help="Also require --folded STEP exports")
    args = parser.parse_args()
    assert run(args.stock_cli,"version",stock=True).strip().startswith("10.0.")
    source = Path(__file__).resolve().parents[1]/"demos/backplane-structure"
    args.output.mkdir(parents=True,exist_ok=True)
    results = {}
    for original in sorted(source.glob("*.kicad_pcb")):
        dest = args.output/original.stem
        dest.mkdir(exist_ok=True)
        board = dest/original.name
        companion = Path(str(board)+".backplane.json")
        shutil.copy2(original,board)
        if original.with_suffix(".kicad_pro").exists():
            shutil.copy2(original.with_suffix(".kicad_pro"), board.with_suffix(".kicad_pro"))
        shutil.copy2(Path(str(original)+".backplane.json"),companion)
        metadata = json.loads(companion.read_text())
        structure = board_structure(metadata)
        structure["future_extension"] = {"preserve": True}
        # These fields belong to a newer producer.  The fork must preserve
        # them when a known object is rewritten, including identified array
        # entries nested below a stack.
        stack_with_material = next((item for item in structure["stacks"]
                                    if item.get("materials")), None)
        if stack_with_material:
            stack_with_material["future_stack_field"] = {"preserve": "stack"}
            stack_with_material["materials"][0]["future_material_field"] = "keep"
        write_companion(companion, metadata)
        run(args.kicad_cli,"pcb","structure",board)
        run(args.kicad_cli,"pcb","upgrade","--force",board)
        after = board_structure(json.loads(companion.read_text()))
        assert after["future_extension"] == {"preserve": True}
        if stack_with_material:
            after_stack = next(item for item in after["stacks"]
                               if item["id"] == stack_with_material["id"])
            assert after_stack["future_stack_field"] == {"preserve": "stack"}
            assert after_stack["materials"][0]["future_material_field"] == "keep"
        for key in ("regions","stacks","bends","stiffeners","coverlays"):
            assert len(after[key]) == len(structure[key]), f"Lost {key} on stock/fork round-trip"
        metadata = json.loads(companion.read_text())
        structure = board_structure(metadata)
        drc = dest/"drc.json"
        run(args.kicad_cli,"pcb","drc",board,"--format","json","--output",drc,"--exit-code-violations")
        v10_dir = args.output / (original.stem + "-v10")
        run(args.kicad_cli, "project", "downgrade", "--target", "10.0", "--force",
            "-o", v10_dir, board.with_suffix(".kicad_pro"))
        v10_board = v10_dir / board.name
        assert v10_board.is_file()
        assert Path(str(v10_board) + ".backplane.json").read_bytes() == companion.read_bytes()
        run(args.stock_cli, "pcb", "upgrade", "--force", v10_board, stock=True)
        run(args.stock_cli,"pcb","drc",v10_board,"--format","json","--output",dest/"stock-drc.json","--exit-code-violations", stock=True)
        negative = run_negative_drc_fixtures(args.kicad_cli, board, companion,
                                              metadata, dest / "negative")
        fab = dest/"gerbers"
        run(args.kicad_cli,"pcb","export","gerbers",board,"--output",fab,"--layers","F.Cu,B.Cu,In1.Cu,In2.Cu,Edge.Cuts")
        run(args.kicad_cli,"pcb","export","drill",board,"--output",fab)
        manifest = json.loads((fab/"board-construction.json").read_text())
        assert manifest["state"] == "flat"
        expected_count = sum(len(structure[key]) for key in ("regions","stiffeners","coverlays"))
        assert len(manifest["files"]) >= expected_count
        bounds = {entry["path"]:gerber_bounds(fab/entry["path"]) for entry in manifest["files"]}
        for entry in manifest["files"]:
            b = bounds[entry["path"]]
            assert b[1][0] > b[0][0] and b[1][1] > b[0][1], b
            if entry["role"] == "stack-region-material-area":
                shape = next(r["shape"] for r in structure["regions"] if r["id"] == entry["object_id"])
                xs, ys = zip(*shape["outline"])
                expected = ((min(xs)/1e6, -max(ys)/1e6), (max(xs)/1e6, -min(ys)/1e6))
                assert all(abs(b[i][j]-expected[i][j]) < 1e-6 for i in (0,1) for j in (0,1)), (b,expected)
        step = dest/"flat.step"
        run(args.kicad_cli,"pcb","export","step",board,"--output",step,"--force","--no-components")
        solids = step_solids(step)
        assert_open_drills(step, [(x, -y, z) for x in (25, 95) for y in (25, 35)
                                 for z in (-.79, -.4, -.2, -.07, -.05, 0, .05, .07, .4, .79)])
        expected_thicknesses = [
            sum(m["thickness_nm"] for m in stack["materials"]) / 1e6
            for stack in structure["stacks"]
        ]
        for thickness in expected_thicknesses:
            assert any(abs((b[5]-b[2])-thickness) < 1e-5 for b in solids), (thickness, solids)
        assert len(solids) >= len(structure["regions"])+len(structure["stiffeners"])
        glb = dest / "flat.glb"
        run(args.kicad_cli, "pcb", "export", "glb", board, "--output", glb,
            "--force", "--no-components")
        data = glb.read_bytes()
        magic, version, length = struct.unpack_from("<4sII", data)
        assert (magic, version, length) == (b"glTF", 2, len(data))
        json_length, chunk_type = struct.unpack_from("<I4s", data, 12)
        assert chunk_type == b"JSON"
        gltf = json.loads(data[20:20+json_length])
        assert gltf.get("meshes"), "GLB has no meshes"
        assert all(mesh.get("primitives") for mesh in gltf["meshes"])
        entry = {"flat_solids":solids,"gerber_bounds_mm":bounds,"round_trip":True,
                 "drc":True,"negative_fixtures":negative,"glb_meshes":len(gltf["meshes"])}
        if args.folded:
            folded = dest/"folded.step"
            run(args.kicad_cli,"pcb","export","step",board,"--output",folded,"--force","--no-components","--folded")
            folded_solids = step_solids(folded)
            z_span = max(s[5] for s in folded_solids)-min(s[2] for s in folded_solids)
            minimum_span = 20.0 if len(structure["bends"]) == 1 else 5.0
            assert z_span > minimum_span, f"Folded export appears flat: z span {z_span}"
            if original.stem == "two-bend-u-fold":
                # Two 90 degree hinges return the terminal island to a
                # horizontal plane, above the fixed island.
                radius = 2.0 + 0.15 / 2
                terminal_z = 15.0 - radius * 3.141592653589793 / 2 + 2 * radius
                assert any(abs(b[2] - (terminal_z - .8)) < 1e-5
                           and abs(b[5] - (terminal_z + .8)) < 1e-5
                           for b in folded_solids), "U-fold terminal height is wrong"
            entry["folded_solids"] = folded_solids
        # Invalid material reference must fail through the actual CLI, not just a helper.
        bad = copy.deepcopy(json.loads(companion.read_text()))
        board_structure(bad)["regions"][0]["stack_id"] = "missing"
        companion.write_text(json.dumps(bad))
        run(args.kicad_cli,"pcb","structure",board,expected=5)
        companion.write_text(json.dumps(metadata,indent=2)+"\n")
        results[original.stem] = entry
    check_coverlay_opening(args.kicad_cli, source / "reinforced-flex.kicad_pcb", args.output)
    if args.folded:
        check_folded_datum(args.kicad_cli, source / "reinforced-flex.kicad_pcb", args.output)
        check_folded_component(args.kicad_cli, source / "rigid-flex-bridge.kicad_pcb", args.output)
    (args.output/"results.json").write_text(json.dumps(results,indent=2)+"\n")
    print(f"PASS: {len(results)} exemplar boards; reports at {args.output}")


if __name__ == "__main__":
    main()
