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
The server listens on the default KiCad IPC socket unless `--socket` is
provided. The `OpenDocument`, `CloseDocument`, and `SaveDocument` commands are
available to the headless server; PCB and schematic handlers use the same
protobuf API as the desktop editors.

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
