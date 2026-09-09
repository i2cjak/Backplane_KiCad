# Backplane IPC build contract

This fork exposes KiCad's protobuf IPC API through `kicad-cli api-server` for
Backplane and compatible clients. It keeps the public KiCad IPC protocol and adds the
headless server entry point to the stable 10.0.6 source base.

## Runtime

Start a server with:

```sh
kicad-cli api-server [PROJECT_OR_FILE] [--socket SOCKET_PATH]
```

The optional path may be a `.kicad_pro`, `.kicad_pcb`, or `.kicad_sch` file.
Use `OpenDocument` with `DOCTYPE_FOOTPRINT` to open a library ID or native
`.kicad_mod` file.
The server listens on the default KiCad IPC socket unless `--socket` is
provided. The `OpenDocument`, `CloseDocument`, and `SaveDocument` commands are
available to the headless server; PCB and schematic handlers use the same
protobuf API as the desktop editors.

A server can keep a project's schematic and PCB open together. `CloseAllDocuments`
closes the project documents; dirty documents require saving first or an explicit
`force` request. As in upstream KiCad, each server owns one project and one document
of each editor type. Headless PCB and footprint documents share one editor context.
Use separate server processes and socket paths for separate projects.

Headless selection and active/visible PCB layers retain editor state without
marking the design modified. Cross-probe selection targets the receiving editor.
Commands that require an interactive canvas, including movement tools and net
highlighting, return `AS_UNIMPLEMENTED` in headless mode. Use `kicad-cli sch erc`
and `kicad-cli pcb drc` for rule checking; ERC/DRC execution is not an IPC command.
Schematic ERC markers are read-only diagnostics. PCB table cells support content
updates; change a table's structure by updating its parent table rather than
creating or deleting individual cells.

The board proto retains `UpdateBoardStackup` for protocol compatibility, but
the stable backport currently registers only `GetBoardStackup`; clients must
treat stackup updates as unavailable until a handler is added. The headless
board APIs expose enabled-layer, origin, plot-setting, design-rule, custom-rule,
netlist, connectivity, and embedded-file updates separately.

## File compatibility

Native `.kicad_pcb`, `.kicad_sch`, `.kicad_mod`, and `.kicad_sym` files target
unmodified KiCad 10.0.6. IPC backports must not increase the native format version
or write tokens that the stable parser cannot read.

Fork-only custom properties and line endings use an adjacent companion named
`<native-filename>.backplane.json`. Keep it beside the design when copying or
sharing files. Stock KiCad reads and edits the native design; the fork restores
the additional metadata when the companion is present. Native geometry remains
authoritative. Stock KiCad does not display these added line endings or edit
the companion metadata.

Release verification includes native save/reopen tests. The mutation, variant,
and schematic fidelity regressions accept `--stock-cli /path/to/kicad-cli` to
resave disposable fixtures through an unmodified 10.0.6 installation before
reopening them in the fork.

## Source and licensing

This repository is based on the official KiCad 10.0.6 release (`caf7377e9c`,
tag `10.0.6`). The focused IPC backport follows upstream commits
`caf1bcc455` (headless `kicad-cli api-server`) and `9fe748f39a`
(headless schematic opening), with the required schematic context/commit
follow-ups from `3dca4e0e38`, `fee17a9580`, and `5c1c5a0c03`. KiCad's original
license and third-party notices remain in the
repository root. Any binary distribution must ship those files alongside the
KiCad runtime and preserve the corresponding third-party notices.

The schematic item and hierarchy backports are based on the following public
upstream revisions, applied without replacing the 10.0.6 base: `4ccede3a24`
(graphic types and serializers), `7526d2901e` (groups and sheet symbols),
`edc6236b07` (text and labels), `9533fdac22` (symbols and pins),
`76ac9e07a6` (symbol/sheet instances), `6cca4c734f` (naming follow-up),
`48f4d67cfe` (hierarchy query), `df8f79916a` (subsheet handling), and
`b90628aa0d` (schematic netlist). Schematic `GetItems`, create/update/delete,
commit rollback, save, hierarchy, and netlist operations are the supported
IPC surface; clients should use the generated stable proto names such as
`SchematicText` and `SchematicSymbolInstance`.

The fork retains the stable KiCad version number. Clients should generate
bindings from this repository's `api/proto` definitions; nightly client version
checks are not a substitute for checking supported commands.

The board-job enum conversion specializations and their round-trip QA coverage
are restored from upstream KiCad commit `46da153141` (`ADDED: IPC API support
for jobs`) for the stable 10.0.6 IPC backport.

The expanded IPC backports are adapted from upstream revision
`2f18d95be746a6504d36b0bc8320f66b5ebd8524`, including `5051a0398c`/`da800cc8c2`
(connectivity), `6fd26c3126`/`10f8e99502` (netlist import), `8fef07daf4`/`c4b5505e0b`
(design rules), `c3570f4925` (plot settings), `5d319ad408` (footprint documents),
`796ae13ba9` (embedded files), `82729df8a4` (cross-probe), `be90a7e200` (modified
state), `144ca549c2` (schematic command names), `4bc59513c3`/`6a91fed912`
(symbol fidelity), `59e7182ce2` (schematic tables), and `a3aafd8499` (rule areas).
Newer native metadata is adapted to the companion format described above.

## Linux release build

The GitHub Actions workflow `backplane-linux-release.yml` builds `kicad-cli`
with IPC enabled and installs it into a staging prefix. Before archiving the
relocated runtime, `scripts/backplane-ipc-smoke.py` opens disposable PCB and
schematic fixtures, checks item creation and editing, rolls back transactions,
and verifies that saved changes survive reopening. It also queries schematic
symbols, hierarchy, and nets, and exports a BOM, schematic SVG, and board GLB.
The release includes the matching source archive
and license files. It builds the selected fork commit, or the fork tag that
triggered the release.

The focused rule-check regression copies `demos/ecc83` into a temporary
workspace, performs a schematic value update and a rolled-back PCB edit through
IPC, then compares ERC/DRC violation type, severity, and item identity before
and after save/reopen. Run it with the built CLI; pass `--stock-cli` with an
unmodified 10.0.6 CLI to repeat the comparison independently. A developer CLI
that omits GUI KIFACEs can use `--rulecheck-cli` for a complete sibling CLI to
produce the ERC/DRC reports.

On Ubuntu 24.04, the local equivalent starts with the following dependency
installation (the workflow is the reproducible path):

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  build-essential cmake ninja-build pkg-config ccache \
  libboost-all-dev libbz2-dev libcairo2-dev libcurl4-openssl-dev \
  libeigen3-dev libfontconfig1-dev libfreetype6-dev libgl-dev libglm-dev \
  libgl1-mesa-dev libglew-dev libglu1-mesa-dev libglib2.0-dev libgtk-3-dev \
  libharfbuzz-dev libgit2-dev libssl-dev \
  libngspice0-dev ngspice libnng-dev libocct-data-exchange-dev libocct-foundation-dev \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev libocct-ocaf-dev \
  libocct-visualization-dev libpixman-1-dev libpng-dev libpoppler-dev \
  libpoppler-glib-dev libprotobuf-dev libsecret-1-dev libspnav-dev libtool \
  libwxgtk3.2-dev \
  libwxgtk-webview3.2-dev libx11-dev libx11-xcb-dev libxkbcommon-x11-dev \
  libyaml-cpp-dev libzstd-dev mesa-common-dev pax-utils protobuf-compiler \
  python3-dev python3-venv rapidjson-dev shared-mime-info swig unixodbc-dev zlib1g-dev \
  libzint-dev
```
