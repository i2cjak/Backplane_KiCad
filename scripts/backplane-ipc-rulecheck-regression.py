#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that harmless headless IPC round-trips do not add ERC or DRC findings.

The fixture is copied into the isolated :class:`IpcSession` temporary
directory.  A schematic value update and a rolled-back PCB move then exercise
the IPC save/reopen paths.  Rule-check reports are compared by violation type,
severity, and the stable item identities in each violation.  Existing fixture
violations are allowed; a new finding is a failure.

Pass ``--stock-cli`` to run the same before/after comparison with an
independent stock 10.0.6 executable.  The stock checker only reads the native
files and never touches a user's project.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def _item_identity(item):
    """Return the most stable identity available in a report item."""
    if not isinstance(item, dict):
        return repr(item)
    for key in ("uuid", "path", "id", "reference", "ref"):
        value = item.get(key)
        if value not in (None, ""):
            if isinstance(value, (dict, list)):
                value = json.dumps(value, sort_keys=True, separators=(",", ":"))
            return f"{key}={value}"
    # ERC reports can describe a global violation without an item UUID.  Its
    # description is the only useful identity in that case.
    return f"description={item.get('description', repr(item))}"


def _violation_key(violation):
    items = violation.get("items")
    if isinstance(items, list):
        identities = tuple(sorted(_item_identity(item) for item in items))
    else:
        identities = (_item_identity(violation),)
    return (
        str(violation.get("type", "")),
        str(violation.get("severity", "")),
        identities,
    )


def _violations(report):
    """Extract report violation records without depending on ERC/DRC nesting."""
    found = []

    def visit(node):
        if isinstance(node, dict):
            if "type" in node and "severity" in node:
                found.append(_violation_key(node))
            for value in node.values():
                visit(value)
        elif isinstance(node, list):
            for value in node:
                visit(value)

    visit(report)
    return Counter(found)


def _report_errors(report):
    """Return explicit report-generation errors, excluding rule violations."""
    errors = []
    for key in ("error", "errors", "fatal_error", "fatal_errors"):
        value = report.get(key) if isinstance(report, dict) else None
        if value in (None, "", [], {}, 0, False):
            continue
        errors.append((key, value))
    return errors


def _run_rulecheck(cli, kind, input_path, output_path, env):
    command = [
        str(cli), kind, "erc" if kind == "sch" else "drc",
        "--format", "json", "--output", str(output_path), "--severity-all",
        str(input_path),
    ]
    result = subprocess.run(command, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=180)
    if result.returncode != 0:
        raise AssertionError(f"{kind} rule check failed ({result.returncode}):\n{result.stdout}")
    if not output_path.is_file():
        raise AssertionError(f"{kind} rule check did not write {output_path}")
    try:
        report = json.loads(output_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise AssertionError(f"invalid {kind} rule-check report {output_path}: {error}") from error
    errors = _report_errors(report)
    if errors:
        raise AssertionError(f"{kind} rule-check report contains errors: {errors}")
    return _violations(report)


def _compare(label, before, after):
    added = after - before
    removed = before - after
    if added:
        details = "\n".join(f"  {count}x {key!r}" for key, count in added.items())
        raise AssertionError(f"{label} added rule-check findings:\n{details}")
    print(f"PASS {label}: {sum(before.values())} before, {sum(after.values())} after; "
          f"{sum(removed.values())} removed")


def _run_pair(cli, api, label, board_path, schematic_path, phase):
    reports = api.root / "rulecheck-reports"
    reports.mkdir(exist_ok=True)
    results = {}
    for kind, path in (("sch", schematic_path), ("pcb", board_path)):
        output = reports / f"{label}-{phase}-{kind}.json"
        results[kind] = _run_rulecheck(cli, kind, path, output, api.env)
    return results


def _edit_and_roundtrip(api, board_path, schematic_path):
    """Apply the harmless schematic edit and dropped PCB edit through IPC."""
    project = api.module("common.commands.project_commands")
    editor = api.module("common.commands.editor_commands")
    types = api.module("common.types.base_types")
    enums = api.module("common.types.enums")
    schematic_types = api.module("schematic.schematic_types")

    document = api.request(
        project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(schematic_path)),
        project.OpenDocumentResponse,
    ).document
    header = types.ItemHeader(document=document)
    symbols = api.request(
        editor.GetItems(header=header, types=[enums.KOT_SCH_SYMBOL]),
        editor.GetItemsResponse,
    )
    assert symbols.items, "rulecheck fixture returned no schematic symbols"
    candidates = []
    for packed in symbols.items:
        candidate = schematic_types.SchematicSymbolInstance()
        assert packed.Unpack(candidate)
        candidates.append(candidate)
    symbol = next((candidate for candidate in candidates
                   if candidate.reference_field.text.text == "R1"), candidates[0])
    original_value = symbol.value_field.text.text
    symbol.value_field.text.text = original_value + " IPC rulecheck"
    transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
    update = editor.UpdateItems(header=header)
    update.items.add().Pack(symbol)
    response = api.request(update, editor.UpdateItemsResponse)
    assert response.updated_items[0].status.code == editor.ISC_OK, response
    api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                 message="Rulecheck regression value update"),
                editor.EndCommitResponse)
    api.request(project.SaveDocument(document=document), Empty)
    api.request(project.CloseDocument(document=document), Empty)
    document = api.request(
        project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(schematic_path)),
        project.OpenDocumentResponse,
    ).document
    header = types.ItemHeader(document=document)
    reopened = api.request(editor.GetItemsById(header=header, items=[symbol.id]),
                            editor.GetItemsResponse)
    assert len(reopened.items) == 1, reopened
    saved_symbol = schematic_types.SchematicSymbolInstance()
    assert reopened.items[0].Unpack(saved_symbol)
    assert saved_symbol.value_field.text.text == original_value + " IPC rulecheck"
    api.request(project.CloseDocument(document=document), Empty)

    document = api.request(
        project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
        project.OpenDocumentResponse,
    ).document
    header = types.ItemHeader(document=document)
    tracks = api.request(editor.GetItems(header=header, types=[enums.KOT_PCB_TRACE]),
                         editor.GetItemsResponse)
    assert tracks.items, "rulecheck fixture returned no PCB tracks"
    track = api.module("board.board_types").Track()
    assert tracks.items[0].Unpack(track)
    original_start = track.start.x_nm
    track.start.x_nm += 1000000
    transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
    update = editor.UpdateItems(header=header)
    update.items.add().Pack(track)
    response = api.request(update, editor.UpdateItemsResponse)
    assert response.updated_items[0].status.code == editor.ISC_OK, response
    api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_DROP,
                                 message="Rulecheck regression PCB rollback"),
                editor.EndCommitResponse)
    unchanged = api.request(editor.GetItemsById(header=header, items=[track.id]),
                             editor.GetItemsResponse)
    assert len(unchanged.items) == 1
    saved_track = api.module("board.board_types").Track()
    assert unchanged.items[0].Unpack(saved_track)
    assert saved_track.start.x_nm == original_start, "PCB rollback changed the track"
    api.request(project.SaveDocument(document=document), Empty)
    api.request(project.CloseDocument(document=document), Empty)
    document = api.request(
        project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
        project.OpenDocumentResponse,
    ).document
    header = types.ItemHeader(document=document)
    reopened = api.request(editor.GetItemsById(header=header, items=[track.id]),
                            editor.GetItemsResponse)
    assert len(reopened.items) == 1
    saved_track = api.module("board.board_types").Track()
    assert reopened.items[0].Unpack(saved_track)
    assert saved_track.start.x_nm == original_start, "PCB rollback did not survive reopen"
    api.request(project.CloseDocument(document=document), Empty)


def main(cli, stock_cli=None, rulecheck_cli=None):
    with IpcSession(cli) as api:
        design = api.copy_project("demos/ecc83", "rulecheck-regression")
        board_path = design / "ecc83-pp.kicad_pcb"
        schematic_path = design / "ecc83-pp.kicad_sch"
        # A developer build can intentionally omit GUI KIFACEs while still
        # providing the headless API server.  ``--rulecheck-cli`` lets a
        # complete sibling KiCad installation inspect the files in that case;
        # release builds use the IPC CLI itself by default.
        checker = rulecheck_cli or cli
        fork_before = _run_pair(checker, api, "fork", board_path, schematic_path, "before")
        stock_before = None
        if stock_cli:
            stock_before = _run_pair(stock_cli, api, "stock", board_path, schematic_path, "before")

        _edit_and_roundtrip(api, board_path, schematic_path)

        fork_after = _run_pair(checker, api, "fork", board_path, schematic_path, "after")
        for kind in ("sch", "pcb"):
            _compare(f"fork {kind.upper()}", fork_before[kind], fork_after[kind])
        if stock_cli:
            stock_after = _run_pair(stock_cli, api, "stock", board_path, schematic_path, "after")
            for kind in ("sch", "pcb"):
                _compare(f"stock {kind.upper()}", stock_before[kind], stock_after[kind])
        print("PASS IPC rule-check regression: harmless edit/save/reopen added no findings")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path)
    parser.add_argument("--rulecheck-cli", type=Path,
                        help="complete kicad-cli used for ERC/DRC when the API build omits KIFACEs")
    args = parser.parse_args()
    # Keep the basename of a launcher symlink (for example ``kicad-cli``);
    # resolving it can turn a dispatcher into ``.kicad-system-wrapper`` and
    # make the launcher's own app-name dispatch fail.
    def executable_path(value):
        path = Path(value).expanduser().absolute()
        if not path.is_file():
            raise SystemExit(f"executable does not exist: {path}")
        return path

    cli = executable_path(args.kicad_cli)
    stock_cli = executable_path(args.stock_cli) if args.stock_cli else None
    rulecheck_cli = executable_path(args.rulecheck_cli) if args.rulecheck_cli else None
    if stock_cli:
        version = subprocess.check_output([str(stock_cli), "--version"], text=True).strip()
        if version != "10.0.6":
            raise SystemExit(f"--stock-cli must be stock 10.0.6, got {version!r}")
    main(cli, stock_cli, rulecheck_cli)
