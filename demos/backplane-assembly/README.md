# Assembly examples

- `two-native-boards.backplane-assembly.json` combines independent native boards
  at separate placements.
- `two-native-boards-collision.backplane-assembly.json` deliberately overlaps
  the boards; `pcb assembly ... --check` must return exit code 5.
- `two-board.backplane-assembly.json` combines a native board with
  `mechanical-block.step`. A point mate places the 20 × 10 × 2 mm mechanical part
  at (45, -35, 5) mm in assembly coordinates.
- `nested-axis.backplane-assembly.json` demonstrates a rotated nested module
  whose parent uses an axis mate.
- `electrical-links.backplane-assembly.json` demonstrates a logical link between
  P2 pad 1 (`A`) on two native boards.

Open **Tools → Assembly**, then **Open** to add manifests as independent tabs.
**View 3D** prepares that tab’s unfolded STEP output for the future assembly
viewer. **Move Component…** edits
the selected footprint in its source PCB, so all instances of that source share
the change. Use **Sync Electrical** to refresh the authored links and save the
manifest to retain the snapshot.

CLI examples:

```sh
kicad-cli pcb assembly demos/backplane-assembly/two-native-boards.backplane-assembly.json --check
kicad-cli pcb assembly demos/backplane-assembly/two-board.backplane-assembly.json --output assembly.step
```

Keep relative source paths and board companions together. The mechanical block
can be regenerated with `scripts/backplane-create-assembly-exemplar.py` using
an environment containing `cadquery-ocp`.

Point and axis mate coordinates are in millimeters, with XYZ rotations in
degrees. Electrical synchronization refreshes logical net names in the
manifest snapshot; it does not rewire source board files.
