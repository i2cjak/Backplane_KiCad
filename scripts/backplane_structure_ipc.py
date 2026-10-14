#!/usr/bin/env python3
"""Small client helpers for the board construction IPC surface.

This module deliberately contains no KiCad implementation knowledge.  It wraps
the generated protobuf module and the isolated :class:`IpcSession`, while
keeping JSON payloads and expected error handling consistent between scripts.
"""

from google.protobuf.empty_pb2 import Empty


class BackplaneIpc:
    """Convenience facade over ``IpcSession`` for backplane commands."""

    def __init__(self, session):
        self.session = session
        self.commands = session.module("backplane.structure")
        self.types = session.module("common.types.base_types")
        self.project = session.module("common.commands.project_commands")

    def command(self, name):
        """Return a generated request class, with a useful protocol error."""
        try:
            return getattr(self.commands, name)
        except AttributeError as error:
            available = sorted(name for name in dir(self.commands)
                               if not name.startswith("_") and name[0:1].isupper())
            raise RuntimeError(f"backplane IPC command {name!r} is missing; "
                               f"available messages: {', '.join(available)}") from error

    def response(self, name):
        """Return a generated response class by name."""
        return getattr(self.commands, name)

    def call(self, request_name, response_name=None, **fields):
        request = self.command(request_name)(**fields)
        response_type = Empty if response_name is None else self.response(response_name)
        return self.session.request(request, response_type)

    def call_raw(self, request_name, **fields):
        return self.session.request_raw(self.command(request_name)(**fields))

    def json_get(self, request_name, response_name, board):
        result = self.call(request_name, response_name, board=board)
        if not hasattr(result, "json"):
            raise AssertionError(f"{response_name} has no json field")
        return result

    def json_set(self, request_name, response_name, board, value,
                 validate_only=False):
        return self.call(request_name, response_name, board=board,
                         json=value, validate_only=validate_only)

    def expect_bad_request(self, request_name, **fields):
        response = self.call_raw(request_name, **fields)
        envelope = self.session.envelope
        if response.status.status != envelope.AS_BAD_REQUEST:
            raise AssertionError(f"{request_name} unexpectedly succeeded: {response.status}")
        return response

    def close(self, document):
        return self.session.request(self.project.CloseDocument(document=document), Empty)
