# Backplane assembly IPC

The assembly API treats a multi-board assembly as a JSON manifest beside the
native PCB files.  The manifest is a host-local file: `path`, `output_path`, and
each instance `source` are resolved by the KiCad process.  Native `.kicad_pcb`
and `.kicad_sch` files remain in upstream master format; assembly metadata is
kept in `.backplane-assembly.json` files.

## File and headless commands

| Command | Purpose | Result |
| --- | --- | --- |
| `GetAssembly` | Load and validate JSON from `path`, or normalize `manifest_json` | `manifest_json` |
| `SaveAssembly` | Validate and atomically save a manifest | `path`, normalized JSON |
| `ResolveAssemblyMates` | Apply point and axis mates | resolved `manifest_json` |
| `CheckAssemblyCollisions` | Load source geometry and check transformed solids | `collisions` |
| `ExportAssemblyStep` | Export the combined transformed assembly | output `path` |
| `ListAssemblyComponents` | Inspect components on one native board instance | `result_json` |
| `MoveAssemblyComponent` | Move a footprint in its source PCB | status and `path` |
| `SyncAssemblyElectrical` | Refresh logical net names from native boards | `result_json` |

`ListAssemblyComponents`, `MoveAssemblyComponent`, and
`SyncAssemblyElectrical` include a `DocumentSpecifier document` identifying the
open source PCB.  It is required for component movement so the edit is applied
to the intended open board.  `MoveAssemblyComponent` coordinates are optional
at the protobuf level, but the handler requires finite X, Y, and rotation values.

Assembly handlers return an `AssemblyResponse`: `status` is a
`CommandStatus` (`CS_OK` or `CS_FAILED`).  Transport or handler validation
failures use the envelope `ApiStatusCode` (`AS_BAD_REQUEST`, for example).

## Python example

Run with `PYTHONPATH=scripts`. The reusable harness starts a local, isolated
headless server; pass `api_socket` to attach to an existing GUI instead.

```python
import json
from backplane_ipc_test_support import IpcSession

with IpcSession("/path/to/kicad-cli") as session:
    assembly = session.module("backplane.assembly")
    types = session.module("common.types.base_types")
    board = session.request(
        session.module("common.commands.project_commands").OpenDocument(
            type=types.DOCTYPE_PCB, path="/work/main.kicad_pcb"),
        session.module("common.commands.project_commands").OpenDocumentResponse).document
    manifest = "/work/assembly.backplane-assembly.json"
    loaded = session.request(assembly.GetAssembly(path=manifest), assembly.AssemblyResponse)
    print(json.loads(loaded.manifest_json)["instances"])

    resolved = session.request(assembly.ResolveAssemblyMates(path=manifest),
                               assembly.AssemblyResponse)
    collisions = session.request(assembly.CheckAssemblyCollisions(path=manifest),
                                 assembly.AssemblyResponse)
    session.request(assembly.ExportAssemblyStep(path=manifest, output_path="/work/assembly.step"),
                    assembly.AssemblyResponse)
```

## GUI workspace and 3D viewing

`OpenAssemblyTab`, `ListAssemblyTabs`, and `ShowAssembly3D` are GUI workspace
commands.  Their `document` selects a PCB editor and `tab_index` selects an
assembly tab within that editor. In a headless handler these commands return
`AS_UNIMPLEMENTED`. The current GUI path validates and prepares an unfolded
assembly STEP for the separate assembly-capable 3D viewer implementation;
folded rigid-flex presentation remains a TODO.

The 3D export job and combined assembly STEP export currently use the unfolded
native-board path. Folded rigid-flex presentation is a clearly bounded TODO for
the separate structure/3D-viewer port. Gerbers describe individual board
fabrication layers and therefore must be generated per source PCB, rather than
from a combined assembly.

## Electrical synchronization

`electrical.links` contains logical endpoint records such as instance, reference,
and pad. `SyncAssemblyElectrical` reads the native board nets and returns a
fresh logical snapshot in `result_json`. Setting `save_snapshot = true` writes
that snapshot into the manifest only after synchronization succeeds. It does
not rewire native board nets or modify `.kicad_sch` files. Endpoints sharing an
instance/native net are merged into logical system nets with stable identifiers.
Conflicting explicit system names are reported; different native names, such as
TX on one board and RX on another, may connect. This is logical interconnect
authoring and synchronization, not a native KiCad system schematic editor.

## Manifests and parallel inspection

Each manifest has its own instance namespace, source base directory, mates, and
electrical section. Nested manifests resolve their sources relative to their own
file and flatten to qualified IDs such as `module/leaf`. Separate manifests can
be opened in separate GUI tabs; the current GUI path prepares an unfolded STEP
artifact for each tab.

Current constraints include one directed parent per mate dependency and the
supported point/axis mate forms. Axis constraints reject zero-length axes and
cycles. Folded state is implemented in staged STEP export and is readable by
both native 3D rendering paths; full folded ray-traced presentation remains a
separate viewer concern.

The assembly port is based on upstream master `fe761fb867`; its headless
regression is `scripts/backplane-assembly-regression.py`. Folded rigid-flex
presentation is intentionally left to the separate structure/3D-viewer port.
