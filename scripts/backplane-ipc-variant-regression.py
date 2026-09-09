#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise PCB/schematic variant commands and stock 10.0.6 persistence."""

import argparse
from pathlib import Path
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
        variants = api.module("common.commands.variant_commands")
        editor = api.module("common.commands.editor_commands")
        project = api.module("common.commands.project_commands")
        types = api.module("common.types.base_types")
        fixture = api.copy_project("qa/data/libraries/test_project", "variants")
        for kind, extension, tool in ((types.DOCTYPE_PCB, "kicad_pcb", "pcb"),
                                     (types.DOCTYPE_SCHEMATIC, "kicad_sch", "sch")):
            path = fixture / f"test_project.{extension}"

            def open_document():
                return api.request(project.OpenDocument(type=kind, path=str(path)),
                                   project.OpenDocumentResponse).document

            document = open_document()

            def request(command, response=Empty, **fields):
                return api.request(command(document=document, **fields), response)

            def all_variants():
                return {variant.name: variant.description for variant in
                        request(variants.GetVariants, variants.VariantsResponse).variants}

            original = all_variants()
            request(variants.AddVariant, name="Backplane primary", description="Primary assembly")
            request(variants.AddVariant, name="Backplane empty")
            assert all_variants() == {**original, "Backplane primary": "Primary assembly", "Backplane empty": ""}
            request(variants.CopyVariant, old_name="BACKPLANE PRIMARY", new_name="Backplane copy",
                    new_description="Copied assembly")
            assert all_variants()["Backplane copy"] == "Copied assembly"
            request(variants.SetVariantDescription, name="BACKPLANE COPY", description="")
            request(variants.RenameVariant, old_name="backplane copy", new_name="Backplane renamed")
            request(variants.SetCurrentVariant, name="Backplane renamed")
            assert request(variants.GetCurrentVariant, variants.CurrentVariantResponse).name == "Backplane renamed"
            request(variants.DeleteVariant, name="BACKPLANE RENAMED")
            assert not request(variants.GetCurrentVariant, variants.CurrentVariantResponse).HasField("name")
            assert request(editor.GetDocumentModifiedState, editor.GetDocumentModifiedStateResponse).state == editor.DMS_MODIFIED
            expected = {**original, "Backplane primary": "Primary assembly", "Backplane empty": ""}
            assert all_variants() == expected
            request(project.SaveDocument)
            assert request(editor.GetDocumentModifiedState, editor.GetDocumentModifiedStateResponse).state == editor.DMS_UNMODIFIED
            request(project.CloseDocument)
            if args.stock_cli:
                result = subprocess.run([str(args.stock_cli), tool, "upgrade", "--force", str(path)],
                                        env=api.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=60)
                assert result.returncode == 0, result.stdout
            document = open_document()
            assert all_variants() == expected, all_variants()
            request(variants.DeleteVariant, name="Backplane primary")
            request(variants.DeleteVariant, name="Backplane empty")
            request(project.SaveDocument)
            request(project.CloseDocument)
            document = open_document()
            assert all_variants() == original
            request(project.CloseDocument)
            print(f"PASS {tool}: variant commands, descriptions, modified state, save/reopen", flush=True)


if __name__ == "__main__":
    main()
