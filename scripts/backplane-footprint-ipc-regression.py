#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise headless footprint-document IPC on disposable library data.

The test opens a footprint through the normal OpenDocument lifecycle, queries
its pads and fields, updates and creates a pad inside a transaction, then
deletes the created pad.  It also writes a native ``.kicad_mod`` copy and
checks that the Backplane companion file is produced.
"""

import argparse
import json
from pathlib import Path
import shutil
import uuid

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def unpack(any_item):
    from google.protobuf import message_factory, symbol_database

    descriptor = symbol_database.Default().pool.FindMessageTypeByName(any_item.TypeName())
    item = message_factory.GetMessageClass(descriptor)()
    assert any_item.Unpack(item)
    return item


def document_modified(api, editor, document):
    return api.request(
        editor.GetDocumentModifiedState(document=document),
        editor.GetDocumentModifiedStateResponse,
    ).state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()

    with IpcSession(args.kicad_cli) as api:
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        editor = api.module("common.commands.editor_commands")
        project_commands = api.module("common.commands.project_commands")
        board_commands = api.module("board.board_commands")
        board_types = api.module("board.board_types")

        project_dir = api.root / "footprint-project"
        library_dir = project_dir / "Regression.pretty"
        project_dir.mkdir()
        library_dir.mkdir()
        shutil.copy2(api.source / "qa/data/libraries/test_project/test_project.kicad_pro",
                     project_dir / "footprint-project.kicad_pro")
        shutil.copy2(api.source / "qa/data/libraries/Resistor_SMD.pretty/R_0603_1608Metric.kicad_mod",
                     library_dir / "Test.kicad_mod")
        (project_dir / "fp-lib-table").write_text(
            '(fp_lib_table\n'
            f'  (lib (name "Regression")(type "KiCad")(uri "{library_dir}")'
            '(options "")(descr "IPC regression"))\n)\n',
            encoding="utf-8",
        )

        project_doc = api.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT,
                path=str(project_dir / "footprint-project.kicad_pro"),
            ),
            project_commands.OpenDocumentResponse,
        ).document
        footprint_doc = api.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_FOOTPRINT,
                path="Regression:Test",
            ),
            project_commands.OpenDocumentResponse,
        ).document
        assert footprint_doc.type == types.DOCTYPE_FOOTPRINT
        assert footprint_doc.lib_id.entry_name == "Test"

        open_docs = api.request(
            editor.GetOpenDocuments(type=types.DOCTYPE_FOOTPRINT),
            editor.GetOpenDocumentsResponse,
        )
        assert list(open_docs.documents) == [footprint_doc]

        header = types.ItemHeader(document=footprint_doc)
        items = api.request(
            editor.GetItems(
                header=header,
                types=[enums.KOT_PCB_PAD, enums.KOT_PCB_FIELD],
            ),
            editor.GetItemsResponse,
        )
        assert items.status == types.IRS_OK and items.items
        pads = [unpack(item) for item in items.items
                if item.TypeName().endswith(".Pad")]
        assert pads, "headless footprint GetItems did not return pads"

        pad = pads[0]

        # Selection is available in a headless footprint context through
        # BOARD_ITEM flags rather than a GUI selection tool.  It is transient
        # editor state, so selection mutations must leave the document clean.
        assert len(api.request(
            editor.GetSelection(header=header), editor.SelectionResponse
        ).items) == 0
        selected = api.request(
            editor.AddToSelection(header=header, items=[pad.id]),
            editor.SelectionResponse,
        )
        assert len(selected.items) == 1
        assert unpack(selected.items[0]).id.value == pad.id.value
        assert api.request(
            editor.GetSelection(header=header), editor.SelectionResponse
        ).items
        assert len(api.request(
            editor.RemoveFromSelection(header=header, items=[pad.id]),
            editor.SelectionResponse,
        ).items) == 0
        api.request(editor.AddToSelection(header=header, items=[pad.id]),
                    editor.SelectionResponse)
        api.request(editor.ClearSelection(header=header), Empty)
        assert len(api.request(
            editor.GetSelection(header=header), editor.SelectionResponse
        ).items) == 0
        assert document_modified(api, editor, footprint_doc) == editor.DMS_UNMODIFIED

        # Layer visibility and active-layer state are also available without a
        # frame.  Restrict the state to F.Cu, then restore it through the same
        # API path so the test exercises both getters and setters.
        api.request(
            board_commands.SetVisibleLayers(
                board=footprint_doc, layers=[board_types.BL_F_Cu]
            ),
            Empty,
        )
        visible = api.request(
            board_commands.GetVisibleLayers(board=footprint_doc),
            board_commands.BoardLayers,
        )
        assert list(visible.layers) == [board_types.BL_F_Cu]
        api.request(
            board_commands.SetActiveLayer(
                board=footprint_doc, layer=board_types.BL_F_Cu
            ),
            Empty,
        )
        active = api.request(
            board_commands.GetActiveLayer(board=footprint_doc),
            board_commands.BoardLayerResponse,
        )
        assert active.layer == board_types.BL_F_Cu

        # A normal save clears the headless dirty state before later mutation
        # and SaveCopy checks.
        api.request(project_commands.SaveDocument(document=footprint_doc), Empty)
        assert document_modified(api, editor, footprint_doc) == editor.DMS_UNMODIFIED

        pad.custom_properties.add(key="backplane.footprint", value="updated")
        update_header = types.ItemHeader(document=footprint_doc)
        update_header.container.CopyFrom(pad.parent)
        transaction = api.request(
            editor.BeginCommit(header=update_header), editor.BeginCommitResponse
        )
        update = editor.UpdateItems(header=update_header)
        update.items.add().Pack(pad)
        update_result = api.request(update, editor.UpdateItemsResponse)
        assert update_result.status == types.IRS_OK
        assert len(update_result.updated_items) == 1
        assert update_result.updated_items[0].status.code == editor.ISC_OK
        api.request(
            editor.EndCommit(header=update_header, id=transaction.id,
                             action=editor.CMA_COMMIT, message="footprint update"),
            editor.EndCommitResponse,
        )
        assert document_modified(api, editor, footprint_doc) == editor.DMS_MODIFIED

        # Headless RevertDocument reloads the saved library item and restores
        # a clean state just as the interactive editor does.
        api.request(project_commands.SaveDocument(document=footprint_doc), Empty)
        baseline_x = pad.position.x_nm
        changed = type(pad)()
        changed.CopyFrom(pad)
        changed.position.x_nm += 500000
        transaction = api.request(
            editor.BeginCommit(header=update_header), editor.BeginCommitResponse
        )
        revert_update = editor.UpdateItems(header=update_header)
        revert_update.items.add().Pack(changed)
        result = api.request(revert_update, editor.UpdateItemsResponse)
        assert result.status == types.IRS_OK
        api.request(
            editor.EndCommit(header=update_header, id=transaction.id,
                             action=editor.CMA_COMMIT, message="footprint revert setup"),
            editor.EndCommitResponse,
        )
        assert document_modified(api, editor, footprint_doc) == editor.DMS_MODIFIED
        api.request(editor.RevertDocument(document=footprint_doc), Empty)
        restored_items = api.request(
            editor.GetItems(
                header=types.ItemHeader(document=footprint_doc),
                types=[enums.KOT_PCB_PAD],
            ),
            editor.GetItemsResponse,
        )
        restored_pads = [unpack(item) for item in restored_items.items]
        assert any(restored_pad.position.x_nm == baseline_x for restored_pad in restored_pads)
        pad = next(restored_pad for restored_pad in restored_pads
                    if restored_pad.position.x_nm == baseline_x)
        update_header.container.CopyFrom(pad.parent)
        assert any(prop.key == "backplane.footprint" and prop.value == "updated"
                   for prop in pad.custom_properties)
        assert document_modified(api, editor, footprint_doc) == editor.DMS_UNMODIFIED

        created = type(pad)()
        created.CopyFrom(pad)
        created.id.value = str(uuid.uuid4())
        created.position.x_nm += 1000000
        transaction = api.request(
            editor.BeginCommit(header=update_header), editor.BeginCommitResponse
        )
        create = editor.CreateItems(header=update_header)
        create.items.add().Pack(created)
        create_result = api.request(create, editor.CreateItemsResponse)
        assert create_result.status == types.IRS_OK
        assert len(create_result.created_items) == 1
        assert create_result.created_items[0].status.code == editor.ISC_OK
        api.request(
            editor.EndCommit(header=update_header, id=transaction.id,
                             action=editor.CMA_COMMIT, message="footprint create"),
            editor.EndCommitResponse,
        )
        created_items = api.request(
            editor.GetItemsById(header=update_header, items=[created.id]),
            editor.GetItemsResponse,
        )
        assert len(created_items.items) == 1

        copy_path = project_dir / "footprint-copy.kicad_mod"
        api.request(
            editor.SaveCopyOfDocument(
                document=footprint_doc,
                path=str(copy_path.relative_to(project_dir)),
                options=editor.SaveOptions(overwrite=True),
            ),
            Empty,
        )
        assert copy_path.is_file()
        companion = Path(str(copy_path) + ".backplane.json")
        assert companion.is_file(), companion
        metadata = json.loads(companion.read_text(encoding="utf-8"))
        assert metadata.get("items"), metadata

        delete_header = types.ItemHeader(document=footprint_doc)
        transaction = api.request(
            editor.BeginCommit(header=delete_header), editor.BeginCommitResponse
        )
        deleted = api.request(
            editor.DeleteItems(header=delete_header,
                               item_ids=[types.KIID(value=created.id.value)]),
            editor.DeleteItemsResponse,
        )
        assert deleted.status == types.IRS_OK
        assert len(deleted.deleted_items) == 1
        assert deleted.deleted_items[0].status == editor.IDS_OK
        api.request(
            editor.EndCommit(header=delete_header, id=transaction.id,
                             action=editor.CMA_COMMIT, message="footprint delete"),
            editor.EndCommitResponse,
        )
        missing = api.request_raw(
            editor.GetItemsById(header=delete_header, items=[created.id])
        )
        assert missing.status.status == api.envelope.AS_BAD_REQUEST

        # Save and close the library document, then reopen the native copy
        # through the same OpenDocument lifecycle.  This exercises the native
        # filename identity retained by HEADLESS_FOOTPRINT_CONTEXT rather than
        # only testing library LIB_ID dispatch.
        api.request(project_commands.SaveDocument(document=footprint_doc), Empty)
        api.request(project_commands.CloseDocument(document=footprint_doc), Empty)
        reopened_library = api.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_FOOTPRINT, path="Regression:Test"
            ),
            project_commands.OpenDocumentResponse,
        ).document
        reopened_items = api.request(
            editor.GetItems(
                header=types.ItemHeader(document=reopened_library),
                types=[enums.KOT_PCB_PAD],
            ),
            editor.GetItemsResponse,
        )
        reopened_pads = [unpack(item) for item in reopened_items.items]
        assert all(reopened_pad.position.x_nm != created.position.x_nm
                   for reopened_pad in reopened_pads)
        assert any(reopened_pad.position.x_nm == baseline_x
                   and any(prop.key == "backplane.footprint" and prop.value == "updated"
                           for prop in reopened_pad.custom_properties)
                   for reopened_pad in reopened_pads)
        api.request(project_commands.CloseDocument(document=reopened_library), Empty)
        native_doc = api.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_FOOTPRINT, path=str(copy_path)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        assert native_doc.type == types.DOCTYPE_FOOTPRINT
        assert native_doc.lib_id.entry_name
        native_items = api.request(
            editor.GetItems(
                header=types.ItemHeader(document=native_doc),
                types=[enums.KOT_PCB_PAD],
            ),
            editor.GetItemsResponse,
        )
        assert native_items.status == types.IRS_OK and native_items.items
        api.request(project_commands.CloseDocument(document=native_doc), Empty)
        api.request(project_commands.CloseAllDocuments(force=True), Empty)
        print("PASS footprint IPC: open/query/update/create/delete/native copy", flush=True)


if __name__ == "__main__":
    main()
