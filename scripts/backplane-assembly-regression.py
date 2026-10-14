#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise multi-board assembly IPC through an isolated headless server."""

import argparse
import json
from pathlib import Path

from backplane_ipc_test_support import IpcSession


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()

    with IpcSession(args.kicad_cli) as api:
        project = api.module("common.commands.project_commands")
        types = api.module("common.types.base_types")
        assembly = api.module("backplane.assembly")
        board_path = api.source / "qa/data/pcbnew/api_kitchen_sink.kicad_pcb"
        document = api.request(project.OpenDocument(type=types.DOCTYPE_PCB, path=str(board_path)),
                               project.OpenDocumentResponse).document

        listed = api.request(assembly.ListAssemblyComponents(
            manifest_json=json.dumps({
                "schema": 1,
                "name": "ipc assembly",
                "anchor": "left",
                "instances": [
                    {"id": "left", "source": str(board_path), "kind": "board"},
                    {"id": "right", "source": str(board_path), "kind": "board",
                     "transform": {"translation": [100, 0, 0], "rotation": [0, 0, 0]}},
                ],
                "mates": [],
            }),
            instance_id="left", document=document), assembly.AssemblyResponse)
        components = json.loads(listed.result_json)["components"]
        assert components, "assembly fixture has no footprints"

        endpoint = next((
            (component["reference"], pad["number"])
            for component in components
            for pad in component.get("pads", [])
            if pad.get("net_name")
        ), None)
        assert endpoint, "assembly fixture has no named pad"
        reference, pad = endpoint
        manifest = {
            "schema": 1,
            "name": "headless assembly regression",
            "anchor": "left",
            "instances": [
                {"id": "left", "source": str(board_path), "kind": "board"},
                {"id": "right", "source": str(board_path), "kind": "board",
                 "transform": {"translation": [100, 0, 0], "rotation": [0, 0, 0]}},
            ],
            "mates": [{
                "id": "right-location", "type": "point",
                "first_instance": "left", "second_instance": "right",
                "first": [100, 0, 0], "second": [0, 0, 0],
            }],
            "electrical": {"links": [{
                "id": "named-net", "name": "BUS",
                "endpoints": [
                    {"instance": "left", "reference": reference, "pad": pad},
                    {"instance": "right", "reference": reference, "pad": pad},
                ],
            }]},
        }
        manifest_json = json.dumps(manifest)
        response = api.request(assembly.GetAssembly(manifest_json=manifest_json),
                               assembly.AssemblyResponse)
        normalized = json.loads(response.manifest_json)
        assert normalized["name"] == manifest["name"]

        saved_path = api.root / "assembly.backplane-assembly.json"
        saved = api.request(assembly.SaveAssembly(path=str(saved_path), manifest_json=response.manifest_json),
                            assembly.AssemblyResponse)
        assert Path(saved.path).is_file()

        resolved = api.request(assembly.ResolveAssemblyMates(path=str(saved_path)),
                               assembly.AssemblyResponse)
        assert json.loads(resolved.manifest_json)["instances"][1]["transform"]["translation"] == [100.0, 0.0, 0.0]

        collisions = api.request(assembly.CheckAssemblyCollisions(path=str(saved_path)),
                                 assembly.AssemblyResponse)
        assert collisions.status == types.CS_OK

        output = api.root / "assembly.step"
        exported = api.request(assembly.ExportAssemblyStep(path=str(saved_path), output_path=str(output)),
                               assembly.AssemblyResponse)
        assert exported.status == types.CS_OK and output.is_file()

        moved = api.request(assembly.MoveAssemblyComponent(
            path=str(saved_path), instance_id="left", reference=reference,
            x_mm=12.0, y_mm=13.0, rotation_deg=27.0, document=document),
                            assembly.AssemblyResponse)
        assert moved.status == types.CS_OK

        synced = api.request(assembly.SyncAssemblyElectrical(
            path=str(saved_path), save_snapshot=True, document=document),
                            assembly.AssemblyResponse)
        assert synced.status == types.CS_OK
        assert json.loads(synced.result_json)["links"]

        for command in (
            assembly.OpenAssemblyTab(document=document, path=str(saved_path)),
            assembly.ListAssemblyTabs(document=document),
            assembly.ShowAssembly3D(document=document, tab_index=0),
        ):
            raw = api.request_raw(command)
            assert raw.status.status == api.envelope.AS_UNIMPLEMENTED, raw

        print("PASS headless assembly manifest, mates, components, move, electrical sync, collision, STEP, and GUI skips",
              flush=True)


if __name__ == "__main__":
    main()
