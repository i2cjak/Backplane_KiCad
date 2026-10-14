# SPDX-License-Identifier: GPL-3.0-or-later
"""Run with blender --background --python this_script.py -- mesh.json output.png.

Input contains real STEP triangles prepared with backplane-tessellate-step.py.
Colors are visualization choices; they do not assert exported material metadata.
"""
import bpy, json, math, argparse, sys
from pathlib import Path
from mathutils import Vector
parser=argparse.ArgumentParser(description="Render tessellated STEP geometry with Blender.")
parser.add_argument("mesh",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--underside",action="store_true")
parser.add_argument("--square",action="store_true",help="Render a 1600 by 1600 image")
args=parser.parse_args(sys.argv[sys.argv.index("--")+1:])
root=args.output.resolve().parent
root.mkdir(parents=True,exist_ok=True)
for stem,below in [(args.output.stem,args.underside)]:
 bpy.ops.wm.read_factory_settings(use_empty=True)
 scene=bpy.context.scene
 scene.render.engine='CYCLES'; scene.cycles.samples=48; scene.cycles.use_denoising=True
 scene.render.resolution_x=1600; scene.render.resolution_y=1600 if args.square else 900; scene.render.resolution_percentage=100
 scene.world=bpy.data.worlds.new('Studio'); scene.world.use_nodes=True
 scene.world.node_tree.nodes['Background'].inputs[0].default_value=(.12,.17,.22,1)
 scene.world.node_tree.nodes['Background'].inputs[1].default_value=.45
 points=[]
 for idx,data in enumerate(json.loads(args.mesh.read_text())):
  verts=[tuple(v) for tri in data['triangles'] for v in tri]
  if below: verts=[(x,y,-z) for x,y,z in verts]
  points.extend(verts)
  faces=[(i+2,i+1,i) if below else (i,i+1,i+2) for i in range(0,len(verts),3)]
  mesh=bpy.data.meshes.new('STEP solid'); mesh.from_pydata(verts,[],faces); mesh.update()
  obj=bpy.data.objects.new('Construction '+str(idx),mesh); scene.collection.objects.link(obj)
  mat=bpy.data.materials.new('Material'); mat.use_nodes=True
  color=data['color']
  srgb=tuple(int(color[i:i+2],16)/255 for i in (1,3,5))
  rgb=tuple(c/12.92 if c<=.04045 else ((c+.055)/1.055)**2.4 for c in srgb)
  bsdf=mat.node_tree.nodes.get('Principled BSDF'); bsdf.inputs['Base Color'].default_value=(*rgb,1)
  bsdf.inputs['Roughness'].default_value=.38
  obj.data.materials.append(mat)
 low=Vector(tuple(min(p[k] for p in points) for k in range(3)))
 high=Vector(tuple(max(p[k] for p in points) for k in range(3)))
 center=(low+high)/2; span=high-low
 bpy.ops.object.camera_add(location=center+Vector((span.x*.9,-span.x*1.2,span.x*.65)))
 camera=bpy.context.object; camera.rotation_euler=(center-camera.location).to_track_quat('-Z','Y').to_euler()
 camera.data.type='ORTHO'
 rotation=camera.rotation_euler.to_quaternion().inverted()
 projected=[rotation @ (Vector(p)-center) for p in points]
 width=max(p.x for p in projected)-min(p.x for p in projected)
 height=max(p.y for p in projected)-min(p.y for p in projected)
 aspect=scene.render.resolution_x/scene.render.resolution_y
 camera.data.ortho_scale=max(width,height*aspect)*1.18
 scene.camera=camera
 bpy.ops.mesh.primitive_plane_add(size=1000,location=(center.x,center.y,low.z-1.5))
 plane=bpy.context.object
 mat=bpy.data.materials.new('Slate'); mat.diffuse_color=(.035,.05,.065,1); plane.data.materials.append(mat)
 for position,power,size in [(center+Vector((-40,-60,100)),180000,100),(center+Vector((80,40,70)),100000,80)]:
  bpy.ops.object.light_add(type='AREA',location=position); light=bpy.context.object
  light.data.energy=power; light.data.shape='DISK'; light.data.size=size
  light.rotation_euler=(center-light.location).to_track_quat('-Z','Y').to_euler()
 scene.render.image_settings.file_format='PNG'
 scene.render.filepath=str(args.output.resolve())
 bpy.ops.wm.save_as_mainfile(filepath=str(root/(stem+'.blend')))
 bpy.ops.render.render(write_still=True)
