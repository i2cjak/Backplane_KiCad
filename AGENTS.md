# Backplane KiCad

- New commits must use author and committer `i2cjak <build@amemb.com>`. Verify `git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT` before committing.
- Before every push, verify the authenticated account is `i2cjak`, the destination is the public `i2cjak/Backplane_KiCad` repository, and review the outgoing commits, diff, and relevant checks. Preserve upstream commit identities and history.
- Base the fork on upstream KiCad master (user decision, 2026-10-14), not on stable releases. `backplane/master` is a pinned upstream master commit plus a small patch set listed in `PATCHES.md`. Rebase onto upstream master regularly (weekly target); each release pins a known-good upstream commit and records it in the runtime manifest.
- Keep patches small and focused, and record their upstream context (base commit, and the source commit when porting). Drop a patch once upstream covers it. Verify both schematic and PCB behavior over IPC. Never label an untested patch a working runtime.
- Native files follow upstream master's format. Do not add fork-only tokens to `.kicad_pcb`/`.kicad_sch`; store fork-only data in adjacent `.backplane.json` / `.backplane-assembly.json` companions and preserve them through save, copy, export, and `kicad-cli ... downgrade`. Stock KiCad 10 users get a copy via `kicad-cli project downgrade --target 10.0`.
- Fork-only IPC messages live in proto package `kiapi.backplane`, never in upstream's packages.
- Heavy builds run on `h@backplane` under `systemd-run --user -p MemoryMax=20G`, -j8, ccache. Never run a full KiCad build on the user's desktop.
- Full builds currently run in GitHub Actions (`backplane-linux-release.yml`, ubuntu:26.04 container, ccache); `h@backplane` is for light work until its hardware is checked (2026-10-14).
- Only the fork lead pushes `backplane/master` and `port/*`. Implementers push only to their own `impl/*` branches.
- Outside pull requests on i2cjak repositories are never merged and never closed; leave them open. Do not check out or run their code (user rule, 2026-10-15).
- This repository and corresponding source for distributed binaries must remain public. Preserve upstream licenses, authorship, and third-party notices. Ship licenses and matching source with release binaries.
- Coordinate the runtime archive layout, handshake, and supported platforms with the sibling Backplane app (`bp-coord/kicad-fork/CONTRACT.md`). Validate the packaged runtime on a host without a separate KiCad installation.
- Never add OpenAI copyright or ownership notices.
