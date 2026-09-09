#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise headless document lifecycle and identity routing on disposable data.

This regression intentionally uses the checked-out protobuf schema through
``IpcSession``.  It covers the common project/document lifetime contract and a
PCB commit because a PCB gives the test a small, deterministic mutable object.
The source and destination companion files are checked when the runtime has
the Backplane metadata hooks installed.

Requires protoc, protobuf, and pynng.  The first argument is the kicad-cli
executable built from this checkout.
"""

import argparse
import json
import shutil
import uuid
from pathlib import Path

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def exchange(session, command):
    """Send a request while retaining the response for negative assertions."""
    request = session.envelope.ApiRequest()
    request.header.client_name = "works.backplane.document-ipc-regression"
    request.header.kicad_token = session.token
    request.message.Pack(command)
    session.connection.send(request.SerializeToString())
    response = session.envelope.ApiResponse.FromString(session.connection.recv())
    session.token = response.header.kicad_token
    return response


def expect_status(session, command, status):
    response = exchange(session, command)
    actual = response.status.status
    assert actual == status, f"{command.DESCRIPTOR.full_name}: {response.status}"


def clone_project(session, name):
    project = session.copy_project("qa/data/libraries/test_project", name)
    for suffix in (".kicad_pro", ".kicad_pcb", ".kicad_sch"):
        old = project / f"test_project{suffix}"
        old.rename(project / f"{name}{suffix}")
    return project


def install_regression_footprint_library(session, project):
    library = project / "Regression.pretty"
    library.mkdir()
    shutil.copy2(
        session.source / "qa/data/cli/fp_lib_test/Audio_Module.v9.pretty/Reverb_BTDR-1V.kicad_mod",
        library / "Reverb_BTDR-1V.kicad_mod",
    )
    (project / "fp-lib-table").write_text(
        """(fp_lib_table
  (version 7)
  (lib (name \"Regression\") (type \"KiCad\")
       (uri \"${KIPRJMOD}/Regression.pretty\") (options \"\")
       (descr \"Backplane document lifecycle regression footprint\"))
)\n""",
        encoding="utf-8",
    )


def document_modified(session, editor, document):
    result = session.request(
        editor.GetDocumentModifiedState(document=document),
        editor.GetDocumentModifiedStateResponse,
    )
    return result.state


def make_track(board, types):
    track_id = str(uuid.uuid4())
    track = board.Track(
        id=types.KIID(value=track_id),
        start=types.Vector2(x_nm=10000000, y_nm=10000000),
        end=types.Vector2(x_nm=20000000, y_nm=10000000),
        width=types.Distance(value_nm=250000),
        layer=board.BL_F_Cu,
    )
    track.custom_properties.add(key="backplane.regression", value=track_id)
    return track


def create_track(session, editor, board, types, document):
    track = make_track(board, types)
    header = types.ItemHeader(document=document)
    transaction = session.request(
        editor.BeginCommit(header=header), editor.BeginCommitResponse
    )
    create = editor.CreateItems(header=header)
    create.items.add().Pack(track)
    result = session.request(create, editor.CreateItemsResponse)
    assert result.status == types.IRS_OK, result
    assert len(result.created_items) == 1, result
    assert result.created_items[0].status.code == editor.ISC_OK, result
    return header, transaction.id, track.id.value


def save_copy_and_check_companion(
    session, project, pcb_document, saved_track_id, live_track_id, editor_commands
):
    source = project / f"{project.name}.kicad_pcb"
    source_companion = Path(str(source) + ".backplane.json")

    destination = project / "copy" / "document-copy.kicad_pcb"
    destination.parent.mkdir()
    relative_destination = destination.relative_to(project)
    session.request(
        editor_commands.SaveCopyOfDocument(
            document=pcb_document,
            path=str(relative_destination),
            options=editor_commands.SaveOptions(overwrite=True, include_project=True),
        ),
        Empty,
    )
    assert destination.is_file(), destination
    copied_project = destination.with_suffix(".kicad_pro")
    assert copied_project.is_file(), copied_project
    live_project = project / f"{project.name}.kicad_pro"
    assert live_project.is_file(), live_project
    copied_companion = Path(str(destination) + ".backplane.json")
    assert copied_companion.is_file(), (
        "SaveCopyOfDocument did not preserve the Backplane companion metadata: "
        f"{copied_companion}"
    )
    copied = json.loads(copied_companion.read_text(encoding="utf-8"))
    source = json.loads(source_companion.read_text(encoding="utf-8"))
    assert saved_track_id in source["items"]
    assert live_track_id not in source["items"]
    assert copied["items"][saved_track_id]["custom_properties"]["backplane.regression"] == saved_track_id
    assert copied["items"][live_track_id]["custom_properties"]["backplane.regression"] == live_track_id
    assert copied != source, "SaveCopyOfDocument copied stale source metadata"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()

    with IpcSession(args.kicad_cli) as session:
        from common.commands import editor_commands_pb2 as editor
        from common.commands import project_commands_pb2 as project_commands
        from common.types import base_types_pb2 as types
        from board import board_types_pb2 as board

        project_a = clone_project(session, "document-project-a")
        project_b = clone_project(session, "document-project-b")
        install_regression_footprint_library(session, project_b)
        native_footprint = project_b / "Standalone.kicad_mod"
        shutil.copy2(
            session.source / "qa/data/cli/fp_lib_test/Audio_Module.v9.pretty/Reverb_BTDR-1V.kicad_mod",
            native_footprint,
        )
        project_a_file = project_a / "document-project-a.kicad_pro"
        project_b_file = project_b / "document-project-b.kicad_pro"
        pcb_a_file = project_a / "document-project-a.kicad_pcb"
        sch_a_file = project_a / "document-project-a.kicad_sch"
        session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT, path=str(project_a_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        pcb_a_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PCB, path=str(pcb_a_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        sch_a_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_SCHEMATIC, path=str(sch_a_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document

        pcb_documents = session.request(
            editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
            editor.GetOpenDocumentsResponse,
        )
        sch_documents = session.request(
            editor.GetOpenDocuments(type=types.DOCTYPE_SCHEMATIC),
            editor.GetOpenDocumentsResponse,
        )
        assert len(pcb_documents.documents) == 1, pcb_documents
        assert len(sch_documents.documents) == 1, sch_documents
        assert pcb_documents.documents[0] == pcb_a_document
        assert sch_documents.documents[0] == sch_a_document

        # A second project cannot be selected while this project's children are open.
        expect_status(
            session,
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT, path=str(project_b_file)
            ),
            session.envelope.AS_BAD_REQUEST,
        )

        # A same-type handler must decline an unrelated document so dispatch can continue.
        wrong_pcb = types.DocumentSpecifier(
            type=types.DOCTYPE_PCB, board_filename="unrelated.kicad_pcb"
        )
        expect_status(
            session,
            editor.GetDocumentModifiedState(document=wrong_pcb),
            session.envelope.AS_UNHANDLED,
        )

        assert document_modified(session, editor, pcb_a_document) == editor.DMS_UNMODIFIED
        header, transaction_id, _ = create_track(
            session, editor, board, types, pcb_a_document
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_MODIFIED
        # An empty commit from another client must not turn the first client's
        # pending transaction into a permanent dirty flag after its rollback.
        empty_response = exchange(session, editor.BeginCommit(header=header))
        assert empty_response.status.status == session.envelope.AS_OK, empty_response
        empty_commit = editor.BeginCommitResponse()
        assert empty_response.message.Unpack(empty_commit)
        expect_status(
            session,
            editor.EndCommit(header=header, id=empty_commit.id, action=editor.CMA_COMMIT),
            session.envelope.AS_OK,
        )
        expect_status(
            session,
            project_commands.CloseDocument(document=pcb_a_document),
            session.envelope.AS_BAD_REQUEST,
        )
        expect_status(
            session,
            project_commands.CloseAllDocuments(force=False),
            session.envelope.AS_BAD_REQUEST,
        )
        session.request(
            editor.EndCommit(
                header=header,
                id=transaction_id,
                action=editor.CMA_DROP,
                message="document lifecycle rollback",
            ),
            editor.EndCommitResponse,
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_UNMODIFIED

        header, transaction_id, track_id = create_track(
            session, editor, board, types, pcb_a_document
        )
        session.request(
            editor.EndCommit(
                header=header,
                id=transaction_id,
                action=editor.CMA_COMMIT,
                message="document lifecycle commit",
            ),
            editor.EndCommitResponse,
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_MODIFIED
        session.request(
            project_commands.SaveDocument(document=pcb_a_document), Empty
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_UNMODIFIED

        # Add a second live item after saving the source.  SaveCopy must capture this
        # in-memory item rather than copying the source companion verbatim.
        header, transaction_id, live_track_id = create_track(
            session, editor, board, types, pcb_a_document
        )
        session.request(
            editor.EndCommit(
                header=header,
                id=transaction_id,
                action=editor.CMA_COMMIT,
                message="live save-copy metadata",
            ),
            editor.EndCommitResponse,
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_MODIFIED
        save_copy_and_check_companion(
            session, project_a, pcb_a_document, track_id, live_track_id, editor,
        )

        # A dirty document must refuse a non-forced close and remain available for the
        # caller to save or explicitly discard.  Forced close still closes children before
        # unloading the project, and the saved copy remains available afterward.
        expect_status(
            session,
            project_commands.CloseAllDocuments(force=False),
            session.envelope.AS_BAD_REQUEST,
        )
        assert document_modified(session, editor, pcb_a_document) == editor.DMS_MODIFIED
        session.request(project_commands.CloseAllDocuments(force=True), Empty)

        # The old project is gone, so the second project can now be loaded.
        project_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT, path=str(project_b_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        assert project_b_document.project.name == "document-project-b"
        pcb_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PCB,
                path=str(project_b / "document-project-b.kicad_pcb"),
            ),
            project_commands.OpenDocumentResponse,
        ).document
        sch_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_SCHEMATIC,
                path=str(project_b / "document-project-b.kicad_sch"),
            ),
            project_commands.OpenDocumentResponse,
        ).document

        # A schematic close request from another project is rejected by identity checks.
        wrong_project_sch = types.DocumentSpecifier(
            type=types.DOCTYPE_SCHEMATIC,
            project=types.ProjectSpecifier(name="not-open", path="/not-open"),
        )
        expect_status(
            session,
            project_commands.CloseDocument(document=wrong_project_sch),
            session.envelope.AS_BAD_REQUEST,
        )
        session.request(project_commands.CloseDocument(document=pcb_b_document), Empty)
        session.request(project_commands.CloseDocument(document=sch_b_document), Empty)
        session.request(project_commands.CloseDocument(document=project_b_document), Empty)

        # Footprint Editor documents share the PCB face but have a library
        # identity instead of a board filename.  Exercise their headless
        # open/query/close lifecycle independently from board documents.
        project_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT, path=str(project_b_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        footprint_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_FOOTPRINT, path="Regression:Reverb_BTDR-1V"
            ),
            project_commands.OpenDocumentResponse,
        ).document
        assert footprint_document.lib_id.entry_name == "Reverb_BTDR-1V"
        footprint_documents = session.request(
            editor.GetOpenDocuments(type=types.DOCTYPE_FOOTPRINT),
            editor.GetOpenDocumentsResponse,
        )
        assert footprint_documents.documents == [footprint_document]
        assert document_modified(session, editor, footprint_document) == editor.DMS_UNMODIFIED
        session.request(project_commands.CloseDocument(document=footprint_document), Empty)

        # A native .kicad_mod is also a first-class footprint document.  Its
        # filename is retained by HEADLESS_FOOTPRINT_CONTEXT so SaveDocument
        # and modified-state routing use the file identity rather than a
        # library nickname.
        native_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_FOOTPRINT, path=str(native_footprint)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        assert native_document.lib_id.entry_name == native_footprint.stem
        assert native_document.lib_id.library_nickname == ""
        assert document_modified(session, editor, native_document) == editor.DMS_UNMODIFIED
        session.request(project_commands.CloseDocument(document=native_document), Empty)
        session.request(project_commands.CloseDocument(document=project_b_document), Empty)

        # Exercise the force=true spelling on a dirty child as well.
        project_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PROJECT, path=str(project_b_file)
            ),
            project_commands.OpenDocumentResponse,
        ).document
        pcb_b_document = session.request(
            project_commands.OpenDocument(
                type=types.DOCTYPE_PCB,
                path=str(project_b / "document-project-b.kicad_pcb"),
            ),
            project_commands.OpenDocumentResponse,
        ).document
        header, transaction_id, _ = create_track(
            session, editor, board, types, pcb_b_document
        )
        session.request(
            editor.EndCommit(
                header=header,
                id=transaction_id,
                action=editor.CMA_COMMIT,
                message="forced dirty close-all document",
            ),
            editor.EndCommitResponse,
        )
        assert document_modified(session, editor, pcb_b_document) == editor.DMS_MODIFIED
        session.request(project_commands.CloseAllDocuments(force=True), Empty)

        print("document IPC regression: lifecycle, identity, commits, save-copy, and close-all passed")


if __name__ == "__main__":
    main()
