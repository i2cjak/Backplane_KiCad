#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify embedded payload access, atomic replacement, and native persistence."""

import argparse
from pathlib import Path
import shutil
import subprocess

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path)
    args = parser.parse_args()
    if args.stock_cli:
        args.stock_cli = args.stock_cli.resolve(strict=True)
        assert subprocess.check_output([str(args.stock_cli), "--version"], text=True).strip() == "10.0.6"
    with IpcSession(args.kicad_cli) as api:
        project = api.module("common.commands.project_commands")
        commands = api.module("board.board_commands")
        files = api.module("common.types.embedded_files")
        types = api.module("common.types.base_types")
        path = api.root / "embedded.kicad_pcb"
        shutil.copy2(api.source / "qa/data/pcbnew/issue22102.kicad_pcb", path)

        def open_document():
            return api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(path)),
                               project.OpenDocumentResponse).document

        document = open_document()

        def read():
            return api.request(commands.GetEmbeddedFiles(board=document), files.EmbeddedFiles)

        original = read()
        assert original.files, "Fixture must contain real embedded payloads"
        added = files.EmbeddedFile()
        added.CopyFrom(original.files[0])
        added.name = "Backplane-embedded-copy.kicad_wks"
        api.request(commands.AddEmbeddedFiles(board=document, files=files.EmbeddedFiles(files=[added])), Empty)
        assert {f.name for f in read().files} == {f.name for f in original.files} | {added.name}
        replacement = files.EmbeddedFiles(files=[added])
        api.request(commands.SetEmbeddedFiles(board=document, files=replacement), Empty)
        assert read() == replacement

        invalid = files.EmbeddedFiles()
        invalid.CopyFrom(original)
        invalid.files[-1].data_hash = "invalid-checksum"
        request = api.envelope.ApiRequest()
        request.header.client_name = "works.backplane.embedded-regression"
        request.header.kicad_token = api.token
        request.message.Pack(commands.SetEmbeddedFiles(board=document, files=invalid))
        api.connection.send(request.SerializeToString())
        response = api.envelope.ApiResponse.FromString(api.connection.recv())
        api.token = response.header.kicad_token
        assert response.status.status == api.envelope.AS_BAD_REQUEST, response
        assert read() == replacement, "Invalid replacement changed existing embedded files"

        api.request(project.SaveDocument(document=document), Empty)
        api.request(project.CloseDocument(document=document), Empty)
        if args.stock_cli:
            result = subprocess.run([str(args.stock_cli), "pcb", "upgrade", "--force", str(path)],
                                    env=api.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=60)
            assert result.returncode == 0, result.stdout
        document = open_document()
        assert read() == replacement, "Embedded data/hash changed after save/reopen"
        api.request(commands.SetEmbeddedFiles(board=document, files=files.EmbeddedFiles()), Empty)
        api.request(project.SaveDocument(document=document), Empty)
        api.request(project.CloseDocument(document=document), Empty)
        document = open_document()
        assert not read().files, "Cleared embedded files returned after reopen"
        api.request(project.CloseDocument(document=document), Empty)
        print("PASS embedded files: get/add/set, invalid replacement atomicity, save/reopen", flush=True)


if __name__ == "__main__":
    main()
