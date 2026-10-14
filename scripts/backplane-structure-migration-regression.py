#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless migration of immutable KiCad 10.0.6 structure fixtures to master.

Uses IpcSession's kicad-cli api-server; no GUI, pcbnew Python, or builds.
The source fixtures contain structure only. Additional copies receive v1 item
metadata to test native property/ending migration, precedence and deletion.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from google.protobuf.empty_pb2 import Empty
from backplane_ipc_test_support import IpcSession
from backplane_structure_ipc import BackplaneIpc

# Populated from snapshot 1747f645995956b918a87e4633e5d1aadb0a65c2.
FIXTURE_HASHES = {
    "reinforced-flex.kicad_pcb": "aa2ac0c958d274ae569e6b15c660def1435c4c30d5c643afc820515dcbeeeebb",
    "reinforced-flex.kicad_pcb.backplane.json": "6640247db21c352685d37b4cd8de8c2ac2a36437a2d24db13d201158eb2dee1a",
    "reinforced-flex.kicad_pro": "4e5b06023fbf08244ba00f0eac3633b07eb006b36731323d96b77303b41c1354",
    "rigid-flex-bridge.kicad_pcb": "ab127eb109a5aaf82bcb0dcec1811653157fa6fb68e093d4ecd206be2697d49b",
    "rigid-flex-bridge.kicad_pcb.backplane.json": "967b1ee4655384fa1c9d4d07ea1833c456e43af961e5f9aae35eb3e16d7fb166",
    "rigid-flex-bridge.kicad_pro": "42478cecda82e31e978ba16032b615e0754c98575795df4a587d1bf5d69194de",
    "two-bend-u-fold.kicad_pcb": "052594c6c6dd305c56d5c35078b320dde8204e454dad250ffbaa02d93abb4d77",
    "two-bend-u-fold.kicad_pcb.backplane.json": "cb543c2a575f6dbc17601823dd01bd6c026c71ce978cba077c884065deee9daa",
    "two-bend-u-fold.kicad_pro": "ee68a8e12bcc33eedafe011e29350b9a760c62f96e15fb0b96e767c8dd20bdac"
}
SHAPE_ID = "ab000000-0000-4000-8000-000000000001"
PROPERTY = "Backplane migration probe"
ENDING = {"style": 1, "length": 700000, "width": 400000,
          "stroke_width": 100000, "stroke_style": 0,
          "stroke_color": [0.2, 0.3, 0.4, 1.0]}


def companion(path):
    return Path(str(path) + ".backplane.json")


def assert_subset(original, loaded):
    """Canonical serialization may add default fields, but must retain inputs."""
    if isinstance(original, dict):
        for key, value in original.items():
            assert key in loaded, key
            assert_subset(value, loaded[key])
    elif isinstance(original, list):
        assert len(original) == len(loaded), (original, loaded)
        for expected, actual in zip(original, loaded):
            assert_subset(expected, actual)
    else:
        assert original == loaded, (original, loaded)


def verify_v2(path, structure):
    value = json.loads(companion(path).read_text())
    assert value == {"format": "backplane-kicad-metadata", "version": 2,
                     "extensions": {"root": {"board_structure": structure}}}, value
    native = path.read_text()
    return native


def inject_legacy_items(path):
    """Use only stock 10.0.6 native tokens; fork data stays in the v1 companion."""
    text = path.read_text()
    at = text.rfind(")")
    assert at >= 0
    segment = (f'  (gr_line (start 22 22) (end 24 22)\n'
               f'    (stroke (width 0.1) (type solid)) (layer "F.SilkS")\n'
               f'    (uuid "{SHAPE_ID}"))\n')
    path.write_text(text[:at] + segment + text[at:])
    data = json.loads(companion(path).read_text())
    assert data["version"] == 1
    data["items"][SHAPE_ID] = {
        "custom_properties": {PROPERTY: "legacy-v1"},
        "start_ending": ENDING,
        "end_ending": {**ENDING, "style": 4},
    }
    companion(path).write_text(json.dumps(data, indent=2) + "\n")


def fetch_probe(session, document):
    editor = session.module("common.commands.editor_commands")
    types = session.module("common.types.base_types")
    board_types = session.module("board.board_types")
    result = session.request(editor.GetItemsById(
        header=types.ItemHeader(document=document), items=[types.KIID(value=SHAPE_ID)]),
        editor.GetItemsResponse)
    assert result.status == types.IRS_OK and len(result.items) == 1, result
    shape = board_types.BoardGraphicShape()
    assert result.items[0].Unpack(shape)
    assert {p.key: p.value for p in shape.custom_properties}[PROPERTY] == "legacy-v1", shape
    assert shape.shape.start_ending.style == types.LES_ARROW, shape
    assert shape.shape.end_ending.style == types.LES_ARROW_OPEN, shape
    for ending in (shape.shape.start_ending, shape.shape.end_ending):
        assert ending.length.value_nm == ENDING["length"], ending
        assert ending.width.value_nm == ENDING["width"], ending
        assert ending.stroke.width.value_nm == ENDING["stroke_width"], ending
    return shape


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    source_root = Path(__file__).resolve().parents[1]
    source = source_root / "demos/backplane-structure"
    assert FIXTURE_HASHES, "snapshot hashes are missing"
    for name, expected in FIXTURE_HASHES.items():
        assert hashlib.sha256((source / name).read_bytes()).hexdigest() == expected, name
    version_header = source_root / "pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h"
    version = int(re.search(r"^#define SEXPR_BOARD_FILE_VERSION\s+(\d+)",
                            version_header.read_text(), re.M)[1])
    with IpcSession(args.kicad_cli) as session:
        bp = BackplaneIpc(session)
        project, types = bp.project, bp.types
        work = session.copy_project("demos/backplane-structure", "originals")
        probes = session.copy_project("demos/backplane-structure", "legacy-item-probes")
        for board_path in sorted(work.glob("*.kicad_pcb")):
            original = json.loads(companion(board_path).read_text())["items"]["root"]["board_structure"]
            for path, probe in ((board_path, False), (probes / board_path.name, True)):
                if probe:
                    inject_legacy_items(path)
                before_bytes = path.read_bytes(), companion(path).read_bytes()
                document = session.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(path)),
                                           project.OpenDocumentResponse).document
                result = bp.json_get("GetBoardStructure", "BoardStructureResponse", document)
                assert result.valid, result
                structure = json.loads(result.json)
                assert_subset(original, structure)
                if probe:
                    fetch_probe(session, document)
                assert before_bytes == (path.read_bytes(), companion(path).read_bytes()), "load wrote files"
                session.request(project.SaveDocument(document=document), Empty)
                native = verify_v2(path, structure)
                assert re.search(r"\(version\s+(\d+)\)", native)[1] == str(version), path
                assert "(board_structure" not in native
                if probe:
                    assert "(custom_property" in native and PROPERTY in native, path
                    assert "(start_shape" in native and "(end_shape" in native, path
                    # SaveCopyOfDocument must serialize the live extension at its destination.
                    copy_path = session.root / ("copy-" + path.name)
                    editor = session.module("common.commands.editor_commands")
                    session.request(editor.SaveCopyOfDocument(
                        document=document, path=str(copy_path)), Empty)
                    verify_v2(copy_path, structure)
                bp.close(document)
                # Downgrade must preserve the v2 companion outside the source project.
                export_dir = session.root / "v10-exports" / (path.stem + ("-probe" if probe else ""))
                command = [str(session.cli), "project", "downgrade", "--target", "10.0", "--force",
                           "-o", str(export_dir), str(path.with_suffix(".kicad_pro"))]
                exported = subprocess.run(command, env=session.env, text=True,
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
                assert exported.returncode == 0, exported.stdout
                export = export_dir / path.name
                assert export.is_file(), exported.stdout
                assert companion(export).read_bytes() == companion(path).read_bytes()
                # A v1 file left beside already migrated native fields must not override them.
                if probe:
                    legacy = {"format": "backplane-kicad-metadata", "version": 1,
                              "items": {"root": {"board_structure": structure}, SHAPE_ID: {
                                  "custom_properties": {PROPERTY: "stale-v1"},
                                  "start_ending": {**ENDING, "style": 2},
                                  "end_ending": {**ENDING, "style": 3}}}}
                    companion(path).write_text(json.dumps(legacy) + "\n")
                document = session.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(path)),
                                           project.OpenDocumentResponse).document
                assert json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse", document).json) == structure
                if probe:
                    fetch_probe(session, document)
                    session.request(project.SaveDocument(document=document), Empty)
                    verify_v2(path, structure)
                    # Removing the last extension deletes the companion, keeping native fields.
                    bp.json_set("SetBoardStructure", "BoardStructureResponse", document, "{}")
                    session.request(project.SaveDocument(document=document), Empty)
                    assert not companion(path).exists(), path
                    bp.close(document)
                    document = session.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(path)),
                                               project.OpenDocumentResponse).document
                    fetch_probe(session, document)
                    assert not json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse", document).json)["regions"]
                bp.close(document)
            print(f"PASS {board_path.stem}: v1 -> native master + v2; copy/downgrade/reopen; item precedence/deletion",
                  flush=True)
    for name, expected in FIXTURE_HASHES.items():
        assert hashlib.sha256((source / name).read_bytes()).hexdigest() == expected, name


if __name__ == "__main__":
    main()
