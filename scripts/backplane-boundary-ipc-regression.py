#!/usr/bin/env python3
"""Regression coverage for atomic rigid-flex partition derivation over IPC."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from google.protobuf.empty_pb2 import Empty

from backplane_ipc_test_support import IpcSession
from backplane_structure_ipc import BackplaneIpc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    parser.add_argument("--stock-cli", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        help="Keep stock/fork Gerbers in this directory")
    args = parser.parse_args()
    fork = args.kicad_cli.resolve(strict=True)
    stock = args.stock_cli.resolve(strict=True) if args.stock_cli else None

    with IpcSession(fork) as session:
        bp = BackplaneIpc(session)
        project = bp.project
        types = bp.types
        source = session.source / "demos/backplane-structure"
        work = session.root / "boundary-regression"
        shutil.copytree(source, work)
        board_path = work / "rigid-flex-bridge.kicad_pcb"
        board = session.request(project.OpenDocument(
            type=types.DOCTYPE_PCB, path=str(board_path)),
            project.OpenDocumentResponse).document
        artifacts = (args.output_dir.resolve() if args.output_dir
                     else session.root / "artifacts")
        artifacts.mkdir(parents=True, exist_ok=True)

        before = bp.json_get("GetBoardStructure", "BoardStructureResponse", board)
        assert before.valid and before.json
        base = json.loads(before.json)

        # A vertical crosscut joining the Edge.Cuts boundary at both ends
        # splits the existing flex region and is accepted atomically.
        candidate = json.loads(before.json)
        # Derivation starts from one seeded whole-board region.  This avoids
        # asking the partitioner to merge the three pre-derived demo regions
        # while still reusing the fixture's real stack definition.
        candidate["regions"] = [{
            "id": "whole",
            "name": "Whole board",
            "stack_id": "rigid",
            "shape": {"outline": [[20000000, 20000000], [100000000, 20000000],
                                  [100000000, 40000000], [20000000, 40000000]],
                      "holes": []},
        }]
        candidate["region_seeds"] = [{"region_id": "whole", "point": [25000000, 30000000]}]
        candidate["coverlays"] = []
        candidate["bends"] = []
        candidate["stiffeners"] = []
        candidate["boundaries"] = [{
            "id": "ipc-crosscut",
            "polyline": [[55000000, 20000000], [55000000, 40000000]],
        }]
        candidate_json = json.dumps(candidate, sort_keys=True,
                                    separators=(",", ":"))
        checked = bp.call("DeriveBoardStructurePartition", "BoardStructureResponse",
                          board=board, json=candidate_json, validate_only=True)
        derived = json.loads(checked.json)
        assert checked.valid and len(derived["regions"]) >= 2, checked
        assert json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse",
                                      board).json) == base

        # A cut wholly inside the board is dangling.  The failed request must
        # leave the current structure untouched as well.
        invalid = json.loads(candidate_json)
        invalid["boundaries"] = [{
            "id": "ipc-dangling",
            "polyline": [[60000000, 25000000], [60000000, 35000000]],
        }]
        rejected = bp.expect_bad_request("DeriveBoardStructurePartition", board=board,
                                         json=json.dumps(invalid), validate_only=False)
        assert "boundary" in rejected.status.error_message.lower()
        assert json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse",
                                      board).json) == base

        # Commit the valid candidate, save its companion, and verify a native
        # close/reopen retains the derived partition.
        committed = bp.call("DeriveBoardStructurePartition", "BoardStructureResponse",
                            board=board, json=candidate_json, validate_only=False)
        assert committed.valid and len(json.loads(committed.json)["regions"]) >= 2
        session.request(project.SaveDocument(document=board), Empty)
        session.request(project.CloseDocument(document=board), Empty)
        board = session.request(project.OpenDocument(
            type=types.DOCTYPE_PCB, path=str(board_path)),
            project.OpenDocumentResponse).document
        reopened = json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse",
                                          board).json)
        assert len(reopened["regions"]) >= 2
        assert any(item["id"] == "ipc-crosscut" for item in reopened["boundaries"])

        if stock:
            stock_output = artifacts / "stock-gerbers"
            stock_env = dict(session.env)
            for variable in ("LD_LIBRARY_PATH", "KICAD_RUN_FROM_BUILD_DIR", "APPDIR",
                             "APPIMAGE", "KICAD_KIFACE_HOME"):
                stock_env.pop(variable, None)
            result = subprocess.run(
                [str(stock), "pcb", "export", "gerbers", "--output", str(stock_output),
                 "--layers", "F.Cu,B.Cu,Edge.Cuts", str(session.stock_v10_check(stock, board_path))], env=stock_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, timeout=90)
            assert result.returncode == 0, result.stdout
            assert len([path for path in stock_output.iterdir() if path.is_file()]) >= 3

        bp.close(board)

        # Exercise the command-line Gerber path against the derived fixture,
        # after IPC has persisted the companion/native pair.
        gerbers = artifacts / "fork-gerbers"
        result = subprocess.run(
            [str(fork), "pcb", "export", "gerbers", "--output", str(gerbers),
             "--layers", "F.Cu,B.Cu,Edge.Cuts", str(board_path)],
            env=session.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=90)
        assert result.returncode == 0, result.stdout
        generated = [path for path in gerbers.iterdir() if path.is_file()]
        assert len(generated) >= 3, generated
        print("PASS derived boundary, validate-only and invalid atomicity, save/reopen,"
              + (" stock reopen," if stock else "") + " and CLI Gerbers", flush=True)


if __name__ == "__main__":
    main()
