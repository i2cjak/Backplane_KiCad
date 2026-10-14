#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise construction, viewer guards, and folded export IPC commands.

The command names intentionally mirror ``backplane/structure.proto``.
The script runs against a disposable copy of the demo projects and never
modifies the repository fixtures.
"""

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
    parser.add_argument("--stock-cli", type=Path,
                        help="Optional stock KiCad 10.0.x CLI used for downgraded reopen check")
    args = parser.parse_args()
    if args.stock_cli:
        args.stock_cli = args.stock_cli.resolve(strict=True)
        version = subprocess.check_output([str(args.stock_cli), "--version"], text=True).strip()
        assert version.startswith("10.0."), f"expected stock KiCad 10.0.x, got {version}"

    with IpcSession(args.kicad_cli) as session:
        bp = BackplaneIpc(session)
        board_jobs = session.module("board.board_jobs")
        jobs = session.module("common.types.jobs")
        board_types = session.module("board.board_types")
        project = bp.project
        types = bp.types
        source = session.source / "demos/backplane-structure"
        work = session.root / "structure-ipc"
        shutil.copytree(source, work / "backplane-structure")
        board_path = work / "backplane-structure" / "rigid-flex-bridge.kicad_pcb"
        board = session.request(project.OpenDocument(
            type=types.DOCTYPE_PCB, path=str(board_path)), project.OpenDocumentResponse).document

        # Construction is a canonical JSON object in the companion file.  A
        # validate-only set must not change the object or mark the document.
        before = bp.json_get("GetBoardStructure", "BoardStructureResponse", board)
        assert before.valid and json.loads(before.json)["schema_version"] == 1, before
        checked = bp.json_set("SetBoardStructure", "BoardStructureResponse", board,
                              before.json, validate_only=True)
        assert checked.valid and checked.json == before.json, checked
        invalid = json.dumps({"schema": 1, "regions": [{"bad": True}]})
        bp.expect_bad_request("SetBoardStructure", board=board, json=invalid,
                              validate_only=False)
        after_invalid = bp.json_get("GetBoardStructure", "BoardStructureResponse", board)
        assert after_invalid.json == before.json, "invalid construction set was not atomic"

        # A real edit must round-trip through the companion and native save.
        edited = json.loads(before.json)
        edited.setdefault("metadata", {})["ipc_regression"] = "roundtrip"
        edited_json = json.dumps(edited, sort_keys=True, separators=(",", ":"))
        bp.json_set("SetBoardStructure", "BoardStructureResponse", board, edited_json)
        session.request(project.SaveDocument(document=board), Empty)
        session.request(project.CloseDocument(document=board), Empty)
        if args.stock_cli:
            session.stock_v10_check(args.stock_cli, board_path)
        board = session.request(project.OpenDocument(
            type=types.DOCTYPE_PCB, path=str(board_path)), project.OpenDocumentResponse).document
        assert json.loads(bp.json_get("GetBoardStructure", "BoardStructureResponse", board).json) == edited

        viewer = bp.call("GetViewerConstructionState", "ViewerConstructionStateResponse", board=board)
        assert not viewer.viewer_open, "headless IPC unexpectedly reports an open 3D viewer"
        viewer_error = bp.expect_bad_request("SetViewerConstructionState", board=board,
                                             construction_view=True, folded_view=True,
                                             open_if_needed=False)
        assert "viewer" in viewer_error.status.error_message.lower()
        print("PASS construction get/validate/set atomicity, save/reopen, headless viewer guard",
              flush=True)

        # The fork wraps an unchanged upstream 3D job with its folded option.
        step_job = board_jobs.RunBoardJobExport3D(
            job_settings=jobs.RunJobSettings(document=board,
                                                  output_path=str(session.root / "folded.step")),
            format=board_jobs.B3D_STEP, export_components=True,
            export_board_body=True, include_unspecified=True, substitute_models=True,
            board_outlines_chaining_epsilon=0.01, overwrite=True)
        step_result = session.request(bp.commands.RunBoardStructureExport3D(
            job=step_job, folded=True), jobs.RunJobResponse)
        assert step_result.status == jobs.JS_SUCCESS, step_result
        gerber_job = board_jobs.RunBoardJobExportGerbers(
            job_settings=jobs.RunJobSettings(document=board,
                                                  output_path=str(session.root / "gerbers")),
            plot_settings=board_jobs.BoardPlotSettings(
            layers=[board_types.BL_F_Cu, board_types.BL_B_Cu, board_types.BL_Edge_Cuts]))
        gerber_result = session.request(gerber_job, jobs.RunJobResponse)
        assert gerber_result.status == jobs.JS_SUCCESS, gerber_result
        assert (session.root / "folded.step").stat().st_size > 1000
        assert len(list((session.root / "gerbers").rglob("*.gbr"))) >= 3
        print("PASS folded STEP and flat Gerber IPC jobs",
              flush=True)
        bp.close(board)


if __name__ == "__main__":
    main()
