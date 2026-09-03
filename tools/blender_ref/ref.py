#!/usr/bin/env python
"""External displacement reference: Blender (bpy module) displaces the REVIVAL greets stone mesh
with the same height map, amplitude, mid-level and camera as the engine's bake.

usage (from the venv python):
  ref.py --obj greets_stone_authored.obj --height greets_wall_h_mip2.png --albedo greets_wall.png
         --cam "x,y,z,dx,dy,dz" --fov 58.1092 --yscale 0.75 --amp 0.3 --mid 0.547 --levels 5
         --arm authored|welded|tfix --look clay|tex --out prefix [--export displaced.obj] [--samples 32]

Coordinate map FDS -> Blender: (x, y, z) -> (x, z, y)  (FDS is y-up left-handed; the swap makes it
right-handed z-up and keeps the picture orientation). Camera basis from the engine's Kick_Camera:
forward V, right N = (V.z, 0, -V.x) normalised, up U = V x N; horizontal FOV = fov; the engine's
PerspY = yscale * PerspX (AspectRatio * YRes/XRes) is reproduced with the render pixel aspect.
"""
import argparse, math, os, sys
import bpy, bmesh
from mathutils import Vector, Matrix

ap = argparse.ArgumentParser()
ap.add_argument("--obj", required=True); ap.add_argument("--height", required=True); ap.add_argument("--albedo")
ap.add_argument("--cam", required=True); ap.add_argument("--fov", type=float, required=True)
ap.add_argument("--yscale", type=float, default=0.75)
ap.add_argument("--amp", type=float, default=0.3); ap.add_argument("--mid", type=float, default=0.547)
ap.add_argument("--levels", type=int, default=5)
ap.add_argument("--arm", default="welded", choices=["authored", "welded", "tfix", "bare"])
ap.add_argument("--look", default="clay", choices=["clay", "tex", "matviz"])
ap.add_argument("--out", required=True); ap.add_argument("--export")
ap.add_argument("--samples", type=int, default=32)
ap.add_argument("--xres", type=int, default=1920); ap.add_argument("--yres", type=int, default=1080)
ap.add_argument("--weld", type=float, default=1e-3, help="merge-by-distance threshold, world u")
ap.add_argument("--sun", default="0.3,-0.5,0.8", help="sun direction TOWARDS the light, FDS coords x,y,z")
ap.add_argument("--pax", type=float, default=0.0); ap.add_argument("--pay", type=float, default=0.0)
ap.add_argument("--adaptive", type=float, default=0.0, help="> 0: Cycles adaptive micro-displacement at this dicing rate (px) instead of the Displace modifier")
A = ap.parse_args(sys.argv[1:] if "--" not in sys.argv else sys.argv[sys.argv.index("--") + 1:])

def fds_to_bl(x, y, z): return Vector((x, z, y))

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

# ── mesh ──────────────────────────────────────────────────────────────────
bpy.ops.wm.obj_import(filepath=A.obj, forward_axis='Y', up_axis='Z', use_split_objects=False, use_split_groups=False)
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
assert objs, "no mesh imported"
if len(objs) > 1:
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
ob = bpy.context.view_layer.objects.active if len(objs) > 1 else objs[0]
ob.name = "stone"
# the importer read raw (x,y,z); apply the FDS->Blender swap to the vertex data
me = ob.data
for v in me.vertices:
    x, y, z = v.co
    v.co = fds_to_bl(x, y, z)
# the swap is a reflection: flip winding so face normals keep the engine's facing
bm = bmesh.new(); bm.from_mesh(me)
bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
nv0, nf0 = len(bm.verts), len(bm.faces)
if A.arm in ("welded", "tfix"):
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=A.weld)
nv1 = len(bm.verts)
ntj = 0
if A.arm == "tfix":
    # T-junctions: a vertex lying strictly inside another face's edge -> split that edge there
    bm.verts.ensure_lookup_table(); bm.edges.ensure_lookup_table()
    eps = A.weld
    changed = True; rounds = 0
    while changed and rounds < 4:
        changed = False; rounds += 1
        edges = list(bm.edges)
        for e in edges:
            if not e.is_valid: continue
            a, b = e.verts[0].co, e.verts[1].co
            ab = b - a; L = ab.length
            if L < eps: continue
            d = ab / L
            for v in list(bm.verts):
                if not v.is_valid or v in e.verts: continue
                t = (v.co - a).dot(d)
                if t <= eps or t >= L - eps: continue
                if ((v.co - a) - d * t).length > eps: continue
                ne, nv = bmesh.utils.edge_split(e, e.verts[0], t / L)
                bmesh.ops.pointmerge(bm, verts=[nv, v], merge_co=v.co)
                ntj += 1; changed = True
                break
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=eps)
    # a split edge leaves an n-gon with collinear corners; re-triangulate so
    # the subdivision sees proper triangles, not slivers along the T line
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method='BEAUTY', ngon_method='BEAUTY')
bm.to_mesh(me); bm.free()
me.update()
print("[REF] mesh: %d verts / %d faces authored -> %d verts after weld (arm %s), %d T-junctions split" % (nv0, nf0, nv1, A.arm, ntj))
# smooth shading so the displace normals are the vertex normals (as the engine's smooth N)
for p in me.polygons: p.use_smooth = True

# ── height texture (Non-Color, repeat, linear, no mipmaps) ────────────────
himg = bpy.data.images.load(A.height); himg.colorspace_settings.name = 'Non-Color'
tex = bpy.data.textures.new("height", type='IMAGE'); tex.image = himg
tex.extension = 'REPEAT'; tex.use_interpolation = True; tex.use_mipmap = False
tex.use_calculate_alpha = False; tex.use_alpha = False

if A.arm != "bare" and A.adaptive <= 0:
    sub = ob.modifiers.new("subdiv", 'SUBSURF'); sub.subdivision_type = 'SIMPLE'
    sub.levels = A.levels; sub.render_levels = A.levels
    disp = ob.modifiers.new("displace", 'DISPLACE'); disp.texture = tex
    disp.texture_coords = 'UV'; disp.uv_layer = me.uv_layers[0].name
    disp.direction = 'NORMAL'; disp.strength = A.amp; disp.mid_level = A.mid; disp.space = 'LOCAL'
elif A.arm != "bare":
    # Cycles adaptive subdivision + true displacement in the material: the
    # surface is diced at render time to ~dicing_rate pixels per micro-polygon
    sub = ob.modifiers.new("subdiv", 'SUBSURF'); sub.subdivision_type = 'SIMPLE'
    sub.levels = 0; sub.render_levels = 0
    ob.cycles.use_adaptive_subdivision = True; ob.cycles.dicing_rate = 1.0

# ── material ──────────────────────────────────────────────────────────────
mat = bpy.data.materials.new("stone"); mat.use_nodes = True
nodes = mat.node_tree.nodes; links = mat.node_tree.links
bsdf = nodes["Principled BSDF"]; bsdf.inputs["Roughness"].default_value = 0.9
if A.look == "tex" and A.albedo:
    aimg = bpy.data.images.load(A.albedo)
    tn = nodes.new("ShaderNodeTexImage"); tn.image = aimg; tn.extension = 'REPEAT'
    links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
else:
    bsdf.inputs["Base Color"].default_value = (0.62, 0.58, 0.50, 1.0)
if A.adaptive > 0 and A.arm != "bare":
    # material displacement: scale * (height - midlevel) along the normal, same formula as the engine
    hn = nodes.new("ShaderNodeTexImage"); hn.image = himg; hn.extension = 'REPEAT'; hn.interpolation = 'Linear'
    dn = nodes.new("ShaderNodeDisplacement"); dn.space = 'OBJECT'
    dn.inputs["Midlevel"].default_value = A.mid; dn.inputs["Scale"].default_value = A.amp
    links.new(hn.outputs["Color"], dn.inputs["Height"])
    out = nodes["Material Output"]; links.new(dn.outputs["Displacement"], out.inputs["Displacement"])
    mat.displacement_method = 'DISPLACEMENT'
if A.look == "matviz":
    # flat emission colour per imported material slot (rooms / floor), for locating boundaries
    cols = [(1.0, 0.1, 0.1, 1.0), (0.1, 1.0, 0.1, 1.0), (0.1, 0.1, 1.0, 1.0), (1.0, 1.0, 0.1, 1.0)]
    for i, slot in enumerate(me.materials):
        m = bpy.data.materials.new("mv%d" % i); m.use_nodes = True
        n = m.node_tree.nodes; n.clear()
        em = n.new("ShaderNodeEmission"); em.inputs["Color"].default_value = cols[i % len(cols)]; em.inputs["Strength"].default_value = 1.0
        o = n.new("ShaderNodeOutputMaterial"); m.node_tree.links.new(em.outputs[0], o.inputs[0])
        print("[REF] matviz slot %d '%s' -> colour %s" % (i, slot.name if slot else "?", cols[i % len(cols)][:3]))
        me.materials[i] = m
else:
    me.materials.clear(); me.materials.append(mat)

# ── camera (engine Kick_Camera basis, FDS->Blender map) ───────────────────
c = [float(t) for t in A.cam.split(",")]
src = Vector(c[:3]); V = Vector(c[3:]).normalized()
N = Vector((V.z, 0.0, -V.x)).normalized(); U = V.cross(N)
srcB, VB, NB, UB = fds_to_bl(*src), fds_to_bl(*V), fds_to_bl(*N), fds_to_bl(*U)
camd = bpy.data.cameras.new("cam"); camo = bpy.data.objects.new("cam", camd)
scene.collection.objects.link(camo); scene.camera = camo
R = Matrix((( NB.x, UB.x, -VB.x), ( NB.y, UB.y, -VB.y), ( NB.z, UB.z, -VB.z)))  # columns: right, up, -forward
camo.matrix_world = Matrix.Translation(srcB) @ R.to_4x4()
camd.sensor_fit = 'HORIZONTAL'; camd.angle = math.radians(A.fov)
camd.clip_start = 0.05; camd.clip_end = 2000.0
scene.render.resolution_x = A.xres; scene.render.resolution_y = A.yres; scene.render.resolution_percentage = 100
# engine PerspY = yscale * PerspX. Blender's pixel aspect semantics are settled
# empirically (--pax/--pay override; see the correlation test in the log).
if A.pax > 0 and A.pay > 0:
    scene.render.pixel_aspect_x = A.pax; scene.render.pixel_aspect_y = A.pay
else:
    scene.render.pixel_aspect_x = A.yscale; scene.render.pixel_aspect_y = 1.0
print("[REF] pixel aspect x=%.4f y=%.4f, sensor_fit %s, angle %.4f deg" % (scene.render.pixel_aspect_x, scene.render.pixel_aspect_y, camd.sensor_fit, math.degrees(camd.angle)))

# ── light + render ────────────────────────────────────────────────────────
sd = [float(t) for t in A.sun.split(",")]
sun = bpy.data.lights.new("sun", type='SUN'); sun.energy = 3.0; sun.angle = math.radians(4)
suno = bpy.data.objects.new("sun", sun); scene.collection.objects.link(suno)
sdir = fds_to_bl(*sd).normalized()   # towards the light; Blender sun points along its -Z
suno.rotation_euler = (-sdir).to_track_quat('-Z', 'Y').to_euler()
scene.world = bpy.data.worlds.new("w"); scene.world.use_nodes = True
scene.world.node_tree.nodes["Background"].inputs[0].default_value = (0.35, 0.37, 0.40, 1.0)
scene.world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
scene.render.engine = 'CYCLES'; scene.cycles.device = 'CPU'
if A.adaptive > 0:
    scene.cycles.feature_set = 'EXPERIMENTAL'
    scene.cycles.dicing_rate = A.adaptive; scene.cycles.preview_dicing_rate = A.adaptive
    scene.cycles.max_subdivisions = 10
scene.cycles.samples = A.samples; scene.cycles.use_denoising = False
scene.render.image_settings.file_format = 'PNG'
scene.render.filepath = A.out + ".png"
scene.view_settings.view_transform = 'Standard'
bpy.ops.render.render(write_still=True)
print("[REF] rendered", scene.render.filepath)

# ── export the displaced mesh (evaluated) ─────────────────────────────────
if A.export:
    dg = bpy.context.evaluated_depsgraph_get(); ev = ob.evaluated_get(dg)
    m2 = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=dg)
    # back to FDS coordinates for the diff
    for v in m2.vertices:
        x, y, z = v.co; v.co = Vector((x, z, y))
    o2 = bpy.data.objects.new("displaced", m2); scene.collection.objects.link(o2)
    bpy.ops.object.select_all(action='DESELECT'); o2.select_set(True); bpy.context.view_layer.objects.active = o2
    bpy.ops.wm.obj_export(filepath=A.export, export_selected_objects=True, export_uv=False, export_normals=False,
                          export_materials=False, forward_axis='Y', up_axis='Z', apply_modifiers=False)
    print("[REF] exported %d verts / %d faces -> %s" % (len(m2.vertices), len(m2.polygons), A.export))
