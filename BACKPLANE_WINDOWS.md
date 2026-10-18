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

The packaged runtime must pass native identity/descriptor checks and the
existing real PCB/schematic IPC smoke: create, rollback, commit, save/reopen,
plus BOM, SVG and GLB exports. Windows NNG IPC uses named pipes, so the client
verifies a real request instead of waiting for a Unix socket file to appear.
No Windows acceptance is inferred from a Linux run. The VM integration and
parent supervision still need their own Windows verification before the full
Backplane integration is considered complete.
