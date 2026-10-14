# Board construction implementation and local validation

Master-port status (2026-10-14): source port only, based on `fe761fb867`
(KiCad 10.99) and WIP snapshot `1747f64599`. No C++ compilation, native GUI,
DRC, routing, viewer, or STEP runtime checks were performed for this port.
Historical builds below refer to the 10.0.6 fork. The lead must validate on the
separate build host before treating this as a working runtime.

The Layers shortcut and canvas authoring follow-up are documented and tested in
[Rigid-flex canvas workflow](BACKPLANE_STRUCTURE_CANVAS.md).

Implementation work is in progress. This report distinguishes completed local checks
from the broader design goals in [the specification](BACKPLANE_RIGID_FLEX_SPEC.md).
It is not a fabrication qualification or a claim of complete Altium parity.

## Implemented paths

- Current storage uses master native board artwork plus a v2 companion. A KiCad 10
  copy is made through project downgrade. Physical regions, local
  material stacks, stiffeners, adhesive, coverlay, and bends are stored in the
  adjacent construction companion.
- Board Setup includes construction authoring, a regional material layer editor,
  and a flat region/bend preview.
- DRC checks regional coverage, copper layer presence, region overlaps, bend strips,
  and reinforcement placement. Zone filling clips copper to regional domains.
  The interactive router receives temporary construction keepouts.
- Gerber exports include construction material areas, coverlay openings, adhesive
  areas, bend references, and a JSON manufacturing manifest. Plot dialog and CLI
  use the same construction exporter. Fabrication coordinates remain flat.
- STEP exports include physical regional thickness, stiffeners/adhesive, coverlay,
  and drilled holes. Folded export uses curved geometry and rigid component
  transforms. Invalid geometry fails export.

## Completed local milestones

The following results were reported by the 10.0.6 snapshot. They have not
been reproduced on master and do not validate this source port.

| Check | Local result (exports build 13; GUI build 14/19) |
| --- | --- |
| Build PCB editor, CLI, PCB QA targets | Passed |
| Focused Backplane unit tests | 31 passed |
| Native board-item, footprint, group save/load tests | 26 passed |
| Stock 10.0.6 and fork save/reopen | All three exemplars passed |
| Stock and fork DRC | Zero violations/unconnected items on all three exemplars |
| Negative DRC fixtures | Gaps, overlap, absent copper, holes, pads/vias in bends detected |
| Gerber independent parsing | Passed; region coordinates and material polarity checked |
| STEP independent OCP reopening | All exported solids valid |
| Flat STEP physical dimensions | Regional stack thicknesses and drill voids checked at multiple depths |
| Folded STEP | All three exemplars passed; holes retained on moving/fixed regions |
| Folded component orientation | Rotation, anchor position, handedness, and regional seating passed |
| Assembly | Separated assembly clean; intentional overlap detected; missing source rejected |
| Neutral axis / datum | Custom neutral offset and shifted stack datum passed; reinforcement stays aligned |
| Coverlay openings | Clear Gerber polarity and mechanical adhesive/coverlay voids passed |
| Router | Native PNS world test passed: absent regional layer blocks tracks; bend blocks vias |
| Zone fill | F.Cu fill split into rigid islands; no copper across the flex bridge |
| GLB | Valid binary containers and nonempty meshes for all three exemplars |
| GUI | Regional layer and stiffener edits saved/reopened; Cancel retained exact companion bytes |
| Native component models | Stock headers, SOIC-8 and 0805 SMD packages verified seated on fixed and moving islands through a 180° U-fold; local DRC remains clean; populated STEP solids valid |
| Native 3D viewer | Flat raytrace passed; build 19 OpenGL flat/fold toggling and two-bend U-fold visually verified with closed sidewalls and curved surfaces |

The regression driver is [scripts/backplane-structure-regression.py](scripts/backplane-structure-regression.py).
It copies source examples into a separate output directory, preserving the
authoritative example files. It uses stock KiCad, the fork, Gerbonara, and an
independent OCP installation. Negative cases must produce the intended diagnostics.

Example invocation:

```sh
python scripts/backplane-structure-regression.py /path/to/fork/kicad-cli \
  --stock-cli /usr/bin/kicad-cli --output /tmp/structure-regression --folded
```

## Known scope boundaries

Assembly GUI/IPC, manifests, mates and collision checks belong to the separate
assembly port. Historical assembly results in the old table do not validate this
board-structure port. Mechanical optimization is outside this implementation.
Bends use ordered developed half-planes with nonintersecting strips and a
common parent flex thickness and center datum. Crossing hinge strips and
heterogeneous bend-parent stacks are rejected.
OpenGL construction mode currently omits depth-specific counterbore/countersink
enlargements; ordinary through-drill voids are retained. Stiffeners are designer-authored. Bend visualization does not predict stress or
fatigue life. Folded native viewing uses construction view with clipped regional copper,
pads, mask, silk and component models. Models retain their local offsets, rotations, scales, and side while
following their parent island.

Before release, rerun this report against the final source revision, verify packaged
execution without a separate KiCad installation, and validate additional bend
topologies before extending support.

Native viewer screenshots are direct X11 captures. Viewer acceptance caught and
fixed a same-board refresh that reset fold state, missing folded wall normals,
internal triangle walls, constant-Z lighting on curved faces, and folded contour
emission leaking into flat and ordinary electrical views. Flat reinforcement and coverlay seating now use the parent material stack datum,
with separate adhesive solids. Construction solids are opaque to avoid unsorted
transparency artifacts. Construction
meshes use a 0.25 mm developed sampling interval; they are a visual approximation
of the independently checked mechanical export.

## Construction layers and artwork, build 42 (2026-09-12)

See `BACKPLANE_STRUCTURE_CANVAS.md` for the layer-based authoring workflow and
local evidence. Build 42 passes 49 Backplane cases / 198 assertions, the
construction/assembly IPC regression, boundary IPC persistence/Gerber checks,
and partition rebuild checks for all three updated exemplars. Native OpenGL
inspection confirms continuous copper through both bends, SMD pads/models, silk,
and translucent coverlay. Dedicated boundary/bend/F-B stiffener contexts are
available in the Layers panel and toolbar; physical data currently remains in
companions. These are historical 10.0.6 results; stock 10 export on master uses downgrade.

Remaining limits include construction-order-dependent cuts, rejected unsupported
boundary crossings, explicit repartition after native Edge.Cuts edits, no native
whole-feature drag handles, and omitted folded via barrel/depth-machining surfaces.
These checks validate software fixtures locally; they are not fabricated-board or
flex-fatigue qualification.

Build 43 compiles the editor, CLI, and PCB QA targets after a button-sizing-only
polish. Native GUI inspection at 1100 × 1100 confirms all six construction
actions fit the 250-pixel sidebar. Selecting B.Stiffener and invoking Properties
opens the bottom-side material, thickness, and adhesive editor. The model-test
results above are from build 42; they were not repeated for this sizing change.
Four square native screenshots are retained in the local
`/home/h/.cache/backplane-flex-tests/final-validation/` directory:
`01-folded-copper-smd.png`, `02-flat-copper-smd.png`,
`03-construction-layers.png`, and `04-stiffener-properties.png`.

## KiCad 10 export and migration

See [the master migration/export contract](BACKPLANE_RIGID_FLEX_SPEC.md#kicad-10-export).
Stock KiCad 10 sees a downgraded flat native board and ignores structure in the
companion. V1 properties/endings migrate into native master fields; subsequent
saves write a v2 extension-only companion. The new headless migration script has
source/schema checks here but requires the separate host's build for runtime validation.

## Master port source checks and build verification

All changed C++ is uncompiled. There is no known unresolved master API mismatch
after source/header review; this does not establish that the port builds. The
following integration sites must receive first-build verification on the lead
host, followed by native tests. No compilation was attempted on the workstation.

| File:line | Unverified integration |
| --- | --- |
| `common/backplane_document_metadata.cpp:48` | v1/v2 reader, native precedence and companion serialization |
| `pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.cpp:3794` | native board load/save and root property migration |
| `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr.cpp:116` | schematic children/library metadata and SCH_LINE endings |
| `pcbnew/board.cpp:4529`, `eeschema/schematic.cpp:3226` | master history/autosave staging |
| `pcbnew/board_structure.cpp:321` | model/polygon APIs and unknown-field round trip |
| `pcbnew/tools/board_structure_tool.cpp:91`, `pcbnew/tools/board_structure_edit.cpp:158` | ResolveSnap().position and construction editing |
| `pcbnew/dialogs/panel_setup_rigid_flex.cpp:793`, `pcbnew/undo_redo.cpp:637` | GUI staging and transient structure undo |
| `pcbnew/drc/drc_test_provider_board_structure.cpp:52`, `pcbnew/zone_filler.cpp:3684` | structure DRC and regional fill clipping |
| `pcbnew/router/pns_kicad_iface.cpp:2405`, `qa/tests/pcbnew/test_structure_router.cpp:40` | temporary keepouts and real-net PNS queries |
| `3d-viewer/3d_rendering/opengl/create_scene.cpp:369`, `3d-viewer/3d_rendering/opengl/render_3d_opengl.cpp:1421` | deferred ownership, render mutex, folded footprint matrices |
| `3d-viewer/3d_rendering/raytracing/create_scene.cpp:465` | regional ray-tracing containers and material seating |
| `pcbnew/exporters/step/step_pcb_model.cpp:1908`, `pcbnew/exporters/step/step_pcb_model.cpp:2502` | OCC component transforms and folded solids |
| `pcbnew/api/api_pcb_structure.cpp:78`, `pcbnew/api/api_handler_pcb.cpp:2352` | GUI/headless registration and unchanged upstream job wrapper |
| `pcbnew/exporters/board_structure_fabrication.cpp:33`, `kicad/cli/command_pcb_structure.cpp:15` | Gerber plotter and CLI/job entry points |
| `qa/tests/pcbnew/test_backplane_document_metadata.cpp:45` and all ported structure/geometry QA sources | native unit-test compilation and execution |

Source checks completed: `git diff --check`; direct included master headers
compared with `git diff caf7377e9c fe761fb867 -- <header>`; upstream/member API
name/declaration inspection; master deferred-render and footprint-matrix adaptation;
protoc generation of all master/fork schemas; Python AST/import/constructor checks;
and byte/SHA256 equality for all nine authoritative fixture files. These source
checks do not substitute for C++ compilation, native migration, or visual tests.

The source port excludes assembly paths/messages/tests as assigned to the other
implementer. The removed snapshot `board_jobs.proto.folded` field is represented
by `kiapi.backplane.RunBoardStructureExport3D`; it is not an upstream message
extension. The old headless-board-context test's loader workaround is obsolete on
master and was not reapplied. No source feature within the assigned structure
scope was deliberately deferred; runtime validation remains outstanding.

Run on the built host: the migration script, structure and boundary IPC scripts,
structure fabrication/folded regression, regional routing regression, and native
PCB QA structure/geometry/metadata tests. Exercise GUI layer selection, drawing,
stack assignment, Cancel, Undo/Redo, save/copy/save-as, history/autosave recovery,
flat ray tracing and flat/folded OpenGL. Compare STEP in an independent reader.
The demo models retain `KICAD10_3DMODEL_DIR`; provide that library variable/alias
when running component/viewer tests on a master runtime.
