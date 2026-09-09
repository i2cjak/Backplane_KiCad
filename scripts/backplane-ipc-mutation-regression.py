#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check item-class CRUD, transaction rollback, and file persistence through IPC."""

import argparse
from collections import OrderedDict
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from google.protobuf import message_factory, symbol_database
from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def unpack(any_item):
    descriptor = symbol_database.Default().pool.FindMessageTypeByName(any_item.TypeName())
    result = message_factory.GetMessageClass(descriptor)()
    assert any_item.Unpack(result)
    return result


def item_key(item):
    key = item.DESCRIPTOR.name
    if key == "Dimension":
        key += "." + item.WhichOneof("dimension_style")
    elif key in ("SchematicLine", "BusEntry"):
        key += "." + str(item.type)
    return key


def item_id(item):
    if item.DESCRIPTOR.name == "Field":
        return item.text.id
    if item.DESCRIPTOR.name in ("TableCell", "SchematicTableCell"):
        return item.text_box.id
    return item.id


def clone_with_new_ids(item):
    """Keep internal references coherent while giving cloned children new UUIDs."""
    clone = type(item)()
    clone.CopyFrom(item)
    replacements = {}

    def visit(message, callback):
        if message.DESCRIPTOR.full_name == "google.protobuf.Any":
            payload = unpack(message)
            visit(payload, callback)
            message.Pack(payload)
            return
        callback(message)
        for field, value in message.ListFields():
            if field.message_type is None or field.message_type.GetOptions().map_entry:
                continue
            repeated = getattr(field, "is_repeated", None)
            if repeated is None:
                repeated = field.label == field.LABEL_REPEATED
            for child in value if repeated else [value]:
                visit(child, callback)

    def replace_id(message):
        descriptor = message.DESCRIPTOR.fields_by_name.get("id")
        if descriptor and descriptor.message_type and descriptor.message_type.full_name == "kiapi.common.types.KIID":
            old = message.id.value
            message.id.value = str(uuid.uuid4())
            if old:
                replacements[old] = message.id.value

    def replace_reference(message):
        if message.DESCRIPTOR.full_name == "kiapi.common.types.KIID" and message.value in replacements:
            message.value = replacements[message.value]

    visit(clone, replace_id)
    visit(clone, replace_reference)
    return clone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--kind", choices=("all", "schematic", "pcb"), default="all",
                        help="document class to exercise (default: all)")
    parser.add_argument("--stock-cli", type=Path,
                        help="Also resave each result with an unmodified KiCad 10.0.6 CLI")
    args = parser.parse_args()
    if args.stock_cli:
        args.stock_cli = args.stock_cli.resolve(strict=True)
        version = subprocess.check_output([str(args.stock_cli), "--version"], text=True).strip()
        assert version == "10.0.6", f"Compatibility check requires stock 10.0.6, got {version}"
    with IpcSession(args.kicad_cli) as api:
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        board = api.module("board.board_types")
        sch = api.module("schematic.schematic_types")
        sch_rules = api.module("schematic.schematic_rules")
        document_kinds = (
            (types.DOCTYPE_PCB, "pcbnew", "kicad_pcb", "KOT_PCB_"),
            (types.DOCTYPE_SCHEMATIC, "eeschema", "kicad_sch", "KOT_SCH_"),
        )
        if args.kind != "all":
            wanted = types.DOCTYPE_SCHEMATIC if args.kind == "schematic" else types.DOCTYPE_PCB
            document_kinds = tuple(item for item in document_kinds if item[0] == wanted)

        for kind, folder, extension, prefix in document_kinds:
            destination = api.root / folder
            destination.mkdir()
            source = api.source / "qa/data" / folder
            for name in (f"api_kitchen_sink.{extension}", "api_kitchen_sink.kicad_pro",
                         "erc_test_dynamic_power_symbol_subsheet.kicad_sch"):
                if (source / name).is_file():
                    shutil.copy2(source / name, destination / name)
            if kind == types.DOCTYPE_SCHEMATIC and not (destination / "api_kitchen_sink.kicad_pro").is_file():
                # The schematic kitchen-sink fixture predates the project-file
                # requirement.  Give it a real project container so the API
                # server can open it without changing the source QA fixture.
                shutil.copy2(source / "ERC_dynamic_power_symbol_test.kicad_pro",
                             destination / "api_kitchen_sink.kicad_pro")
            if kind == types.DOCTYPE_SCHEMATIC:
                # The upstream fixture is generated by a newer KiCad than the
                # stock 10.0.6 reader.  Keep the class-rich fixture while
                # normalizing only newer native syntax that 10.0.6 cannot read.
                fixture = destination / "api_kitchen_sink.kicad_sch"
                native = fixture.read_text(encoding="utf-8")
                native = native.replace("(version 20260326)", "(version 20250114)", 1)
                native = re.sub(r"\n\s*\(locked yes\)", "", native)
                native = re.sub(
                    r"\(fill\s+\(type\s+(?:hatch|cross_hatch|reverse_hatch)\)\s+\(color\s+[^)]*\)\)",
                    "(fill (type none))", native)
                fixture.write_text(native, encoding="utf-8")
            path = destination / f"api_kitchen_sink.{extension}"

            def open_document():
                return api.request(project.OpenDocument(type=kind, path=str(path)),
                                   project.OpenDocumentResponse).document

            document = open_document()
            header = types.ItemHeader(document=document)

            if kind == types.DOCTYPE_SCHEMATIC:
                # ERC markers are transient rule-check results.  They have no
                # KIID and are excluded by native save, so mutation must
                # return a per-item error rather than creating an unaddressable
                # object or dereferencing a missing context.
                marker = sch_rules.ErcMarker(
                    error_type=sch_rules.ERCET_GENERIC_WARNING,
                    position=types.Vector2(x_nm=10000000, y_nm=10000000))

                create_marker = editor.CreateItems(header=header)
                create_marker.items.add().Pack(marker)
                create_result = api.request(create_marker, editor.CreateItemsResponse)
                assert create_result.status == types.IRS_OK, create_result
                assert len(create_result.created_items) == 1
                create_status = create_result.created_items[0].status
                assert create_status.code == editor.ISC_IMMUTABLE, create_result
                assert "read-only" in create_status.error_message.lower(), create_result

                update_marker = editor.UpdateItems(header=header)
                update_marker.items.add().Pack(marker)
                update_result = api.request(update_marker, editor.UpdateItemsResponse)
                assert update_result.status == types.IRS_OK, update_result
                assert len(update_result.updated_items) == 1
                update_status = update_result.updated_items[0].status
                assert update_status.code == editor.ISC_IMMUTABLE, update_result
                assert "read-only" in update_status.error_message.lower(), update_result
                print("PASS KOT_SCH_Marker: read-only create/update rejection", flush=True)

            requested = [value for name, value in enums.KiCadObjectType.items()
                         if name.startswith(prefix)]
            response = api.request(editor.GetItems(header=header, types=requested),
                                   editor.GetItemsResponse)
            assert response.status == types.IRS_OK, response
            prototypes = OrderedDict()
            for any_item in response.items:
                item = unpack(any_item)
                if "id" in item.DESCRIPTOR.fields_by_name:
                    prototypes.setdefault(item_key(item), item)

            if kind == types.DOCTYPE_PCB:
                prototypes["Group"] = board.Group(name="IPC matrix group")
                footprint = prototypes["FootprintInstance"]
                field = board.Field()
                field.CopyFrom(footprint.value_field)
                field.id.id = 0  # FIELD_T::USER; never create another mandatory field.
                field.name = "IPC matrix user field"
                prototypes["Field"] = field
                cell = board.TableCell(column_span=1, row_span=1)
                cell.text_box.layer = board.BL_F_SilkS
                cell.text_box.textbox.text = "IPC table cell"
                cell.text_box.textbox.attributes.size.CopyFrom(
                    types.Vector2(x_nm=1000000, y_nm=1000000))
                cell.text_box.textbox.top_left.CopyFrom(types.Vector2(x_nm=10000000, y_nm=10000000))
                cell.text_box.textbox.bottom_right.CopyFrom(types.Vector2(x_nm=20000000, y_nm=15000000))
                cell.custom_properties.add(key="Cell property", value="survives")
                prototypes["Table"] = board.Table(
                    column_count=1, column_widths=[10000000], row_heights=[5000000],
                    layer=board.BL_F_SilkS, cells=[cell])
                if "Arc" not in prototypes:
                    prototypes["Arc"] = board.Arc(
                        start=types.Vector2(x_nm=10000000, y_nm=10000000),
                        mid=types.Vector2(x_nm=15000000, y_nm=15000000),
                        end=types.Vector2(x_nm=20000000, y_nm=10000000),
                        width=types.Distance(value_nm=250000), layer=board.BL_F_Cu)
                expected = {"Track", "Arc", "Via", "BoardText", "BoardTextBox",
                            "BoardGraphicShape", "Barcode", "Zone", "Group",
                            "ReferenceImage", "Pad", "Field", "FootprintInstance", "Table",
                            *(f"Dimension.{style}" for style in
                              ("aligned", "orthogonal", "radial", "leader", "center"))}
            else:
                cell = sch.SchematicTableCell(column_span=1, row_span=1)
                cell.text_box.textbox.text = "IPC table cell"
                cell.text_box.textbox.attributes.size.CopyFrom(
                    types.Vector2(x_nm=1270000, y_nm=1270000))
                cell.text_box.textbox.top_left.CopyFrom(types.Vector2(x_nm=10000000, y_nm=10000000))
                cell.text_box.textbox.bottom_right.CopyFrom(types.Vector2(x_nm=20000000, y_nm=15000000))
                prototypes["SchematicTable"] = sch.SchematicTable(
                    column_count=1, column_widths=[types.Distance(value_nm=10000000)],
                    row_heights=[types.Distance(value_nm=5000000)], cells=[cell])
                if "BusEntry.2" not in prototypes:
                    bus_entry = type(prototypes["BusEntry.1"])()
                    bus_entry.CopyFrom(prototypes["BusEntry.1"])
                    bus_entry.type = sch.BET_BUS_TO_BUS
                    bus_entry.id.value = str(uuid.uuid4())
                    prototypes["BusEntry.2"] = bus_entry
                rule_area = sch.SchematicRuleArea()
                rule_area.id.value = str(uuid.uuid4())
                # SCH_RULE_AREA only accepts polygon geometry.  A rectangle
                # is a valid GraphicShape for ordinary graphics, but native
                # SCH_RULE_AREA::Deserialize deliberately rejects it.
                outline = rule_area.shape.polygon.polygons.add().outline
                outline.closed = True
                for x_nm, y_nm in ((10000000, 10000000), (20000000, 10000000),
                                    (20000000, 15000000), (10000000, 15000000)):
                    outline.nodes.add().point.CopyFrom(types.Vector2(x_nm=x_nm, y_nm=y_nm))
                prototypes["SchematicRuleArea"] = rule_area
                expected = {"Junction", "NoConnectMarker", "BusEntry.1", "BusEntry.2",
                            "SchematicLine.1", "SchematicLine.2", "SchematicLine.3",
                            "SchematicGraphicShape", "SchematicImage", "SchematicTextBox",
                            "SchematicText", "LocalLabel", "GlobalLabel", "HierarchicalLabel",
                            "DirectiveLabel", "Group", "SchematicSymbolInstance", "SheetSymbol",
                            "SchematicRuleArea", "SchematicTable"}
            assert expected <= prototypes.keys(), f"Fixture lacks {sorted(expected - prototypes.keys())}"

            def fetch(item_id):
                response = api.request_raw(editor.GetItemsById(
                    header=header, items=[types.KIID(value=item_id)]))
                if (response.status.status == api.envelope.AS_BAD_REQUEST and
                        response.status.error_message == "none of the requested IDs were found or valid"):
                    return None
                assert response.status.status == api.envelope.AS_OK, response
                result = editor.GetItemsResponse()
                assert response.message.Unpack(result), response
                assert result.status == types.IRS_OK, result
                return unpack(result.items[0]) if result.items else None

            def transaction(action, operation):
                commit = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                result = operation()
                api.request(editor.EndCommit(header=header, id=commit.id, action=action,
                                             message="Disposable item-class regression"),
                            editor.EndCommitResponse)
                return result

            def write_item(item, create, container_id=None):
                item_header = types.ItemHeader(document=document)
                if kind == types.DOCTYPE_PCB and item.DESCRIPTOR.name in ("Pad", "Field"):
                    item_header.container.CopyFrom(item.text.parent if item.DESCRIPTOR.name == "Field" else item.parent)
                elif kind == types.DOCTYPE_PCB and item.DESCRIPTOR.name == "TableCell":
                    assert container_id, "PCB table-cell mutations require their table container"
                    item_header.container.CopyFrom(types.KIID(value=container_id))
                command = editor.CreateItems(header=item_header) if create else editor.UpdateItems(header=item_header)
                command.items.add().Pack(item)
                result = api.request(command, editor.CreateItemsResponse if create else editor.UpdateItemsResponse)
                assert result.status == types.IRS_OK, result
                items = result.created_items if create else result.updated_items
                assert len(items) == 1 and items[0].status.code == editor.ISC_OK, result
                return unpack(items[0].item)

            def assert_children_preserved(expected_item, actual_item, key):
                """Check nested schematic data that has no independent CRUD ID."""
                if expected_item.DESCRIPTOR.name not in ("SchematicTable", "Table"):
                    return

                assert len(actual_item.cells) == len(expected_item.cells), \
                    f"{key}: table cell count changed"
                expected_text = [cell.text_box.textbox.text for cell in expected_item.cells]
                actual_text = [cell.text_box.textbox.text for cell in actual_item.cells]
                assert actual_text == expected_text, f"{key}: table cell text changed"
                expected_properties = [
                    {(prop.key, prop.value) for prop in cell.custom_properties}
                    for cell in expected_item.cells
                ]
                actual_properties = [
                    {(prop.key, prop.value) for prop in cell.custom_properties}
                    for cell in actual_item.cells
                ]
                assert actual_properties == expected_properties, \
                    f"{key}: table cell properties changed"

            def table_grid_snapshot(table):
                """Capture table structure while ignoring editable cell content."""
                return (
                    table.column_count,
                    tuple(table.column_widths),
                    tuple(table.row_heights),
                    table.external_border,
                    table.header_separator,
                    table.row_separators,
                    table.column_separators,
                    tuple((cell.text_box.id.value, cell.column_span, cell.row_span)
                          for cell in table.cells),
                )

            def table_cell_snapshot(cell):
                return (
                    cell.text_box.id.value,
                    cell.text_box.textbox.text,
                    tuple(sorted((prop.key, prop.value) for prop in cell.custom_properties)),
                    cell.column_span,
                    cell.row_span,
                )

            def reopen():
                nonlocal document, header
                api.request(project.SaveDocument(document=document), Empty)
                api.request(project.CloseDocument(document=document), Empty)
                if args.stock_cli:
                    result = subprocess.run(
                        [str(args.stock_cli), "pcb" if kind == types.DOCTYPE_PCB else "sch",
                         "upgrade", "--force", str(path)],
                        env=api.env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=60,
                    )
                    assert result.returncode == 0, f"Stock 10.0.6 rejected saved file: {result.stdout}"
                document = open_document()
                header = types.ItemHeader(document=document)

            # Run the table child round-trip before the rule-area protocol
            # dependency, so the independent nested-item coverage remains
            # visible even when an older daemon lacks the rule-area type map.
            keys = sorted(expected, key=lambda value: (value == "SchematicRuleArea", value))
            for key in keys:
                print(f"RUN {prefix}{key}", flush=True)
                item = clone_with_new_ids(prototypes[key])
                if item.DESCRIPTOR.name == "Group":
                    # KiCad omits empty PCB groups. Use a disposable child to avoid
                    # changing the original fixture's group membership in either editor.
                    del item.items[:]
                    member_key = "Track" if kind == types.DOCTYPE_PCB else "SchematicText"
                    member = clone_with_new_ids(prototypes[member_key])
                    transaction(editor.CMA_COMMIT, lambda: write_item(member, True))
                    item.items.add().CopyFrom(item_id(member))
                uuid_value = item_id(item).value
                transaction(editor.CMA_DROP, lambda: write_item(item, True))
                assert fetch(uuid_value) is None, f"{key}: create rollback left an item"
                transaction(editor.CMA_COMMIT, lambda: write_item(item, True))
                created = fetch(uuid_value)
                assert created is not None, f"{key}: create commit lost the item"
                assert_children_preserved(item, created, key)

                assert "custom_properties" in created.DESCRIPTOR.fields_by_name, key
                before = {prop.key: prop.value for prop in created.custom_properties}
                changed = type(created)()
                changed.CopyFrom(created)
                changed.custom_properties.add(key="Backplane regression", value="persisted")
                transaction(editor.CMA_DROP, lambda: write_item(changed, False))
                assert {prop.key: prop.value for prop in fetch(uuid_value).custom_properties} == before, f"{key}: update rollback failed"
                transaction(editor.CMA_COMMIT, lambda: write_item(changed, False))

                def delete():
                    result = api.request(editor.DeleteItems(header=header,
                                                           item_ids=[types.KIID(value=uuid_value)]),
                                         editor.DeleteItemsResponse)
                    assert result.status == types.IRS_OK, result
                    assert len(result.deleted_items) == 1 and result.deleted_items[0].status == editor.IDS_OK, result

                reopen()
                saved = fetch(uuid_value)
                if key == "BusEntry.2":
                    # Stock 10.0.6 has no native bus-to-bus entry token.  The
                    # writer converts it to a bus segment.  The conversion
                    # must retain the UUID and companion metadata, while
                    # preserving the geometry and delete/rollback semantics.
                    assert saved is not None, "BusEntry.2 conversion lost its UUID"
                    assert saved.DESCRIPTOR.name == "SchematicLine", saved
                    assert saved.type == sch.SLT_BUS, saved
                    expected_end = types.Vector2(
                        x_nm=item.position.x_nm + item.size.x_nm,
                        y_nm=item.position.y_nm + item.size.y_nm)
                    assert saved.start == item.position and saved.end == expected_end, (
                        f"{key}: converted bus geometry changed")
                    assert {prop.key: prop.value for prop in saved.custom_properties}.get(
                        "Backplane regression") == "persisted", (
                        f"{key}: companion metadata did not follow converted bus line")
                    transaction(editor.CMA_DROP, delete)
                    assert fetch(uuid_value) is not None, f"{key}: delete rollback lost converted line"
                    transaction(editor.CMA_COMMIT, delete)
                    reopen()
                    assert fetch(uuid_value) is None, f"{key}: converted line survived delete/reopen"
                    print(f"PASS {prefix}{key}: stock-compatible bus segment conversion", flush=True)
                    continue
                assert saved is not None and {prop.key: prop.value for prop in saved.custom_properties}.get("Backplane regression") == "persisted", (
                    f"{key}: update did not survive save/reopen")
                assert_children_preserved(changed, saved, key)

                if key == "Table":
                    # TableCell is nested in Table but has a stable KIID. Exercise
                    # its direct UpdateItems path with the table as the request
                    # container, while proving that cell edits do not mutate the
                    # table's grid or cell spans.
                    assert len(saved.cells) == 1, "Table: expected one regression cell"
                    grid_before = table_grid_snapshot(saved)
                    cell_before = table_cell_snapshot(saved.cells[0])
                    updated_cell = board.TableCell()
                    updated_cell.CopyFrom(saved.cells[0])
                    updated_cell.text_box.textbox.text += " (direct update)"
                    del updated_cell.custom_properties[:]
                    updated_cell.custom_properties.add(key="Cell property", value="updated")
                    expected_cell = table_cell_snapshot(updated_cell)

                    update_cell = lambda: write_item(updated_cell, False, uuid_value)
                    transaction(editor.CMA_DROP, update_cell)
                    rolled_back_table = fetch(uuid_value)
                    assert table_cell_snapshot(rolled_back_table.cells[0]) == cell_before, (
                        "Table: direct cell update rollback changed cell content")
                    assert table_grid_snapshot(rolled_back_table) == grid_before, (
                        "Table: direct cell update rollback changed grid structure")

                    transaction(editor.CMA_COMMIT, update_cell)
                    committed_table = fetch(uuid_value)
                    assert table_cell_snapshot(committed_table.cells[0]) == expected_cell, (
                        "Table: direct cell update did not change text and metadata")
                    assert table_grid_snapshot(committed_table) == grid_before, (
                        "Table: direct cell update changed grid structure")

                    reopen()
                    saved = fetch(uuid_value)
                    assert table_cell_snapshot(saved.cells[0]) == expected_cell, (
                        "Table: direct cell update did not survive save/reopen")
                    assert table_grid_snapshot(saved) == grid_before, (
                        "Table: direct cell update changed grid after save/reopen")
                    print("PASS PCB_TableCell: direct update, rollback, grid preservation, save/reopen",
                          flush=True)

                    # A table cell has a stable UUID for updates and reads, but
                    # its ownership is structural: clients may not add or
                    # remove cells independently of the table grid.
                    rejected_cell = clone_with_new_ids(saved.cells[0])
                    cell_header = types.ItemHeader(document=document)
                    cell_header.container.CopyFrom(types.KIID(value=uuid_value))
                    create_cell = editor.CreateItems(header=cell_header)
                    create_cell.items.add().Pack(rejected_cell)
                    create_cell_result = api.request(create_cell, editor.CreateItemsResponse)
                    assert create_cell_result.status == types.IRS_OK, create_cell_result
                    assert len(create_cell_result.created_items) == 1
                    assert create_cell_result.created_items[0].status.code == editor.ISC_IMMUTABLE, (
                        "TableCell create was not rejected as immutable")

                    unchanged = fetch(uuid_value)
                    assert table_cell_snapshot(unchanged.cells[0]) == expected_cell, (
                        "Rejected TableCell create changed table cell content")
                    assert table_grid_snapshot(unchanged) == grid_before, (
                        "Rejected TableCell create changed table grid")

                    cell_id = saved.cells[0].text_box.id.value
                    delete_cell = editor.DeleteItems(
                        header=header, item_ids=[types.KIID(value=cell_id)])
                    delete_cell_result = api.request(delete_cell, editor.DeleteItemsResponse)
                    assert delete_cell_result.status == types.IRS_OK, delete_cell_result
                    assert len(delete_cell_result.deleted_items) == 1
                    assert delete_cell_result.deleted_items[0].status == editor.IDS_IMMUTABLE, (
                        "TableCell delete was not rejected as immutable")

                    unchanged = fetch(uuid_value)
                    assert table_cell_snapshot(unchanged.cells[0]) == expected_cell, (
                        "Rejected TableCell delete changed table cell content")
                    assert table_grid_snapshot(unchanged) == grid_before, (
                        "Rejected TableCell delete changed table grid")
                    reopen()
                    unchanged = fetch(uuid_value)
                    assert table_cell_snapshot(unchanged.cells[0]) == expected_cell, (
                        "Rejected TableCell mutation did not leave clean saved state")
                    assert table_grid_snapshot(unchanged) == grid_before, (
                        "Rejected TableCell mutation changed saved table grid")
                    print("PASS PCB_TableCell: independent create/delete rejection, clean state",
                          flush=True)

                cleared = type(saved)()
                cleared.CopyFrom(saved)
                del cleared.custom_properties[:]
                transaction(editor.CMA_COMMIT, lambda: write_item(cleared, False))
                reopen()
                assert not fetch(uuid_value).custom_properties, f"{key}: cleared metadata returned after save/reopen"

                transaction(editor.CMA_DROP, delete)
                assert fetch(uuid_value) is not None, f"{key}: delete rollback lost the item"
                transaction(editor.CMA_COMMIT, delete)
                reopen()
                assert fetch(uuid_value) is None, f"{key}: deleted item returned after reopen"
                print(f"PASS {prefix}{key}: create/update/delete, rollback, save/reopen", flush=True)

            api.request(project.CloseDocument(document=document), Empty)


if __name__ == "__main__":
    main()
