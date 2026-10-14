#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tessellate actual STEP solids for reproducible construction renders.

Requires cadquery-ocp. Color is a presentation choice, not inferred material.
"""
import argparse
import json
from pathlib import Path

from OCP.STEPControl import STEPControl_Reader
from OCP.IFSelect import IFSelect_RetDone
from OCP.TopAbs import TopAbs_SOLID, TopAbs_FACE, TopAbs_REVERSED
from OCP.TopExp import TopExp_Explorer
from OCP.TopoDS import TopoDS
from OCP.TopLoc import TopLoc_Location
from OCP.BRep import BRep_Tool
from OCP.BRepMesh import BRepMesh_IncrementalMesh

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--color", default="#d3a14b")
args = parser.parse_args()
reader = STEPControl_Reader()
if reader.ReadFile(str(args.source)) != IFSelect_RetDone or reader.TransferRoots() == 0:
    raise SystemExit("Could not read STEP geometry")
shape = reader.OneShape()
BRepMesh_IncrementalMesh(shape, .12, False, .3, True).Perform()
objects = TopExp_Explorer(shape, TopAbs_SOLID)
meshes = []
while objects.More():
    faces = TopExp_Explorer(objects.Current(), TopAbs_FACE)
    triangles = []
    while faces.More():
        face = TopoDS.Face(faces.Current())
        location = TopLoc_Location()
        mesh = BRep_Tool.Triangulation_s(face, location)
        if mesh is not None:
            transform = location.Transformation()
            for i in range(1, mesh.NbTriangles() + 1):
                triangle = [mesh.Node(k).Transformed(transform).Coord()
                            for k in mesh.Triangle(i).Get()]
                if face.Orientation() == TopAbs_REVERSED:
                    triangle.reverse()
                triangles.append(triangle)
        faces.Next()
    if triangles:
        meshes.append({"triangles": triangles, "color": args.color})
    objects.Next()
if not meshes:
    raise SystemExit("STEP contains no tessellated solids")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(meshes) + "\n")
print(f"Wrote {len(meshes)} solids to {args.output}")
