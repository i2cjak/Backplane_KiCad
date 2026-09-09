#!/usr/bin/env python3
"""Exercise schematic IPC fidelity against an isolated KiCad CLI.

This is deliberately separate from the broad release smoke.  It keeps a
complete symbol definition in the update request, changes one instance field,
and verifies that graphics, library identity, pins, variants, fields and
custom metadata survive rollback, commit, save and reopen.  It also sends the
published 10.0.6 command URLs to cover the namespace compatibility path.
"""

import argparse
from pathlib import Path
import subprocess

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def expect_status(api, command, status):
    response = api.request_raw(command)
    assert response.status.status == status, response.status


def symbol_snapshot(symbol):
    """Return stable, value-independent data from a symbol response."""
    definition = symbol.definition

    def canonical_child(child):
        """Normalize generated UUIDs on library-definition children.

        KiCad's stock schematic format does not persist UUIDs for library
        graphics and pins.  The IPC model assigns UUIDs while loading the
        definition, so those IDs can change after a native save/reopen even
        when the geometry and pin data are identical.
        """
        type_url = child.item.type_url
        value = child.item.value
        full_name = type_url.rsplit("/", 1)[-1]

        try:
            descriptor = child.DESCRIPTOR.file.pool.FindMessageTypeByName(full_name)
            message = message_factory.GetMessageClass(descriptor)()
        except KeyError:
            return type_url, value

        if not child.item.Unpack(message):
            return type_url, value

        if "id" in message.DESCRIPTOR.fields_by_name:
            message.ClearField("id")

        return type_url, message.SerializeToString(deterministic=True)

    canonical_items = tuple(
        (canonical_child(child), child.unit.unit if child.HasField("unit") else None,
         child.body_style.style if child.HasField("body_style") else None, child.is_private)
        for child in definition.items
    )
    children = tuple(item for item in canonical_items)
    fields = tuple(
        (field.name, field.text.text, field.visible, field.show_name, field.allow_auto_place)
        for field in symbol.user_fields
    )
    pins = tuple(
        item[0]
        for item in canonical_items
        if item[0][0].endswith("SchematicPin")
    )
    return {
        "id": symbol.id.value,
        "library_id": (definition.id.library_nickname, definition.id.entry_name),
        "children": children,
        "pins": pins,
        "user_fields": fields,
        "variants": tuple(
            (variant.name, variant.description,
             tuple(sorted(variant.fields.items())),
             variant.attributes.exclude_from_simulation,
             variant.attributes.do_not_populate)
            for variant in symbol.variants
        ),
        "custom": tuple((prop.key, prop.value) for prop in symbol.custom_properties),
    }


def legacy_request(api, command, response_type, response_url):
    """Send a wire-compatible request using the old 10.0.6 Any URL."""
    request = api.envelope.ApiRequest()
    request.header.client_name = "works.backplane.schematic-legacy-regression"
    request.header.kicad_token = api.token
    request.message.Pack(command)
    request.message.type_url = "type.googleapis.com/kiapi.schematic.types." + command.DESCRIPTOR.name
    api.connection.send(request.SerializeToString())
    response = api.envelope.ApiResponse.FromString(api.connection.recv())
    api.token = response.header.kicad_token
    if response.status.status != api.envelope.AS_OK:
        raise AssertionError(f"legacy {command.DESCRIPTOR.name}: {response.status}")

    # The server returns the old response URL for this compatibility route.
    assert response.message.type_url == response_url, response.message.type_url
    # Normalise it only for protobuf's generated-class URL check.  The wire
    # payload is identical between the old and corrected package names.
    response.message.type_url = "type.googleapis.com/" + response_type.DESCRIPTOR.full_name
    result = response_type()
    if not response.message.Unpack(result):
        raise AssertionError(f"legacy response did not unpack: {response.message.type_url}")
    return result


def legacy_symbol_instance(symbol):
    """Model the published scalar field 11 in a separate protobuf descriptor pool."""
    pool = descriptor_pool.DescriptorPool()

    def add_file(source):
        for dependency in source.dependencies:
            try:
                pool.FindFileByName(dependency.name)
            except KeyError:
                add_file(dependency)
        descriptor = descriptor_pb2.FileDescriptorProto.FromString(source.serialized_pb)
        for message in descriptor.message_type:
            if message.name == "SchematicSymbol":
                field = next(field for field in message.field if field.number == 11)
                field.name = "body_style_count"
                field.json_name = "bodyStyleCount"
                field.type = field.TYPE_UINT32
                field.label = field.LABEL_OPTIONAL
                field.ClearField("type_name")
        pool.Add(descriptor)

    add_file(symbol.DESCRIPTOR.file)
    descriptor = pool.FindMessageTypeByName(symbol.DESCRIPTOR.full_name)
    return message_factory.GetMessageClass(descriptor).FromString(symbol.SerializeToString())


def main(cli, stock_cli=None):
    with IpcSession(cli) as api:
        # Import generated modules only after IpcSession has created its
        # isolated binding directory.
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        variants_api = api.module("common.commands.variant_commands")
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        schematic = api.module("schematic.schematic_types")
        commands = api.module("schematic.schematic_commands")

        design = api.copy_project("demos/ecc83", "schematic-fidelity")
        path = design / "ecc83-pp.kicad_sch"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)

        hierarchy = api.request(commands.GetSchematicHierarchy(document=document),
                                commands.SchematicHierarchyResponse)
        assert hierarchy.top_level_sheets, "hierarchy response is empty"
        netlist = api.request(commands.GetSchematicNetlist(document=document),
                              commands.SchematicNetlistResponse)
        assert netlist.nets, "corrected GetSchematicNetlist returned no nets"

        old_hierarchy = legacy_request(
            api, commands.GetSchematicHierarchy(document=document),
            commands.SchematicHierarchyResponse,
            "type.googleapis.com/kiapi.schematic.types.SchematicHierarchyResponse")
        assert old_hierarchy.top_level_sheets, "legacy hierarchy response is empty"
        old_netlist = legacy_request(
            api, commands.GetSchematicNetlist(document=document),
            commands.SchematicNetlistResponse,
            "type.googleapis.com/kiapi.schematic.types.SchematicNetlistResponse")
        assert old_netlist.nets, "legacy GetSchematicNetlist returned no nets"

        variant_name = "Backplane IPC Fidelity"
        api.request(variants_api.AddVariant(document=document, name=variant_name,
                                            description="round-trip variant"), Empty)
        listed = api.request(variants_api.GetVariants(document=document),
                             variants_api.VariantsResponse)
        assert any(item.name == variant_name and item.description == "round-trip variant"
                   for item in listed.variants), "schematic variant was not created"
        api.request(variants_api.SetCurrentVariant(document=document, name=variant_name), Empty)
        current = api.request(variants_api.GetCurrentVariant(document=document),
                              variants_api.CurrentVariantResponse)
        assert current.name == variant_name, "schematic current variant was not selected"

        # Variant metadata is a document mutation.  Establish a clean baseline
        # before checking that selection state itself does not dirty the file.
        api.request(project.SaveDocument(document=document), Empty)

        result = api.request(editor.GetItems(header=header, types=[enums.KOT_SCH_SYMBOL]),
                             editor.GetItemsResponse)
        assert result.items, "schematic returned no symbols"
        symbols = []
        for item in result.items:
            candidate = schematic.SchematicSymbolInstance()
            assert item.Unpack(candidate)
            symbols.append(candidate)
        symbol = next(item for item in symbols if item.reference_field.text.text == "R1")
        assert symbol.definition.id.entry_name, "symbol library identifier was lost"
        assert symbol.definition.items, "symbol library definition has no graphics or pins"
        selection_baseline = api.request(
            editor.GetDocumentModifiedState(document=document),
            editor.GetDocumentModifiedStateResponse,
        ).state

        # Selection is editor state and must round-trip through a headless
        # schematic context without making the native document dirty.
        assert api.request(editor.GetSelection(header=header),
                           editor.SelectionResponse).items == []
        selected = api.request(
            editor.AddToSelection(header=header, items=[symbol.id]),
            editor.SelectionResponse,
        )
        assert len(selected.items) == 1
        selected_symbol = schematic.SchematicSymbolInstance()
        assert selected.items[0].Unpack(selected_symbol)
        assert selected_symbol.id.value == symbol.id.value
        assert api.request(editor.GetDocumentModifiedState(document=document),
                           editor.GetDocumentModifiedStateResponse).state == selection_baseline
        removed = api.request(
            editor.RemoveFromSelection(header=header, items=[symbol.id]),
            editor.SelectionResponse,
        )
        assert not removed.items
        api.request(editor.AddToSelection(header=header, items=[symbol.id]),
                    editor.SelectionResponse)
        api.request(editor.ClearSelection(header=header), Empty)
        assert not api.request(editor.GetSelection(header=header),
                               editor.SelectionResponse).items
        assert api.request(editor.GetDocumentModifiedState(document=document),
                           editor.GetDocumentModifiedStateResponse).state == selection_baseline

        original = symbol_snapshot(symbol)
        old_value = symbol.value_field.text.text
        symbol.value_field.text.text = old_value + " IPC"
        symbol.custom_properties.add(key="Backplane fidelity", value="survives save")

        def read_symbol():
            response = api.request(editor.GetItemsById(header=header, items=[symbol.id]),
                                   editor.GetItemsResponse)
            assert len(response.items) == 1, response
            current = schematic.SchematicSymbolInstance()
            assert response.items[0].Unpack(current)
            return current

        # A client generated from the published scalar body-style field must
        # still read the count and update the symbol without losing its graphics.
        legacy = legacy_symbol_instance(symbol)
        assert legacy.definition.body_style_count == len(symbol.definition.body_style)
        legacy.definition.DiscardUnknownFields()
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        update = editor.UpdateItems(header=header)
        update.items.add().Pack(legacy)
        result = api.request(update, editor.UpdateItemsResponse)
        assert result.updated_items[0].status.code == editor.ISC_OK, result
        assert len(read_symbol().definition.body_style) == legacy.definition.body_style_count
        assert symbol_snapshot(read_symbol())["children"] == original["children"]
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_DROP,
                                     message="Legacy symbol wire format"), editor.EndCommitResponse)

        # A dropped update must restore every part of the original definition.
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        update = editor.UpdateItems(header=header)
        update.items.add().Pack(symbol)
        response = api.request(update, editor.UpdateItemsResponse)
        assert response.updated_items[0].status.code == editor.ISC_OK, response
        expect_status(api, project.CloseDocument(document=document), api.envelope.AS_BAD_REQUEST)
        expect_status(api, project.CloseAllDocuments(force=False), api.envelope.AS_BAD_REQUEST)
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_DROP,
                                     message="Schematic fidelity rollback"), editor.EndCommitResponse)
        assert symbol_snapshot(read_symbol()) == original, "symbol graphics changed during rollback"

        # Commit the value edit, then save and reopen the native 10.0.6 file.
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        update = editor.UpdateItems(header=header)
        update.items.add().Pack(symbol)
        response = api.request(update, editor.UpdateItemsResponse)
        assert response.updated_items[0].status.code == editor.ISC_OK, response
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                     message="Schematic fidelity value update"), editor.EndCommitResponse)
        committed = read_symbol()
        assert committed.value_field.text.text == old_value + " IPC"
        assert symbol_snapshot(committed)["library_id"] == original["library_id"]
        assert symbol_snapshot(committed)["children"] == original["children"], \
            "symbol graphics/pins changed during value update"

        api.request(project.SaveDocument(document=document), Empty)
        companion = Path(str(path) + ".backplane.json")
        assert companion.exists(), "custom metadata companion was not written"
        api.request(project.CloseDocument(document=document), Empty)
        if stock_cli:
            stock = subprocess.run([str(stock_cli), "sch", "upgrade", "--force", str(path)],
                                   env=api.env, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=60)
            assert stock.returncode == 0, stock.stdout
        document = api.request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)
        listed = api.request(variants_api.GetVariants(document=document),
                             variants_api.VariantsResponse)
        assert any(item.name == variant_name and item.description == "round-trip variant"
                   for item in listed.variants), "schematic variant did not survive save/reopen"
        reopened = read_symbol()
        assert reopened.value_field.text.text == old_value + " IPC"
        assert symbol_snapshot(reopened)["children"] == original["children"], \
            "symbol graphics/pins changed after save/reopen"
        assert ("Backplane fidelity", "survives save") in symbol_snapshot(reopened)["custom"]

        # Endpoint styles are newer than the stock native format and therefore
        # exercise the companion metadata path.  They still round-trip through
        # the typed IPC message and must remain attached to the same UUID.
        lines = api.request(editor.GetItems(header=header, types=[enums.KOT_SCH_LINE]),
                            editor.GetItemsResponse)
        assert lines.items, "schematic fixture returned no line segments"
        line = schematic.SchematicLine()
        assert lines.items[0].Unpack(line)
        line.start_ending.style = types.LES_ARROW
        transaction = api.request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
        update = editor.UpdateItems(header=header)
        update.items.add().Pack(line)
        response = api.request(update, editor.UpdateItemsResponse)
        assert response.updated_items[0].status.code == editor.ISC_OK, response
        api.request(editor.EndCommit(header=header, id=transaction.id, action=editor.CMA_COMMIT,
                                     message="Schematic line-ending fidelity"), editor.EndCommitResponse)
        api.request(project.SaveDocument(document=document), Empty)
        api.request(project.CloseDocument(document=document), Empty)
        if stock_cli:
            stock = subprocess.run([str(stock_cli), "sch", "upgrade", "--force", str(path)],
                                   env=api.env, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=60)
            assert stock.returncode == 0, stock.stdout
        document = api.request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(path)),
                               project.OpenDocumentResponse).document
        header = types.ItemHeader(document=document)
        saved_line_items = api.request(editor.GetItemsById(header=header, items=[line.id]),
                                       editor.GetItemsResponse)
        assert len(saved_line_items.items) == 1, saved_line_items
        saved_line = schematic.SchematicLine()
        assert saved_line_items.items[0].Unpack(saved_line)
        assert saved_line.start_ending.style == types.LES_ARROW, \
            "line ending did not survive save/reopen"
        api.request(project.CloseDocument(document=document), Empty)
        print(f"schematic fidelity passed: {len(netlist.nets)} nets, "
              f"{len(original['children'])} definition items; corrected and legacy queries passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path)
    args = parser.parse_args()
    if args.stock_cli:
        args.stock_cli = args.stock_cli.resolve(strict=True)
        assert subprocess.check_output([str(args.stock_cli), "--version"], text=True).strip() == "10.0.6"
    main(args.kicad_cli.resolve(strict=True), args.stock_cli)
