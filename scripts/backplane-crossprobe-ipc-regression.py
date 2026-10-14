#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise cross-probe selection against the receiving headless document.

Cross-probe commands do not carry a document specifier.  The checks therefore
open one PCB or schematic document at a time and verify that the receiver
resolves references in that document instead of forwarding the request to a
different editor.  Negative responses are checked at both the envelope and
cross-probe status levels.
"""

import argparse
from pathlib import Path

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def unpack(any_item, module_name, api):
    message = api.module(module_name)
    name = any_item.TypeName().rsplit(".", 1)[-1]
    item = getattr(message, name)()
    assert any_item.Unpack(item), any_item.type_url
    return item


def response_for(api, command):
    response = api.request_raw(command)
    assert response.status.status == api.envelope.AS_OK, response.status
    return response


def cross_response(api, command, response_type, expected):
    response = response_for(api, command)
    result = response_type()
    assert response.message.Unpack(result), response.message.type_url
    assert result.status == expected, result
    return result


def announce(api, cross, types, frame_type):
    result = cross_response(
        api,
        cross.CrossProbeAnnounce(
            frame_type=frame_type,
            socket_path="ipc:///tmp/backplane-crossprobe-peer.sock",
        ),
        cross.CrossProbeAnnounceResponse,
        cross.CPS_OK,
    )
    assert result.message == "", result


def check_headless_unsupported(api, cross):
    response = api.request_raw(cross.HighlightNets(net_name=["N$1"]))
    assert response.status.status == api.envelope.AS_UNIMPLEMENTED, response.status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()

    with IpcSession(args.kicad_cli) as api:
        project = api.module("common.commands.project_commands")
        editor = api.module("common.commands.editor_commands")
        types = api.module("common.types.base_types")
        enums = api.module("common.types.enums")
        board = api.module("board.board_types")
        schematic = api.module("schematic.schematic_types")
        cross = api.module("common.commands.cross_probe_commands")

        design = api.copy_project("demos/ecc83", "crossprobe")
        pcb_path = design / "ecc83-pp.kicad_pcb"
        sch_path = design / "ecc83-pp.kicad_sch"

        pcb = api.request(
            project.OpenDocument(type=types.DOCTYPE_PCB, path=str(pcb_path)),
            project.OpenDocumentResponse,
        ).document
        # The announce command is a real peer registration path.  Send it
        # after opening the receiver so the domain handler is available.
        announce(api, cross, types, types.FT_PCB_EDITOR)
        header = types.ItemHeader(document=pcb)
        items = api.request(
            editor.GetItems(header=header, types=[enums.KOT_PCB_FOOTPRINT]),
            editor.GetItemsResponse,
        )
        assert items.items, "fixture has no PCB footprints"
        footprint = unpack(items.items[0], "board.board_types", api)
        reference = footprint.reference_field.text.text.text

        valid = cross.SelectionSpec(
            footprint=cross.FootprintSelectionSpec(reference=reference)
        )
        cross_response(
            api,
            cross.SyncSelection(
                context=cross.SSC_EXPLICIT,
                mode=cross.SSM_ITEMS_ONLY,
                items=[valid],
            ),
            cross.SyncSelectionResponse,
            cross.CPS_OK,
        )
        selected = api.request(editor.GetSelection(header=header), editor.SelectionResponse)
        assert len(selected.items) == 1, selected
        selected_footprint = unpack(selected.items[0], "board.board_types", api)
        assert selected_footprint.id.value == footprint.id.value

        # Empty selection is a meaningful clear operation.
        cross_response(
            api,
            cross.SyncSelection(context=cross.SSC_EXPLICIT, mode=cross.SSM_ITEMS_ONLY),
            cross.SyncSelectionResponse,
            cross.CPS_OK,
        )
        assert not api.request(editor.GetSelection(header=header), editor.SelectionResponse).items

        # An unknown target is a valid request with a precise not-found result.
        missing = cross.SelectionSpec(
            footprint=cross.FootprintSelectionSpec(reference="BACKPLANE_MISSING")
        )
        cross_response(
            api,
            cross.SyncSelection(items=[missing]),
            cross.SyncSelectionResponse,
            cross.CPS_NOT_FOUND,
        )
        cross_response(
            api,
            cross.FocusOnItem(),
            cross.FocusOnItemResponse,
            cross.CPS_NOT_FOUND,
        )
        response = api.request_raw(cross.HighlightNets(net_name=[]))
        # Headless net highlighting is deliberately unsupported, including an
        # empty request; this must be explicit rather than a fake success.
        assert response.status.status == api.envelope.AS_UNIMPLEMENTED, response.status
        malformed = api.request_raw(
            project.OpenDocument(type=types.DOCTYPE_PCB, path=str(design / "invalid.kicad_pcb"))
        )
        assert malformed.status.status == api.envelope.AS_BAD_REQUEST, malformed.status
        check_headless_unsupported(api, cross)
        api.request(project.CloseDocument(document=pcb), Empty)
        print("PASS PCB cross-probe announce, own selection, empty, missing and headless status", flush=True)

        sch = api.request(
            project.OpenDocument(type=types.DOCTYPE_SCHEMATIC, path=str(sch_path)),
            project.OpenDocumentResponse,
        ).document
        announce(api, cross, types, types.FT_SCHEMATIC_EDITOR)
        header = types.ItemHeader(document=sch)
        items = api.request(
            editor.GetItems(header=header, types=[enums.KOT_SCH_SYMBOL]),
            editor.GetItemsResponse,
        )
        assert items.items, "fixture has no schematic symbols"
        symbols = [unpack(item, "schematic.schematic_types", api) for item in items.items]
        symbol = next(
            item for item in symbols if item.reference_field.text.text in ("R1", "R2")
        )
        reference = symbol.reference_field.text.text
        valid = cross.SelectionSpec(
            footprint=cross.FootprintSelectionSpec(reference=reference)
        )
        cross_response(
            api,
            cross.SyncSelection(
                context=cross.SSC_EXPLICIT,
                mode=cross.SSM_ITEMS_ONLY,
                items=[valid],
            ),
            cross.SyncSelectionResponse,
            cross.CPS_OK,
        )
        selected = api.request(editor.GetSelection(header=header), editor.SelectionResponse)
        assert len(selected.items) == 1, selected
        selected_symbol = unpack(selected.items[0], "schematic.schematic_types", api)
        assert selected_symbol.id.value == symbol.id.value
        cross_response(
            api,
            cross.SyncSelection(items=[]),
            cross.SyncSelectionResponse,
            cross.CPS_OK,
        )
        assert not api.request(editor.GetSelection(header=header), editor.SelectionResponse).items
        cross_response(
            api,
            cross.SyncSelection(
                items=[cross.SelectionSpec(
                    footprint=cross.FootprintSelectionSpec(reference="BACKPLANE_MISSING")
                )]
            ),
            cross.SyncSelectionResponse,
            cross.CPS_NOT_FOUND,
        )
        check_headless_unsupported(api, cross)
        api.request(project.CloseDocument(document=sch), Empty)
        print("PASS schematic cross-probe own selection, empty, missing and headless status", flush=True)


if __name__ == "__main__":
    main()
