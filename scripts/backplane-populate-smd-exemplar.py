#!/usr/bin/env python3
"""Add stock, unconnected SMD packages to the rigid-flex 3D test exemplar.

Usage: system-KiCad-python this-script.py example.kicad_pcb
These are mechanical visualization fixtures, not an electrically specified circuit.
The existing board path and adjacent construction companion remain authoritative.
"""
from pathlib import Path
import sys
import pcbnew as pcb

board_path = Path(sys.argv[1]).resolve()
board = pcb.LoadBoard(str(board_path))
packages = [
    ('U1', 'Package_SO', 'SOIC-8_3.9x4.9mm_P1.27mm', 36, 30, 0),
    ('U2', 'Package_SO', 'SOIC-8_3.9x4.9mm_P1.27mm', 84, 30, 90),
    ('R1', 'Resistor_SMD', 'R_0805_2012Metric', 33, 25, 0),
    ('R2', 'Resistor_SMD', 'R_0805_2012Metric', 80, 25, 90),
    ('C1', 'Capacitor_SMD', 'C_0805_2012Metric', 40, 35, 0),
    ('C2', 'Capacitor_SMD', 'C_0805_2012Metric', 88, 35, 90),
]
refs = {row[0] for row in packages}
for footprint in list(board.GetFootprints()):
    if footprint.GetReference() in refs:
        board.Remove(footprint)
for ref, library, name, x, y, angle in packages:
    footprint = pcb.FootprintLoad(f'/usr/share/kicad/footprints/{library}.pretty', name)
    if footprint is None:
        raise RuntimeError(f'Missing stock footprint {library}:{name}')
    footprint.SetReference(ref)
    footprint.SetValue('3D package fixture')
    footprint.Value().SetVisible(False)
    footprint.SetPosition(pcb.VECTOR2I(pcb.FromMM(x), pcb.FromMM(y)))
    footprint.SetOrientationDegrees(angle)
    footprint.Models().clear()
    model = pcb.FP_3DMODEL()
    model.m_Filename = '${KICAD10_3DMODEL_DIR}/' + library + '.3dshapes/' + name + '.step'
    footprint.Models().push_back(model)
    board.Add(footprint)
pcb.SaveBoard(str(board_path), board)
print(board_path)
