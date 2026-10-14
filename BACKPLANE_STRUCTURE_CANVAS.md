# Rigid-flex construction layers

Master-port status (2026-10-14): source port only, based on `fe761fb867`
(KiCad 10.99) and WIP snapshot `1747f64599`. No C++ compilation, native GUI,
DRC, routing, viewer, or STEP runtime checks were performed for this port.
Historical builds below refer to the 10.0.6 fork. The lead must validate on the
separate build host before treating this as a working runtime.

The **Appearance → Layers** list has four construction layers beside **Edge.Cuts**:
**Stackup.Boundary**, **Bend.Lines**, **F.Stiffener**, and **B.Stiffener**.
They also appear in the PCB editor toolbar layer selector. Each has independent
visibility. Native copper layers retain their normal identities and behavior.

1. Draw the closed outside outline and cutouts on **Edge.Cuts**.
2. Select **Stackup.Boundary** and use Draw Line, or double-click the layer row.
   Click the internal boundary vertices and double-click the final point to finish.
   The boundary must join outline edges or existing partition edges. Escape cancels.
   Finite crosscuts, polylines, and T joins divide the outline into regions.
3. The first cut prompts for a starter rigid stack if regions have no stack.
   All new regions receive that stack. Open **Rigid-flex tools → Assign stack**,
   click a region, choose rigid/flex and a matching stack or create a starter.
   **Edit stackup** opens the detailed regional physical stack editor.
4. Select **Bend.Lines**, draw the two axis endpoints, and enter the flex region,
   radius, angle, moving side, and sequence.
5. Select **F.Stiffener** or **B.Stiffener**, draw a closed polygon, and set
   material, thickness, adhesive, and parent region. The selected layer supplies
   the initial attachment side.
6. With a construction layer selected, **E** enters construction editing.
   Boundary editing selects a vertex then its new position; right-click a vertex
   removes it (or removes a two-point boundary). Other contexts open feature
   properties after selecting a feature. Escape cancels staged changes.

Starter stacks are editable two-layer examples, not manufacturer stack definitions.
Review materials, copper continuity, thicknesses, and tolerances for fabrication.

Drawing and property changes use ordinary Undo/Redo. Save writes the physical model
to the adjacent `.kicad_pcb.backplane.json`; keep it with the native board.
Construction layers have separate internal identities and are never native PCB
layer IDs or file tokens. Structure is a fork companion extension. Stock KiCad 10
receives a downgraded native copy and ignores the companion.
Fabrication artwork remains flat Gerbers; folded STEP/GLB is a separate output.

The native OpenGL construction view includes regional copper, exposed pads,
mask openings, silk, and component models. Artwork is clipped at stack boundaries
and uses the same fold evaluator as the substrate. Flat construction uses the same
regional artwork with its local Z placement. Via barrel walls and depth-specific
machining are not yet represented by this artwork path.

The IPC commands `GetConstructionLayerState` and `SetConstructionLayerState`
control the editor context and visibility. `DeriveBoardStructurePartition` accepts
a candidate JSON model, derives regions from the open board's Edge.Cuts, validates,
and commits atomically; `validate_only` previews the result. Existing
Get/SetBoardStructure commands remain available.

Current limits: boundaries are processed in construction order; unsupported
crossings and cuts ending on cutouts are rejected. Native Edge.Cuts edits require
an explicit partition rebuild (through boundary editing or IPC). Whole-feature
handles/dragging and canvas coverlay authoring remain future work; coverlay is
configured in Board Setup.

## Historical 10.0.6 validation, builds 33–35

Tests used the disposable `~/.cache/backplane-flex-tests/layers-ui` board on Xvfb
`:97`; the user's desktop board was not edited.

- Drew rigid and flex regions from an empty construction model, created both
  stacks, and added a bend and stiffener. IPC confirmed all counts and validity.
- Undo/Redo removed/restored a region together with its new stack.
- Escape during a canvas preview left the structure unchanged.
- Click-to-edit renamed a region. Deleting a flex region removed its bend and
  stiffener; one Undo restored all three.
- Save/reopen preserved the geometry and properties. Stock 10.0.6 reopened a copy
  and left its companion byte-for-byte unchanged.
- Checked native layer-list usability at 1280×720 and the full workspace at
  1600×1100. Pixel checks confirmed flex visibility hides/restores the overlay.
- Corrected overlay depth so stiffeners and bends appear above region fills;
  inspected the rebuilt native GUI.
- Build 34: 42 Backplane unit cases / 167 assertions passed. Headless IPC tests
  passed construction, assemblies, stock reopen, folded STEP, and Gerber export.

Evidence: `structure-canvas35.png`, `stock-reopen.log`, and `drawn-structure.json`
in the test directory; `/tmp/backplane-unit-34.log`, `/tmp/backplane-ipc-34.log`.

## Historical 10.0.6 validation, build 42 (2026-09-12)

- Built PCB editor, CLI, and PCB QA executable locally.
- 49 Backplane test cases / 198 assertions passed, including finite crosscuts,
  T joins, L polylines, cutout area preservation, disconnected islands, stable
  repeated derivation, stack inheritance, and atomic rejection of spanning features.
- Native GUI on Xvfb: first boundary from an empty construction model, starter
  stack creation, flex assignment, T join, bottom stiffener, layer/toolbar selection,
  visibility, Undo/Redo, and save were exercised on disposable boards.
- Construction/assembly IPC regression passed; boundary IPC passed derivation,
  validate-only and invalid-edit atomicity, save/reopen, and Gerber export.
- All three source exemplar partitions rebuilt with IDs, stack assignments,
  polygon areas, bends, coverlays, and stiffeners preserved (list order normalized).
- Native OpenGL flat and folded views were inspected with SMD models, exposed
  pads, silkscreen, mask, and traces visible beneath translucent coverlay.
  The viewer's first IPC open now retains the requested folded state.

Evidence is in `~/.cache/backplane-flex-tests/final-validation/`, including
`build42.log`, `unit42.log`, `structure-ipc.log`, `boundary-ipc.log`, and
`exemplar-partitions.log`. Screenshots are actual native viewer/editor captures.
Those historical checks used the 10.0.6 fork and v1 companions. The master port
now migrates per-item fields into native master tokens and writes v2 companions.

## KiCad 10 export and migration

See [the master migration/export contract](BACKPLANE_RIGID_FLEX_SPEC.md#kicad-10-export).
Stock KiCad 10 sees a downgraded flat native board and ignores structure in the
companion. V1 properties/endings migrate into native master fields; subsequent
saves write a v2 extension-only companion. The new headless migration script has
source/schema checks here but requires the separate host's build for runtime validation.
