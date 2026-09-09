#!/usr/bin/env python3
"""Verify schematic IPC distances remain physical nanometres across save/reopen.

The native schematic editor stores integer internal units while the IPC schema
stores distances in nanometres.  A one millimetre endpoint move catches either
side silently treating the other representation as native units.
"""

import argparse
import re
from pathlib import Path
import subprocess

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


ONE_MM_NM = 1_000_000


def native_wire_delta(path: Path, item_id: str):
    """Read a wire's millimetre coordinates directly from the native file."""
    text = path.read_text(encoding="utf-8")
    marker = f'(uuid "{item_id}")'
    marker_at = text.find(marker)
    assert marker_at >= 0, f"native file has no wire UUID {item_id}"
    wire_at = text.rfind("\n\t(wire", 0, marker_at)
    assert wire_at >= 0, f"UUID {item_id} is not inside a native wire"
    block_end = text.find("\n\t)", marker_at)
    assert block_end >= 0, f"unterminated native wire {item_id}"
    block = text[wire_at:block_end]
    points = [
        (float(x), float(y))
        for x, y in re.findall(r"\(xy\s+([-+0-9.]+)\s+([-+0-9.]+)\)", block)
    ]
    assert len(points) >= 2, f"native wire {item_id} has fewer than two points"
    return points[0], points[-1]


def native_at(path: Path, token: str, item_id: str):
    """Read an item's native `(at x y ...)` position in millimetres."""
    text = path.read_text(encoding="utf-8")
    marker_at = text.find(f'(uuid "{item_id}")')
    assert marker_at >= 0, f"native file has no {token} UUID {item_id}"
    item_at = text.rfind(f"\n\t({token}", 0, marker_at)
    assert item_at >= 0, f"UUID {item_id} is not inside a native {token}"
    block = text[item_at:marker_at]
    match = re.search(r"\(at\s+([-+0-9.]+)\s+([-+0-9.]+)", block)
    assert match, f"native {token} {item_id} has no position"
    return float(match.group(1)), float(match.group(2))


def main(cli, stock_cli=None):
    with IpcSession(cli) as api:
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        schematic = api.module("schematic.schematic_types")
        commands = api.module("schematic.schematic_commands")

        design = api.copy_project("demos/ecc83", "schematic-units")
        path = design / "ecc83-pp.kicad_sch"
        document = api.request(
            project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(path)),
            project.OpenDocumentResponse,
        ).document
        header = types.ItemHeader(document=document)

        response = api.request(
            editor.GetItems(header=header, types=[enums.KOT_SCH_LINE]),
            editor.GetItemsResponse,
        )
        assert response.items, "schematic fixture returned no line segments"
        line = schematic.SchematicLine()
        assert response.items[0].Unpack(line)

        original_start = (line.start.x_nm, line.start.y_nm)
        native_before = native_wire_delta(path, line.id.value)
        native_before_delta = (
            native_before[1][0] - native_before[0][0],
            native_before[1][1] - native_before[0][1],
        )
        assert abs(native_before_delta[0] - (line.end.x_nm - line.start.x_nm) / ONE_MM_NM) < 1e-6
        assert abs(native_before_delta[1] - (line.end.y_nm - line.start.y_nm) / ONE_MM_NM) < 1e-6
        line.end.x_nm = line.start.x_nm + ONE_MM_NM
        expected_delta = (ONE_MM_NM, line.end.y_nm - line.start.y_nm)

        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        update = editor.UpdateItems(header=header)
        update.items.add().Pack(line)
        result = api.request(update, editor.UpdateItemsResponse)
        assert result.updated_items[0].status.code == editor.ISC_OK, result
        api.request(
            editor.EndCommit(
                header=header,
                id=transaction.id,
                action=editor.CMA_COMMIT,
                message="Schematic one millimetre unit regression",
            ),
            editor.EndCommitResponse,
        )

        def read_line():
            current_response = api.request(
                editor.GetItemsById(header=header, items=[line.id]),
                editor.GetItemsResponse,
            )
            assert len(current_response.items) == 1, current_response
            current = schematic.SchematicLine()
            assert current_response.items[0].Unpack(current)
            return current

        committed = read_line()
        assert (committed.start.x_nm, committed.start.y_nm) == original_start
        assert (committed.end.x_nm - committed.start.x_nm,
                committed.end.y_nm - committed.start.y_nm) == expected_delta

        api.request(project.SaveDocument(document=document), Empty)
        native_after = native_wire_delta(path, line.id.value)
        native_after_delta = (
            native_after[1][0] - native_after[0][0],
            native_after[1][1] - native_after[0][1],
        )
        assert abs(native_after_delta[0] - 1.0) < 1e-6, native_after_delta
        assert abs(native_after_delta[1] - expected_delta[1] / ONE_MM_NM) < 1e-6, native_after_delta
        api.request(project.CloseDocument(document=document), Empty)
        if stock_cli:
            stock = subprocess.run([str(stock_cli), "sch", "upgrade", "--force", str(path)],
                                   env=api.env, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=60)
            assert stock.returncode == 0, stock.stdout
        document = api.request(
            project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(path)),
            project.OpenDocumentResponse,
        ).document
        header = types.ItemHeader(document=document)
        reopened_response = api.request(
            editor.GetItemsById(header=header, items=[line.id]),
            editor.GetItemsResponse,
        )
        assert len(reopened_response.items) == 1, reopened_response
        reopened = schematic.SchematicLine()
        assert reopened_response.items[0].Unpack(reopened)
        assert (reopened.end.x_nm - reopened.start.x_nm,
                reopened.end.y_nm - reopened.start.y_nm) == expected_delta

        # Symbol definitions contain both pin lengths and graphic geometry.  R1 is
        # deliberately used because the fixture's native definition is 1.016 mm wide,
        # 5.08 mm tall, with 1.27 mm pins; these assertions catch nm/IU confusion in
        # nested definition items even when the top-level symbol position is correct.
        symbol_response = api.request(
            editor.GetItems(header=header, types=[enums.KOT_SCH_SYMBOL]),
            editor.GetItemsResponse,
        )
        symbol = None
        for item in symbol_response.items:
            candidate = schematic.SchematicSymbolInstance()
            if item.Unpack(candidate) and candidate.reference_field.text.text == "R1":
                symbol = candidate
                break
        assert symbol is not None, "fixture has no R1 symbol"
        native_symbol = native_at(path, "symbol", symbol.id.value)
        assert abs(native_symbol[0] - symbol.position.x_nm / ONE_MM_NM) < 1e-6
        assert abs(native_symbol[1] - symbol.position.y_nm / ONE_MM_NM) < 1e-6

        pin_lengths = []
        rectangle_sizes = []
        for child in symbol.definition.items:
            pin = schematic.SchematicPin()
            if child.item.Unpack(pin):
                pin_lengths.append(pin.length.value_nm)

            graphic = schematic.SchematicGraphicShape()
            if child.item.Unpack(graphic) and graphic.shape.HasField("rectangle"):
                rectangle = graphic.shape.rectangle
                rectangle_sizes.append(
                    (abs(rectangle.top_left.x_nm - rectangle.bottom_right.x_nm),
                     abs(rectangle.top_left.y_nm - rectangle.bottom_right.y_nm))
                )

        assert any(abs(length - 1_270_000) <= 1 for length in pin_lengths), pin_lengths
        assert any(abs(width - 2_032_000) <= 1 and abs(height - 5_080_000) <= 1
                   for width, height in rectangle_sizes), rectangle_sizes

        # Use a fixture with an embedded schematic image to exercise image positions
        # independently of symbol and wire serialization.
        # The IPC server keeps the project associated with an open document active;
        # close the first project before opening a fixture from another project.
        api.request(project.CloseDocument(document=document), Empty)
        image_design = api.copy_project("demos/tiny_tapeout", "schematic-units-image")
        image_path = image_design / "rp2040.kicad_sch"
        image_document = api.request(
            project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(image_path)),
            project.OpenDocumentResponse,
        ).document
        image_header = types.ItemHeader(document=image_document)
        image_response = api.request(
            editor.GetItems(header=image_header, types=[enums.KOT_SCH_BITMAP]),
            editor.GetItemsResponse,
        )
        assert image_response.items, "embedded-image fixture returned no bitmap"
        image = schematic.SchematicImage()
        assert image_response.items[0].Unpack(image)
        native_image = native_at(image_path, "image", image.id.value)
        assert abs(native_image[0] - image.position.x_nm / ONE_MM_NM) < 1e-6
        assert abs(native_image[1] - image.position.y_nm / ONE_MM_NM) < 1e-6
        api.request(project.CloseDocument(document=image_document), Empty)

        print("schematic unit regression passed: one millimetre survives IPC and native reopen")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("cli_pos", nargs="?", help="headless kicad-cli executable")
    parser.add_argument("--cli", dest="cli_opt", help="headless kicad-cli executable")
    parser.add_argument("--stock-cli", type=Path,
                        help="optional stock 10.0.6 kicad-cli used to resave the native file")
    args = parser.parse_args()
    cli = args.cli_opt or args.cli_pos
    if not cli:
        parser.error("provide kicad-cli as a positional argument or with --cli")
    stock_cli = args.stock_cli.resolve(strict=True) if args.stock_cli else None
    if stock_cli:
        assert subprocess.check_output([str(stock_cli), "--version"], text=True).strip() == "10.0.6"
    main(cli, stock_cli)
