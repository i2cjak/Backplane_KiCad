# Native macOS runtime (development build)

`scripts/backplane-macos.sh` builds this pinned master-based fork on Apple
silicon, installs a private KiCad bundle, retains its CLI and both kifaces,
relocates Homebrew and build-tree dylibs into the bundle, ad-hoc signs the modified Mach-O
objects and packages the launcher, descriptor, manifest, corresponding source
and license records. It never installs into `/Applications` or a user's
managed runtime. No signing identity, notarization account or release tag is
required for this development artifact.

The `Backplane KiCad macOS` workflow uses a standard public `macos-15` runner.
It checks the relocated package with isolated PCB/schematic IPC smoke and
save/reopen regressions. Diagnostic output is uploaded even on failure.
An explicit compiler-cache directory is shared by ccache and Actions. If a
later packaging gate fails, the compiled bundle is also retained for inspection.
The native relocation regression accepts an already-rewritten, signed copy of
the same library, rejects a different library with the same basename, and runs
a relocated executable after its original build libraries have been removed.
Successful archives include `SOURCE_COMMIT`, `UPSTREAM_COMMIT`, a runtime
manifest and SHA256SUMS. A workflow running or an archive existing is not a
claim that actual Backplane Mac viewer acceptance passed: that requires
opening copied projects through the Mac Backplane package and inspecting the
Board, Schematic and 3D views in the shipped web interface, saved updates and
native helper shutdown.

Protobuf 29.6 and Python protobuf 5.29.6 are paired. Other native dependencies
are recorded from Homebrew in the artifact's license/package inventory.
The source branch starts at bp.4 source 5dbc22d292. This is a platform
build of that source, not a new published bp release.
