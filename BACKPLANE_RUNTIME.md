# Backplane headless runtime

This runtime is based on upstream KiCad master `fe761fb867` (KiCad 10.99).
Release builds append `bp.N` through `KICAD_VERSION_EXTRA`; the configured
Backplane identity is reported separately.

## Start the headless server

The hub starts one server per project, passes its own PID, and waits for the
ready line before connecting:

```sh
kicad-cli api-server <project>.kicad_pro \
  --socket "$XDG_RUNTIME_DIR/backplane/kicad/<project-hash>/api.sock" \
  --parent-pid <hub-pid>
```

The hub creates the project directory with mode `0700`. The server prefixes
the filesystem socket with `ipc://` and prints this unchanged readiness line:

```text
KiCad API server listening at <path>
```

On Linux and macOS, `--parent-pid` checks the parent about every 500 ms. If the
PID is gone, the server closes open documents and exits through the normal
shutdown path. Windows currently accepts the option but leaves supervision as
a no-op.

## Static description

```sh
kicad-cli api-server --describe
```

This prints one JSON object and exits without starting an API server. It
contains the manifest version, source repository and commit, pinned upstream
commit, fork and KiCad versions, API version, shared capability names, native
file-format versions, downgrade target IDs, platform when known, and
`GPL-3.0-or-later`.

## Live capabilities

After connecting, call the protobuf command
`kiapi.backplane.GetCapabilities`. Its response repeats the fork identity and
capabilities, gives the board and schematic file-format versions, and lists
every fully-qualified command type registered by every running API handler.

## Open in stock KiCad 10

The manual-work export command is:

```sh
kicad-cli project downgrade --target 10.0 \
  -o <project>/backplane-export-v10 <project>.kicad_pro \
  --report-json <dir>/downgrade-report.json
```

The command exits nonzero without writing an export when the report contains a
blocked or lossy change. The caller can show the report and retry with
`--force`; `--dry-run` produces the report only. Backplane companion files
(`*.backplane.json` and `*.backplane-assembly.json`) are copied beside the
export and reported as kept because stock KiCad ignores them.

## Relocated runtime hooks

Set `KICAD_KIFACE_HOME` to the directory containing the runtime's `.kiface`
plugins. Set `KICAD_STOCK_PLUGIN_HOME` to the runtime's stock plugin directory.
These explicit paths take precedence over the normal executable/AppImage
discovery paths and allow a bundled runtime to be relocated safely.
