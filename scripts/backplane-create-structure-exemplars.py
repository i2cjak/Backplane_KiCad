#!/usr/bin/env python3
"""Regenerate the stock-compatible structure examples using KiCad's pcbnew module.

Run with a Python interpreter provided by the local KiCad installation.
Outputs are fixed, named example boards, never a user's active design.
"""
from pathlib import Path
import json
import uuid
import argparse
import pcbnew as pcb

ROOT = None
NS = uuid.UUID("a10f9fbd-03a2-4876-9680-295bc45c775d")


def uid(name):
    return str(uuid.uuid5(NS, name))


def poly(x1, y1, x2, y2):
    return {"outline": [[round(x*1e6), round(y*1e6)]
                        for x, y in [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]], "holes": []}


def stack(name, flex, copper):
    total = 0.15 if flex else 1.6
    # Shared inner copper and polyimide occupy identical global Z coordinates.
    # Rigid islands laminate FR4 and outer copper around the continuous flex core.
    if flex:
        construction = [("copper", "Copper", .018, copper[-1]),
                        ("dielectric", "Polyimide", .114, -1),
                        ("copper", "Copper", .018, copper[0])]
    else:
        construction = [("copper", "Copper", .035, copper[-1]),
                        ("dielectric", "FR4", .690, -1),
                        ("copper", "Copper", .018, copper[-2]),
                        ("dielectric", "Polyimide", .114, -1),
                        ("copper", "Copper", .018, copper[1]),
                        ("dielectric", "FR4", .690, -1),
                        ("copper", "Copper", .035, copper[0])]
    materials, layers = [], []
    z = -total / 2
    for kind, material, thickness, cu in construction:
        key = f"{name}-{len(materials)}"
        materials.append({"id": key, "category": kind, "material": material,
                          "thickness_nm": round(thickness*1e6),
                          "min_thickness_nm": round(thickness*1e6),
                          "max_thickness_nm": round(thickness*1e6)})
        layers.append({"material_id": key, "global_copper_layer": cu, "z_offset_nm": round(z*1e6)})
        z += thickness
    return {"id": name, "name": name, "kind": "flex" if flex else "rigid",
            "copper_layers": copper, "materials": materials, "layers": layers,
            "continuity_id": "shared-copper"}


def make(name, rigid, multi_bend=False):
    pcb.KIID.SeedGenerator( 42 if rigid else 43 )
    board = pcb.BOARD()
    board.SetCopperLayerCount(4 if rigid else 2)
    for i, (a, b) in enumerate([((20,20),(100,20)), ((100,20),(100,40)),
                                ((100,40),(20,40)), ((20,40),(20,20))]):
        shape = pcb.PCB_SHAPE(board)
        shape.SetShape(pcb.SHAPE_T_SEGMENT)
        shape.SetStart(pcb.VECTOR2I(*(round(v*1e6) for v in a)))
        shape.SetEnd(pcb.VECTOR2I(*(round(v*1e6) for v in b)))
        shape.SetLayer(pcb.Edge_Cuts)
        shape.SetWidth(pcb.FromMM(0.05))
        board.Add(shape)
    route_layer = pcb.In1_Cu if rigid else pcb.F_Cu
    for n, y in enumerate((25,35), 1):
        net = pcb.NETINFO_ITEM(board, f"SIGNAL_{n}")
        board.Add(net)
        for ref, x in ((f"J{n}",25), (f"J{n+2}",95)):
            fp = pcb.FOOTPRINT(board)
            fp.SetReference(ref)
            fp.SetValue("Test terminal")
            model = pcb.FP_3DMODEL()
            model.m_Filename = "${KICAD10_3DMODEL_DIR}/Connector_PinHeader_2.54mm.3dshapes/PinHeader_1x01_P2.54mm_Vertical.step"
            fp.Models().push_back(model)
            fp.SetPosition(pcb.VECTOR2I(pcb.FromMM(x),pcb.FromMM(y)))
            pad = pcb.PAD(fp)
            pad.SetNumber("1")
            pad.SetAttribute(pcb.PAD_ATTRIB_PTH)
            pad.SetShape(pcb.PAD_SHAPE_CIRCLE)
            pad.SetSize(pcb.VECTOR2I(pcb.FromMM(2),pcb.FromMM(2)))
            pad.SetDrillSize(pcb.VECTOR2I(pcb.FromMM(1),pcb.FromMM(1)))
            layers = pcb.LSET.AllCuMask()
            layers.AddLayer(pcb.F_Mask)
            layers.AddLayer(pcb.B_Mask)
            pad.SetLayerSet(layers)
            pad.SetPosition(fp.GetPosition())
            pad.SetNet(net)
            fp.Add(pad)
            board.Add(fp)
        track = pcb.PCB_TRACK(board)
        track.SetStart(pcb.VECTOR2I(pcb.FromMM(25),pcb.FromMM(y)))
        track.SetEnd(pcb.VECTOR2I(pcb.FromMM(95),pcb.FromMM(y)))
        track.SetWidth(pcb.FromMM(0.4))
        track.SetLayer(route_layer)
        track.SetNet(net)
        board.Add(track)
    flex_id = "flex"
    structure = {"schema_version": 1, "stacks": [], "regions": [],
                 "boundaries": [], "region_seeds": [], "stiffeners": [],
                 "coverlays": [], "bends": []}
    if rigid:
        structure["stacks"] = [stack("rigid",False,[0,4,6,2]), stack("flex",True,[4,6])]
        structure["regions"] = [
            {"id":"left","name":"Fixed island","stack_id":"rigid","shape":poly(20,20,45,40)},
            {"id":flex_id,"name":"Flex bridge","stack_id":"flex","shape":poly(45,20,75,40)},
            {"id":"right","name":"Moving island","stack_id":"rigid","shape":poly(75,20,100,40)}]
        structure["boundaries"] = [
            {"id":"boundary-left-flex", "polyline":[[45000000,20000000],[45000000,40000000]]},
            {"id":"boundary-flex-right", "polyline":[[75000000,20000000],[75000000,40000000]]}]
        structure["region_seeds"] = [
            {"region_id":"left", "point":[32500000,30000000]},
            {"region_id":flex_id, "point":[60000000,30000000]},
            {"region_id":"right", "point":[87500000,30000000]}]
    else:
        structure["stacks"] = [stack("flex",True,[0,2])]
        structure["regions"] = [{"id":flex_id,"name":"Reinforced flex","stack_id":"flex","shape":poly(20,20,100,40)}]
        structure["region_seeds"] = [{"region_id":flex_id, "point":[60000000,30000000]}]
        structure["stiffeners"] = [{"id":"support","region_id":flex_id,"shape":poly(85,20,100,40),
            "material":"FR4","adhesive":"Thermoset","thickness_nm":300000,
            "min_thickness_nm":280000,"max_thickness_nm":320000,"adhesive_thickness_nm":25000,
            "top":False,"role":"Terminal support","support_targets":[]}]
    structure["coverlays"] = [{"id":"cover-top","region_id":flex_id,
        "coverage":poly(45 if rigid else 30,20,75 if rigid else 85,40),"openings":[],
        "material":"Polyimide","adhesive":"Acrylic","thickness_nm":25000,
        "adhesive_thickness_nm":25000,"top":True,"registration_tolerance_nm":75000}]
    structure["bends"] = [{"id":"fold-1","region_id":flex_id,"start":[55000000,40000000],
        "end":[55000000,20000000],"angle_degrees":90,"inner_radius_mm":2,
        "neutral_axis_offset_mm":-1,"moving_side_left":True,"sequence":0}]
    if multi_bend:
        structure["bends"].append({"id":"fold-2","region_id":flex_id,"start":[70000000,40000000],
            "end":[70000000,20000000],"angle_degrees":90,"inner_radius_mm":2,
            "neutral_axis_offset_mm":-1,"moving_side_left":True,"sequence":1})
    filename = ROOT / (name + ".kicad_pcb")
    pcb.SaveBoard(str(filename),board)
    filename.with_suffix(".kicad_pro").write_text(json.dumps({
        "meta": {"filename": filename.with_suffix(".kicad_pro").name, "version": 3}
    }, indent=2)+"\n")
    companion = {"format":"backplane-kicad-metadata","version":2,"extensions":{"root":{"board_structure":structure}}}
    filename.with_suffix(filename.suffix+".backplane.json").write_text(json.dumps(companion,indent=2)+"\n")
    return filename


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    ROOT = args.output.resolve()
    fixtures = Path(__file__).resolve().parents[1] / "demos/backplane-structure"
    if ROOT == fixtures.resolve() or fixtures.resolve() in ROOT.parents:
        parser.error("output must be outside the immutable migration fixture directory")
    ROOT.mkdir(parents=True,exist_ok=True)
    for name, rigid, multi in [("reinforced-flex",False,False),
                               ("rigid-flex-bridge",True,False),
                               ("two-bend-u-fold",True,True)]:
        print(make(name, rigid, multi))
