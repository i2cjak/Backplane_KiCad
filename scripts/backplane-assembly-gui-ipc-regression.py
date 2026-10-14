#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise assembly/viewer IPC against an already running PCB GUI."""

import argparse
import json
from pathlib import Path
import time

from backplane_ipc_test_support import IpcSession


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--socket", type=Path,
                        help="GUI API socket; omit to skip on headless CI")
    parser.add_argument("--board", required=True, type=Path)
    parser.add_argument("--manifest1", required=True, type=Path)
    parser.add_argument("--manifest2", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--xdisplay", action="store_true",
                        help="Capture a best-effort screen image using PIL ImageGrab")
    args = parser.parse_args()
    args.board = args.board.resolve()
    args.manifest1 = args.manifest1.resolve()
    args.manifest2 = args.manifest2.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    if args.socket is None:
        print("SKIP assembly GUI regression: no GUI API socket", flush=True)
        return

    with IpcSession(args.cli, api_socket=args.socket) as session:
        editor = session.module("common.commands.editor_commands")
        commands = session.module("backplane.assembly")
        types = session.module("common.types.base_types")

        opened = session.request(editor.GetOpenDocuments(type=types.DOCTYPE_PCB),
                                 editor.GetOpenDocumentsResponse)
        board_doc = next((doc for doc in opened.documents
                          if Path(doc.board_filename).name == args.board.name), None)
        assert board_doc is not None, f"GUI has no open PCB document for {args.board}"

        def call(request, response):
            return session.request(request, response)

        bad_doc = types.DocumentSpecifier(type=types.DOCTYPE_PCB,
                                          board_filename=str(args.board.with_name("missing.kicad_pcb")))
        raw = session.request_raw(commands.ListAssemblyTabs(document=bad_doc))
        assert raw.status.status in (session.envelope.AS_BAD_REQUEST, session.envelope.AS_UNHANDLED), raw

        for manifest in (args.manifest1, args.manifest2):
            result = call(commands.OpenAssemblyTab(document=board_doc, path=str(manifest)),
                          commands.AssemblyResponse)
            assert result.status == types.CS_OK, result
        tabs = call(commands.ListAssemblyTabs(document=board_doc), commands.AssemblyResponse)
        tab_data = json.loads(tabs.result_json)
        assert len(tab_data) >= 2 and len({str(item.get("path", item.get("manifest", item)))
                                           for item in tab_data}) >= 2, tab_data

        indices = [item["index"] for item in tab_data
                   if Path(item["path"]).resolve() in (args.manifest1, args.manifest2)]
        assert len(indices) == 2, tab_data
        for index in indices:
            result = call(commands.ShowAssembly3D(document=board_doc, tab_index=index),
                          commands.AssemblyResponse)
            assert result.status == types.CS_OK, result
            shown = json.loads(result.result_json)
            assert next(item for item in shown if item["index"] == index)["unfolded_step"]
            time.sleep(0.25)

        final_tabs = json.loads(call(commands.ListAssemblyTabs(document=board_doc), commands.AssemblyResponse).result_json)
        assert all(next(t for t in final_tabs if t["index"] == i)["unfolded_step"] for i in indices)
        if args.xdisplay:
            try:
                from PIL import ImageGrab
                ImageGrab.grab().save(args.output_dir / "assembly-gui.png")
            except Exception as error:  # screen capture is optional on headless CI
                (args.output_dir / "screenshot-error.txt").write_text(str(error))
        (args.output_dir / "gui-ipc-results.json").write_text(json.dumps({
            "tabs": tab_data,
        }, indent=2) + "\n")
        print("PASS GUI assembly wrong-document rejection, two tabs, and unfolded STEP preparation",
              flush=True)


if __name__ == "__main__":
    main()
