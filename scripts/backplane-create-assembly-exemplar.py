#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the mechanical STEP part used by the assembly example.

Requires cadquery-ocp. The part is a simple 20 x 10 x 2 mm mechanical block.
"""
from pathlib import Path
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.IFSelect import IFSelect_RetDone

target = Path(__file__).resolve().parents[1] / "demos/backplane-assembly/mechanical-block.step"
writer = STEPControl_Writer()
writer.Transfer(BRepPrimAPI_MakeBox(20.0, 10.0, 2.0).Shape(), STEPControl_AsIs)
if writer.Write(str(target)) != IFSelect_RetDone:
    raise SystemExit("Could not export mechanical block")
