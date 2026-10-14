#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the Backplane runtime handshake against an isolated KiCad CLI.

Covers the fork patches agreed in bp-coord/kicad-fork/CONTRACT.md:
`api-server --describe`, `kiapi.backplane.GetCapabilities`, `api-server --parent-pid`,
and Backplane companion files carried through `project downgrade --target 10.0`.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time

from backplane_ipc_test_support import IpcSession

CAPABILITIES = {"api-server", "headless-pcb", "headless-sch", "export-v10", "parent-pid",
                "capabilities", "sketch-route", "assembly"}


def check_describe(cli, env):
    result = subprocess.run([str(cli), "api-server", "--describe"], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    assert result.returncode == 0, result.stderr
    manifest = json.loads(result.stdout)
    assert manifest["manifestVersion"] == 2, manifest
    assert manifest["apiVersion"] >= 1, manifest
    assert CAPABILITIES <= set(manifest["capabilities"]), manifest["capabilities"]
    assert "10.0" in manifest["downgradeTargets"], manifest
    assert manifest["fileFormats"]["board"] > 20260206, manifest
    assert manifest["fileFormats"]["schematic"] > 20260306, manifest
    assert manifest["kicadVersion"].startswith("10.99"), manifest
    print(f"PASS --describe: {manifest['version']} (upstream {manifest['upstreamCommit']})", flush=True)
    return manifest


def check_capabilities(api, manifest):
    backplane = api.module("backplane.capabilities")
    reply = api.request(backplane.GetCapabilities(), backplane.GetCapabilitiesResponse)
    assert reply.fork_version == manifest["version"], reply
    assert reply.upstream_commit == manifest["upstreamCommit"], reply
    assert reply.api_version == manifest["apiVersion"], reply
    assert set(reply.capabilities) == set(manifest["capabilities"]), reply
    assert reply.board_file_version == manifest["fileFormats"]["board"], reply
    assert reply.schematic_file_version == manifest["fileFormats"]["schematic"], reply
    commands = set(reply.commands)
    for name in ("kiapi.backplane.GetCapabilities", "kiapi.common.commands.GetVersion"):
        assert name in commands, sorted(commands)
    print(f"PASS GetCapabilities: {len(commands)} registered commands", flush=True)


def check_parent_pid(cli, root, env):
    parent = subprocess.Popen(["sleep", "600"])
    socket = root / "parent-pid" / "api.sock"
    server = subprocess.Popen([str(cli), "api-server", "--socket", str(socket),
                               "--parent-pid", str(parent.pid)],
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 30
        while not socket.is_socket():
            assert server.poll() is None, f"server exited early: {server.stdout.read()}"
            assert time.monotonic() < deadline, "server did not start"
            time.sleep(0.05)
        time.sleep(1.5)
        assert server.poll() is None, "server exited while its parent was alive"
        parent.kill()
        parent.wait()  # Reap it: a zombie still answers kill(pid, 0).
        server.wait(timeout=10)
        print(f"PASS --parent-pid: server exited ({server.returncode}) after its parent died",
              flush=True)
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait()
        if server.poll() is None:
            server.kill()
            server.wait()


def check_downgrade_companions(cli, source, root, env):
    project = root / "companions" / "test_project"
    shutil.copytree(source, project)
    board_companion = project / "test_project.kicad_pcb.backplane.json"
    board_companion.write_text('{"format":"backplane-kicad-metadata","version":1,"items":{}}\n',
                               encoding="utf-8")
    assembly = project / "test_project.backplane-assembly.json"
    assembly.write_text('{"format":"backplane-assembly","version":1}\n', encoding="utf-8")
    out = root / "companions" / "test_project-kicad10"
    report = root / "companions" / "report.json"
    result = subprocess.run([str(cli), "project", "downgrade", "--target", "10.0", "--force",
                             "-o", str(out), "--report-json", str(report),
                             str(project / "test_project.kicad_pro")],
                            env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=300)
    assert result.returncode == 0, result.stdout
    for companion in (board_companion, assembly):
        copied = out / companion.name
        assert copied.read_bytes() == companion.read_bytes(), f"{copied} missing or changed"
    text = report.read_text(encoding="utf-8")
    assert board_companion.name in text and assembly.name in text, text
    print("PASS project downgrade: Backplane companions copied and reported", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kicad_cli", type=Path)
    args = parser.parse_args()
    with IpcSession(args.kicad_cli) as api:
        manifest = check_describe(api.cli, api.env)
        check_capabilities(api, manifest)
        check_parent_pid(api.cli, api.root, api.env)
        check_downgrade_companions(api.cli, api.source / "qa/data/libraries/test_project",
                                   api.root, api.env)


if __name__ == "__main__":
    main()
