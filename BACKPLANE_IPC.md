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

The Backplane additions are limited to the headless IPC plumbing and its PCB
and schematic document contexts. They do not claim compatibility with KiCad
development or nightly builds.

## Linux release build

The GitHub Actions workflow `backplane-linux-release.yml` builds `kicad-cli`
with IPC enabled, installs it into a staging prefix, checks both `--version`
and `api-server --help`, and archives the installed runtime with the KiCad
source and license files. It builds the selected fork commit, or the fork tag
that triggered the release.

On Ubuntu 24.04, the local equivalent starts with the following dependency
installation (the workflow is the reproducible path):

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  build-essential cmake ninja-build pkg-config ccache \
  libboost-all-dev libbz2-dev libcairo2-dev libcurl4-openssl-dev \
  libeigen3-dev libfontconfig1-dev libfreetype6-dev libgl-dev libglew-dev \
  libglib2.0-dev libgtk-3-dev libharfbuzz-dev \
  libngspice-dev libnng-dev libocct-data-exchange-dev libocct-foundation-dev \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev libpixman-1-dev \
  libpng-dev libprotobuf-dev libsecret-1-dev libtool libwxgtk3.2-dev \
  libwxgtk-webview3.2-dev libx11-dev libx11-xcb-dev libxkbcommon-x11-dev \
  libyaml-cpp-dev libzstd-dev mesa-common-dev pax-utils protobuf-compiler \
  rapidjson-dev swig zlib1g-dev
```
