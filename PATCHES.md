# Backplane_KiCad patch triage: backplane/10.0.6 → upstream master

Base for the new branch: upstream master `fe761fb867` (2026-10-14, KiCad 10.99.0).
Old base: tag 10.0.6 `caf7377e9c`. 18 commits on backplane/10.0.6 plus 4 on origin/main.

Classes: **DROP-UPSTREAM** = master has it; **DROP-OBSOLETE** = only meaningful on 10.0.6;
**PORT** = still needed; **PKG** = release packaging, port with changes.

| # | Commit | Subject | Class | Evidence / notes |
|---|---|---|---|---|
| 1 | e424a264fa | backport headless PCB/SCH IPC | DROP-UPSTREAM | `kicad-cli api-server` is upstream (caf1bcc455); master has `headless_sch_context`, `headless_pcb_context` (replaces our `headless_board_context`). |
| 2 | 25348df972 | build deps, provenance | DROP-OBSOLETE | Workflow text only; apt list folded into new CI. |
| 3 | c303bad718 | build prerequisites | DROP-OBSOLETE | Same. |
| 4 | 313b85c6e3 | schematic IPC items + save | DROP-UPSTREAM | All proto fields match master; `sch_api_save`, `schematic_jobs.proto` upstream. Smoke script → #T. |
| 5 | a9ab802ba9 | protocol backports, relocatable runtime | DROP-UPSTREAM + **PORT** | Protos upstream. **Port** `KICAD_KIFACE_HOME` / `KICAD_STOCK_PLUGIN_HOME` hooks (common/paths.cpp, kiway.cpp): needed for the relocatable archive. |
| 6 | b8690cd037 | CI lexer order | DROP-OBSOLETE | 10.0.6 build fix. |
| 7 | 926b6776e9 | stable PCB loader | DROP-OBSOLETE | 10.0.6 loader fix. |
| 8 | 29c379cb6d | PCB job + metadata | DROP-UPSTREAM | upstream 46da153141. |
| 9 | 727005a6f7 | bundle libraries + runtimes | PKG | Repin libraries/template for 10.99 (master libs), drop python3.12 hardcode. |
| 10 | ba40a26602 | template pin | DROP-OBSOLETE | Stable-tag pin; redo for master. |
| 11 | b18ad732b1 | template install layout | PKG | Ports as-is. |
| 12 | 233601cd13 | notice collection | PKG | Ports as-is. |
| 13 | 7c4ecf24c5 | smoke: legacy geometry | PKG | Ports as-is. |
| 14 | c4ca95e57a | smoke: model mesh | PKG | Ports as-is. |
| 15 | 77d957b7f9 | bundle module deps | PKG | Ports; re-validate ELF closure. |
| 16 | d0727c2036 | materialize Python links | PKG | Ports; Python version follows CI image. |
| 17 | 75e38b910c | reject broken links | PKG | Ports as-is. |
| 18 | 1c193606ed | expand editor APIs + 10.0.6 file compat | mostly DROP-UPSTREAM | 1089/1116 proto additions identical on master (adapted from master 2f18d95be7). `FocusOnItem` → upstream `FocusOnItems`; `RuleCheckerMarker` → upstream `DrcMarker`/`ErcMarker`. Line endings + custom properties are **native tokens on master** → the `.backplane.json` sidecar for them is DROP-OBSOLETE. Keep a **reduced** sidecar for fork-only data (board_structure, assembly). Regression scripts → #T. |
| — | e1309bcd6f | QA build fix | DROP-OBSOLETE | Test replaced by upstream `test_headless_pcb_context`. |
| — | 688a0c8229 | sketch router | **PORT** | Fork-only. 4 standalone router files + test; UI hooks (router_tool, pcb_actions, edit_tool, menubar) need hand-merge (heavy upstream churn). |
| — | d5b38b21fa | SketchRoute IPC | **PORT** | Register in master's `api_handler_board.cpp`. Proto in a separate fork file to avoid upstream field collisions. |
| — | 6c8feb7b9f | AGENTS.md | rewrite | New AGENTS.md for master base (2026-10-14 decision). |
| — | (uncommitted) | rigid-flex, structure canvas/validation, assembly IPC | **PORT (phase 2)** | Was never committed; snapshotted to local branch `backplane/wip-structure-2026-09-12` (139 files, +17.8k). Data lives in `.kicad_pcb.backplane.json` sidecars and `.backplane-assembly.json`, no native tokens. Touches 3D viewer, router, STEP exporter, `api_handler_pcb.cpp` (split upstream): hard conflicts expected. |

## New fork patches (not in the old branch)
| ID | Patch | Why |
|---|---|---|
| N1 | `kicad-cli version --format json` | Cheap version + capability handshake without starting a server. |
| N2 | `kiapi.backplane.GetCapabilities` IPC command (own proto package) | Lists fork version, upstream commit, registered API commands, file-format versions. |
| N3 | `api-server --parent-pid PID` | Server exits when the Backplane hub dies; no orphan servers. Windows checks the native PID with wxProcess::Exists; the readiness line is flushed for named-pipe clients. |
| N4 | downgrade carries `.backplane.json` companions | `project downgrade --target 10.0` copies fork sidecars beside the output and reports them as preserved-but-ignored by stock KiCad. |
| N5 | partial PCB footprint updates preserve omitted data | `UpdateItems` requests without `FootprintInstance.definition` retain the existing footprint body and other presence-tracked fields; the PCB IPC regression covers memory and save/reopen. |
| W1 | sketch-router Windows SDK compatibility | On pinned upstream master `fe761fb867` plus fork router port `688a0c8229`, rename local `far` vectors to avoid the Windows SDK legacy `far` macro; the radius calculation is unchanged. |

## Patch set on backplane/master (base fe761fb867; see `git log $(cat BACKPLANE_UPSTREAM)..backplane/master`)
| Commit | Patch | Source |
|---|---|---|
| docs | AGENTS.md (master base, 2026-10-14), PATCHES.md | new |
| ci(release) | Linux runtime workflow for master; removes upstream lockdown.yml | 727005a6f7 … 75e38b910c |
| ci | weekly upstream merge on a sync branch | new |
| test(ipc) | regressions ported to master protos; `--stock-cli` checks `downgrade --target 10.0` copies | scripts from 6c8feb7b9f |
| feat(runtime) | `KICAD_KIFACE_HOME`, `KICAD_STOCK_PLUGIN_HOME` | a9ab802ba9 |
| feat(build) | fork identity + file-format versions in `backplane_build_version.h` | N1/N2 |
| feat(cli) | `api-server --describe` (manifest v2) | N1 |
| feat(cli) | `api-server --parent-pid` | N3 |
| feat(ipc) | `kiapi.backplane.GetCapabilities` | N2 |
| feat(cli) | project downgrade carries `*.backplane.json` / `*.backplane-assembly.json` | N4 |
| docs | BACKPLANE_RUNTIME.md | new |
| feat(pcbnew) | sketch router core | 688a0c8229 |
| feat(pcbnew) | Sketch Route / Autoroute Selected in the router tool | 688a0c8229 |
| feat(ipc) | `kiapi.backplane.SketchRoute` | d5b38b21fa |
| test(ipc) | runtime handshake regression | new |
| fix(pcbnew) | copy teardrop settings in `BOARD_DESIGN_SETTINGS::initFromOther` (SetBoardDesignRules dropped them) | from 1c193606ed; upstream candidate |
| fix(api) | reply `AS_INTERNAL_ERROR` when a handler throws (clients hung forever) | new; upstream candidate |
| fix(eeschema) | parse `custom_property` on schematic graphic polylines (saved files failed to load) | new; upstream candidate |
| fix(eeschema) | resolve headless symbol library IDs synchronously for placement and library-only `CreateItems` | this branch; CI regression below |
| docs | BACKPLANE_IPC.md agent guide for master | new |

## Upstream gaps found by the IPC regressions (fork-patch candidates)
Priority (user decision, 2026-10-15): almost all editing goes through IPC, so IPC-side correctness (create,
update, delete, rollback, save/reopen over IPC) is the goal. **Interactive GUI undo is a known non-goal**: fork
patches are verified over IPC only, and GUI undo behaviour of patched paths (e.g. user-field undo level, PR #7) is
not checked.
Tests print `KNOWN GAP ...` for these instead of failing; each needs a focused patch (and an upstream report).
| Gap | Impact for Backplane | Status |
|---|---|---|
| `CloseAllDocuments(force=False)` ignores `force` and discards unsaved changes | data loss risk for agents | **fixed** in the fork (honours `force`; also guards closing the project document); upstream candidate |
| Footprint/pad children created in a footprint are not found after commit | footprint editing over IPC | **fixed** in the fork (PR #7: 307c41268d, f14a2d270f, f7898cdaf3, 68f5db0933); upstream candidate (GUI undo not checked: non-goal) |
| Partial `FootprintInstance` update clears the footprint body | pads, graphics, zones, groups, constraints, models, and points are lost on update/save | **fixed** in `impl/footprint-partial-update`; upstream candidate |
| Sheet symbol created over IPC (reusing a sheet file) makes the next save fail | schematic becomes unsaveable | candidate patch |
| Placed-symbol update detaches its library link (`ecc83-pp:R` → `:R_1`) | silent library-link loss | candidate patch |
| Headless footprint modified state reflects the board context | wrong dirty flag | candidate patch |
| Footprint `SaveCopyOfDocument` writes no file | export of footprints | candidate patch |
| Footprints open by library ID only (10.0.6 fork also opened `.kicad_mod` by path) | fork feature not carried | decide |
| PCB table cell direct update not applied; standalone cell create/delete not rejected | table editing | candidate patch |
| Schematic line ending set over IPC lost on save | fidelity | candidate patch |
| Schematic graphic shapes do not write custom properties | fidelity (writer change would add tokens upstream can't read: needs upstream) | report upstream |
| Bus-to-bus entry conversion on save gets a new UUID | ID stability | report upstream |
| BOM "Grouped By Value" field preset throws `std::bad_alloc` (kicad-cli too) | BOM preset unusable | report upstream |

Rigid-flex/board structure is source-ported on `port/structure` from snapshot
`1747f64599`, excluding the assembly implementation. The reduced metadata layer
reads v1 item data into native master fields and writes v2 fork extensions only.
Fork IPC lives in `api/proto/backplane/structure.proto`; `board-structure` is a
shared capability. No C++ build or native runtime validation was run on the
workstation. The lead build host must run the migration gate and existing geometry,
DRC/router, GUI/viewer and export regressions. Assembly remains a separate port.
