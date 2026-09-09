# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolated IPC test session; never opens or edits the user's projects."""

import importlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import pynng


class IpcSession:
    def __init__(self, cli, timeout=180000):
        self.cli = Path(cli).resolve(strict=True)
        self.source = Path(__file__).resolve().parents[1]
        self.timeout = timeout
        self.token = ""
        self.server = None
        self.connection = None
        self.log = None

    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="backplane-ipc-regression-")
        self.root = Path(self.temporary.name)
        self.bindings = self.root / "bindings"
        self.bindings.mkdir()
        proto = self.source / "api/proto"
        try:
            subprocess.run(["protoc", f"-I{proto}", f"--python_out={self.bindings}",
                            *map(str, sorted(proto.rglob("*.proto")))], check=True)
            sys.path.insert(0, str(self.bindings))
            self.envelope = self.module("common.envelope")
            self.env = dict(os.environ)
            for variable, directory in (
                ("HOME", "home"), ("XDG_DATA_HOME", "data"),
                ("XDG_CACHE_HOME", "cache"), ("XDG_CONFIG_HOME", "config"),
                ("KICAD_CONFIG_HOME", "config/kicad"),
            ):
                path = self.root / directory
                path.mkdir(parents=True, exist_ok=True)
                self.env[variable] = str(path)
            self.env.pop("DISPLAY", None)
            self.env.pop("WAYLAND_DISPLAY", None)
            self.socket = self.root / "api.sock"
            self.log = (self.root / "server.log").open("w+")
            self.server = subprocess.Popen(
                [str(self.cli), "api-server", "--socket", str(self.socket)],
                env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
            )
            deadline = time.monotonic() + 30
            while not self.socket.is_socket():
                if self.server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"IPC server did not start: {self.server.poll()}")
                time.sleep(0.05)
            self.connection = pynng.Req0(
                dial=f"ipc://{self.socket}", block_on_dial=True,
                send_timeout=self.timeout, recv_timeout=self.timeout,
            )
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def module(self, name):
        return importlib.import_module(name + "_pb2")

    def copy_project(self, source, name):
        destination = self.root / name
        shutil.copytree(self.source / source, destination)
        return destination

    def request_raw(self, command, *, type_url=None):
        if os.environ.get("BACKPLANE_IPC_TRACE"):
            print(f"IPC {command.DESCRIPTOR.full_name}", flush=True)
        request = self.envelope.ApiRequest()
        request.header.client_name = "works.backplane.ipc-regression"
        request.header.kicad_token = self.token
        request.message.Pack(command)
        if type_url is not None:
            request.message.type_url = type_url
        try:
            self.connection.send(request.SerializeToString())
            response = self.envelope.ApiResponse.FromString(self.connection.recv())
        except pynng.exceptions.Timeout as error:
            raise TimeoutError(
                f"{command.DESCRIPTOR.full_name} timed out; "
                f"IPC server exit status: {self.server.poll()}"
            ) from error
        self.token = response.header.kicad_token
        return response

    def request(self, command, response_type, *, type_url=None):
        response = self.request_raw(command, type_url=type_url)
        if response.status.status != self.envelope.AS_OK:
            raise AssertionError(f"{command.DESCRIPTOR.full_name}: {response.status}")
        result = response_type()
        if not response.message.Unpack(result):
            raise AssertionError(f"Wrong reply type to {command.DESCRIPTOR.full_name}: "
                                 f"{response.message.type_url}")
        return result

    def __exit__(self, exc_type, exc, traceback):
        if self.connection is not None:
            self.connection.close()
        if self.server is not None and self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=10)
        if self.log is not None:
            if exc is not None:
                self.log.seek(0)
                print(self.log.read()[-24000:], file=sys.stderr)
            self.log.close()
        if str(self.bindings) in sys.path:
            sys.path.remove(str(self.bindings))
        self.temporary.cleanup()
