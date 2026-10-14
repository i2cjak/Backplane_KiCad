# Board construction examples

These boards use stock KiCad 10.0.6 native artwork plus adjacent
`.kicad_pcb.backplane.json` construction companions. Keep each pair together.
All coordinates in the companion are native nanometres unless a field explicitly
uses `_mm` or degrees.

- **reinforced-flex**: two copper layers on a 0.15 mm flex substrate, top coverlay,
  bottom 0.30 mm FR4 terminal stiffener with 0.025 mm adhesive, and a 90 degree fold.
- **rigid-flex-bridge**: two 1.6 mm rigid islands joined by a 0.15 mm flex bridge.
  In1.Cu and In2.Cu remain continuous at the same physical height through all
  regions. The outer copper exists only on the rigid islands.
- **two-bend-u-fold**: the bridge receives sequential 90 degree bends at x=55 and
  x=70 mm, returning the moving island to a parallel plane above the fixed island.
  It also carries SOIC-8 and 0805 SMD package fixtures on both islands for model
  seating/orientation checks. These additional SMD pads have no assigned nets.

All three examples have two routed nets and four drilled through-hole terminals.
Each terminal references the stock KiCad 1×1 vertical 2.54 mm pin-header STEP
model through `KICAD10_3DMODEL_DIR`; install the KiCad 3D model library to view them.
The bend starts at x=55 mm; the positive-X side moves with a 2 mm inner radius.
The reinforcement was explicitly placed at the terminal area; there is no
automatic mechanical stiffener-placement solver.

In Board Setup, use **Rigid-Flex Structure** to edit the region, regional stack,
stiffener, coverlay and bend definitions. The regional stack layer editor controls
material thickness and physical Z offsets independently of native copper identity.

Local regression commands (use a freshly built fork executable):

```sh
kicad-cli pcb structure rigid-flex-bridge.kicad_pcb
kicad-cli pcb export gerbers rigid-flex-bridge.kicad_pcb --output fabrication/
kicad-cli pcb export drill rigid-flex-bridge.kicad_pcb --output fabrication/
kicad-cli pcb export step rigid-flex-bridge.kicad_pcb --no-components --output flat.step
```

Construction Gerbers describe positive material areas, with clear holes/openings.
Bend-area Gerbers are reference drawings. They are not copper, milling, or material
layers. `board-construction.json` identifies each output and carries the material,
adhesive, thickness and bend specification. Native copper and fabrication
artwork always remain in the flat developed state.

Regenerate the source examples with the stock KiCad Python interpreter:

```sh
python3 scripts/backplane-create-structure-exemplars.py
python3 scripts/backplane-populate-smd-exemplar.py demos/backplane-structure/two-bend-u-fold.kicad_pcb
```

Run the independent local regression using an environment containing
`gerbonara` and `cadquery-ocp`:

```sh
python scripts/backplane-structure-regression.py /path/to/fork/kicad-cli \
  --stock-cli /usr/bin/kicad-cli --output /tmp/structure-regression --folded
```

See the repository research specification for the intended broader feature set.
These are engineering test fixtures, not fabrication-qualified product designs.

To render the actual exported construction solids locally, install `cadquery-ocp`
in your Python environment and use Blender:

```sh
python scripts/backplane-tessellate-step.py folded.step /tmp/folded-mesh.json
blender --background --python scripts/backplane-render-construction.py -- \
  /tmp/folded-mesh.json /tmp/folded.png
```

The renderer also saves a `.blend` scene beside the PNG. Add `--square` for a 1600 × 1600 image. Use `--underside` to
inspect a bottom stiffener. Colors are presentation choices; use the construction
manifest to identify actual materials and thicknesses.
