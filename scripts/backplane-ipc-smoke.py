#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise headless PCB/schematic IPC on disposable copies of KiCad fixtures.

Requires protoc, protobuf==5.29.6, and pynng==0.9.0. Protocol bindings are
generated from this checkout so a newer client's schema cannot mask omissions.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import pynng
from google.protobuf.empty_pb2 import Empty


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    cli = args.kicad_cli.resolve(strict=True)
    source = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="backplane-ipc-") as temporary:
        root = Path(temporary)
        bindings = root / "bindings"
        bindings.mkdir()
        proto = source / "api/proto"
        subprocess.run(
            ["protoc", f"-I{proto}", f"--python_out={bindings}",
             *map(str, sorted(proto.rglob("*.proto")))],
            check=True,
        )
        sys.path.insert(0, str(bindings))
        from common import envelope_pb2 as envelope
        from common.commands import base_commands_pb2 as base
        from common.commands import editor_commands_pb2 as editor
        from common.commands import project_commands_pb2 as project
        from common.types import base_types_pb2 as types
        from common.types import enums_pb2 as enums
        from board import board_types_pb2 as board
        from schematic import schematic_types_pb2 as schematic
        from schematic import schematic_commands_pb2 as sch_commands

        design = root / "project"
        shutil.copytree(source / "qa/data/libraries/test_project", design)
        socket_path = root / "api.sock"
        env = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"),
                   KICAD_CONFIG_HOME=str(root / "config/kicad"))
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)

        with (root / "server.log").open("w+") as log:
            server = subprocess.Popen(
                [str(cli), "api-server", "--socket", str(socket_path)],
                env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 20
                while not socket_path.is_socket():
                    if server.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Headless API server did not start")
                    time.sleep(0.05)

                with pynng.Req0(dial=f"ipc://{socket_path}", block_on_dial=True,
                                send_timeout=15000, recv_timeout=15000) as connection:
                    token = ""

                    def request(command, response_type):
                        nonlocal token
                        message = envelope.ApiRequest()
                        message.header.client_name = "works.backplane.release-smoke"
                        message.header.kicad_token = token
                        message.message.Pack(command)
                        connection.send(message.SerializeToString())
                        response = envelope.ApiResponse.FromString(connection.recv())
                        if response.status.status != envelope.AS_OK:
                            raise RuntimeError(f"{command.DESCRIPTOR.full_name}: {response.status}")
                        token = response.header.kicad_token
                        result = response_type()
                        if not response.message.Unpack(result):
                            raise RuntimeError(f"Unexpected reply to {command.DESCRIPTOR.full_name}")
                        return result

                    request(base.Ping(), Empty)
                    version = request(base.GetVersion(), base.GetVersionResponse)
                    print(f"Runtime: {version.version}", flush=True)

                    for kind, extension, item_type, make_item in (
                        (types.DOCTYPE_PCB, "kicad_pcb", enums.KOT_PCB_TRACE,
                         lambda: board.Track(
                             start=types.Vector2(x_nm=10000000, y_nm=10000000),
                             end=types.Vector2(x_nm=20000000, y_nm=10000000),
                             width=types.Distance(value_nm=250000), layer=board.BL_F_Cu)),
                        (types.DOCTYPE_SCHEMATIC, "kicad_sch", enums.KOT_SCH_TEXT,
                         lambda: schematic.SchematicText(text=types.Text(
                             text="Backplane IPC release smoke",
                             position=types.Vector2(x_nm=10000000, y_nm=10000000),
                             attributes=types.TextAttributes(
                                 size=types.Vector2(x_nm=1270000, y_nm=1270000))))),
                    ):
                        path = design / f"test_project.{extension}"

                        def open_document():
                            return request(project.OpenDocument(type=kind, path=str(path)),
                                           project.OpenDocumentResponse).document

                        document = open_document()
                        opened = request(editor.GetOpenDocuments(type=kind),
                                         editor.GetOpenDocumentsResponse)
                        assert len(opened.documents) == 1, opened
                        header = types.ItemHeader(document=document)

                        def items():
                            result = request(editor.GetItems(header=header, types=[item_type]),
                                             editor.GetItemsResponse)
                            assert result.status == types.IRS_OK, result
                            return result.items

                        count = len(items())
                        for action in (editor.CMA_DROP, editor.CMA_COMMIT):
                            transaction = request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                            create = editor.CreateItems(header=header)
                            create.items.add().Pack(make_item())
                            result = request(create, editor.CreateItemsResponse)
                            assert result.status == types.IRS_OK, result
                            assert len(result.created_items) == 1, result
                            assert result.created_items[0].status.code == editor.ISC_OK, result
                            request(editor.EndCommit(header=header, id=transaction.id, action=action,
                                                     message="Disposable IPC smoke"),
                                    editor.EndCommitResponse)
                            expected = count + (action == editor.CMA_COMMIT)
                            assert len(items()) == expected, "Commit/rollback changed item count"

                        title = request(editor.GetTitleBlockInfo(document=document),
                                        types.TitleBlockInfo)
                        title.title = "Backplane IPC smoke"
                        request(editor.SetTitleBlockInfo(document=document, title_block=title), Empty)
                        request(project.SaveDocument(document=document), Empty)
                        request(project.CloseDocument(document=document), Empty)
                        document = open_document()
                        header = types.ItemHeader(document=document)
                        assert len(items()) == count + 1, "Saved item did not survive reopening"
                        title = request(editor.GetTitleBlockInfo(document=document),
                                        types.TitleBlockInfo)
                        assert title.title == "Backplane IPC smoke", "Saved title did not survive reopening"
                        request(project.CloseDocument(document=document), Empty)
                        print(f"{extension}: open, query, create, rollback, commit, save, reopen, close passed",
                              flush=True)

                    populated = root / "ecc83"
                    shutil.copytree(source / "demos/ecc83", populated)
                    sch_path = populated / "ecc83-pp.kicad_sch"
                    document = request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC,
                                                            path=str(sch_path)),
                                       project.OpenDocumentResponse).document
                    header = types.ItemHeader(document=document)
                    symbols = request(editor.GetItems(header=header, types=[enums.KOT_SCH_SYMBOL]),
                                      editor.GetItemsResponse)
                    assert symbols.items, "Populated schematic returned no symbols"
                    hierarchy = request(sch_commands.GetSchematicHierarchy(document=document),
                                        sch_commands.SchematicHierarchyResponse)
                    assert hierarchy.top_level_sheets, "Schematic hierarchy is empty"
                    netlist = request(sch_commands.GetSchematicNetlist(document=document),
                                      sch_commands.SchematicNetlistResponse)
                    assert netlist.nets, "Populated schematic returned no nets"
                    symbol = schematic.SchematicSymbolInstance()
                    assert symbols.items[0].Unpack(symbol)
                    original_x = symbol.position.x_nm

                    def read_symbol():
                        result = request(editor.GetItemsById(header=header, items=[symbol.id]),
                                         editor.GetItemsResponse)
                        assert len(result.items) == 1, result
                        value = schematic.SchematicSymbolInstance()
                        assert result.items[0].Unpack(value)
                        return value

                    for action in (editor.CMA_DROP, editor.CMA_COMMIT):
                        transaction = request(editor.BeginCommit(header=header), editor.BeginCommitResponse)
                        symbol.position.x_nm = original_x + 1000000
                        update = editor.UpdateItems(header=header)
                        update.items.add().Pack(symbol)
                        result = request(update, editor.UpdateItemsResponse)
                        assert result.status == types.IRS_OK, result
                        assert len(result.updated_items) == 1, result
                        assert result.updated_items[0].status.code == editor.ISC_OK, result
                        request(editor.EndCommit(header=header, id=transaction.id, action=action,
                                                 message="Disposable symbol move"),
                                editor.EndCommitResponse)
                        expected_x = original_x + (1000000 if action == editor.CMA_COMMIT else 0)
                        assert read_symbol().position.x_nm == expected_x, "Symbol commit/rollback failed"

                    request(project.SaveDocument(document=document), Empty)
                    request(project.CloseDocument(document=document), Empty)
                    document = request(project.OpenDocument(type=types.DOCTYPE_SCHEMATIC,
                                                            path=str(sch_path)),
                                       project.OpenDocumentResponse).document
                    header = types.ItemHeader(document=document)
                    assert read_symbol().position.x_nm == original_x + 1000000, "Symbol move did not persist"
                    request(project.CloseDocument(document=document), Empty)
                    print(f"Schematic symbols: {len(symbols.items)}, nets: {len(netlist.nets)}; "
                          "query, update, rollback, save and reopen passed", flush=True)
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()


if __name__ == "__main__":
    main()
