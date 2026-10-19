# Windows runtime

The `Backplane KiCad Windows` GitHub Actions workflow builds the fork with
native MSVC on Windows 2022. Its `backplane-kicad-windows-x86_64` artifact
contains a zip, the matching source archive and SHA256SUMS. This is a CI build;
it does not create a release or change the managed Linux runtime pin.

Extract the zip as a whole and use `bin/kicad-cli.exe`. The manifest reports
`windows-x86_64` and the same API/capabilities contract as the source. The
package includes release dependency DLLs, their notices and version metadata.
It needs no WSL or separately installed KiCad. Libraries are installed separately.

The workflow uses upstream's pinned vcpkg tool commit, this checkout's pinned
registries, and KiCad's public prebuilt package feed. The recipe is
`scripts/backplane-windows.ps1`; run it in an x64 MSVC developer shell with
CMake, Ninja and Python on PATH, and the VCPKG variables from the workflow.
It uses a dedicated `C:\bp` build prefix on a disposable builder.
The pinned dependency graph supplies protoc 33.4; the Windows test client uses
Python protobuf 6.33.4. The recipe generates and imports its bindings before
compiling C++, and checks that CMake retained the exact release suffix from
`BACKPLANE_VERSION`. Linux and Windows use that same version file; a release
tag must match it. Each release is rebuilt from its own source commit.

The packaged runtime must pass native identity/descriptor checks and the
existing real PCB/schematic IPC smoke: create, rollback, commit, save/reopen,
plus BOM, SVG and GLB exports. Its live GetVersion must match the manifest's
release suffix, as Backplane's hub requires. Windows NNG IPC uses named pipes, so the client
verifies a real request instead of waiting for a Unix socket file to appear.
No Windows acceptance is inferred from a Linux run. PR #11 passed native Windows
CI (39241183795), including parent supervision, and Linux CI (39241183794).
The packaged Windows candidate also passed the Backplane helper handshake,
PCB partial-update/save/reopen preservation, schematic edit/save/reopen,
visible refresh, repeat-open rendering and hub restart/readback checks on the
Windows VM. Those results apply to the tested candidate; new release packages
must pass the native identity, IPC and export gates again.
