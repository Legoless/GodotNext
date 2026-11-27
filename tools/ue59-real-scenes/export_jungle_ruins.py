"""Export a clean, focused, origin-centered hero glTF 2.0 of Intel Jungle Ruins
(Mayan stepped Pyramid + Banyan canopy + local jungle terrain + scattered moss,
grass, shrubs, and tropical palm/river trees from Sources) for UE 5.9 and GodotNext."""
import json
import math
import os
import random
import bpy
import mathutils

OUT_DIR = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/jungle_ruins_hero"
os.makedirs(OUT_DIR, exist_ok=True)

ORIGIN = mathutils.Vector((543.7, 809.8, 0.0))
CAM_REL = mathutils.Vector((572.5, 766.5, 1.38)) - ORIGIN  # ~(28.8, -43.3, 1.38)

# 1. Clean up materials first so Blender glTF exports clean PBR + Alpha MASK for foliage
for mat in bpy.data.materials:
    if not mat.use_nodes or not mat.node_tree:
        continue
    bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        continue
    alpha_in = bsdf.inputs.get("Alpha")
    if alpha_in and alpha_in.is_linked:
        mat.blend_method = "CLIP"
        mat.alpha_threshold = 0.45
        mat.use_backface_culling = False
    for inp_name in ("Subsurface Weight", "Transmission Weight"):
        inp = bsdf.inputs.get(inp_name)
        if inp and not inp.is_linked and inp.default_value > 0.0:
            inp.default_value = 0.0

# 2. Build clean export scene containing ONLY hero pyramid + banyan + cropped terrain + foliage
old_scenes = list(bpy.data.scenes)
export_scene = bpy.data.scenes.new("JungleRuinsHeroExport")
export_coll = export_scene.collection
dg = bpy.context.evaluated_depsgraph_get()

total_polys = 0
mesh_cache = {}

def copy_mesh_cropped(obj, max_radius_xy=None):
    eo = obj.evaluated_get(dg)
    m = eo.data.copy()
    mw = eo.matrix_world.copy()
    if max_radius_xy is not None:
        # Remove polygons completely outside max_radius_xy from ORIGIN so terrain stays tight & high-precision
        import bmesh
        bm = bmesh.new()
        bm.from_mesh(m)
        to_del = []
        for f in bm.faces:
            c = (mw @ f.calc_center_median()) - ORIGIN
            if math.hypot(c.x, c.y) > max_radius_xy:
                to_del.append(f)
        if to_del:
            bmesh.ops.delete(bm, geom=to_del, context="FACES")
            bm.to_mesh(m)
        bm.free()
    return m, mw

# Hero static meshes
for name, crop_r in [
    ("Pyramid_EmitterShell", None),
    ("T_M_4x3_01", 105.0),
    ("T_M_4x3_03", 105.0),
    ("T_M_3x3_02", 105.0),
    ("T_M_3x3_04", 105.0),
    ("BanyanForest_01_Translucent", None),
]:
    o = bpy.data.objects.get(name)
    if o is None or o.type != "MESH":
        continue
    m, mw = copy_mesh_cropped(o, crop_r)
    if len(m.polygons) == 0:
        continue
    mw.translation -= ORIGIN
    no = bpy.data.objects.new(f"Hero_{name}", m)
    no.matrix_world = mw
    export_coll.objects.link(no)
    total_polys += len(m.polygons)
    print(f"  Static {name}: {len(m.polygons):,} polys", flush=True)

# 3. Collect real foliage prototype meshes from the blend's Sources/Demo Library collections
def find_proto_meshes(substrs, max_polys=180000):
    found = []
    for o in bpy.data.objects:
        if o.type != "MESH" or o.data is None or len(o.data.polygons) < 10:
            continue
        if len(o.data.polygons) > max_polys:
            continue
        colls = " ".join(c.name for c in o.users_collection)
        key = (o.name + " " + colls).lower()
        if any(s in key for s in substrs):
            if o.data.name not in mesh_cache:
                mc = o.data.copy()
                mc.name = f"Proto_{o.data.name[:24]}"
                mesh_cache[o.data.name] = mc
            found.append(mesh_cache[o.data.name])
    # deduplicate
    seen = set()
    uniq = []
    for m in found:
        if m.name not in seen:
            seen.add(m.name)
            uniq.append(m)
    return uniq

moss_protos = find_proto_meshes(["moss_01"], 15000)
grass_protos = find_proto_meshes(["grass_01", "grass_02", "anthurium", "shrub_04", "sorrel", "nettle"], 25000)
tree_protos = find_proto_meshes(["queenforest", "riverforest", "riversapling"], 250000)
print(f"  Protos found: moss={len(moss_protos)} grass/shrub={len(grass_protos)} trees={len(tree_protos)}", flush=True)

# Helper: sample upward-facing surface points on Pyramid and local Terrain for realistic jungle overgrowth!
rng = random.Random(20261126)
pyr_obj = export_coll.objects.get("Hero_Pyramid_EmitterShell")
terr_objs = [o for o in export_coll.objects if o.name.startswith("Hero_T_M_")]

def sample_faces(obj, min_nz=0.55, max_count=400, filter_fn=None):
    m = obj.data
    mw = obj.matrix_world
    rot_m = mw.to_3x3()
    pts = []
    stride = max(1, len(m.polygons) // (max_count * 12))
    for i in range(0, len(m.polygons), stride):
        p = m.polygons[i]
        wn = (rot_m @ p.normal).normalized()
        if wn.z < min_nz:
            continue
        wc = mw @ p.center
        if filter_fn and not filter_fn(wc):
            continue
        pts.append(wc)
    rng.shuffle(pts)
    return pts[:max_count]

added_inst = 0
def place_inst(prefix, mesh_data, loc, scale_val, yaw):
    global added_inst, total_polys
    o = bpy.data.objects.new(f"{prefix}_{added_inst:04d}", mesh_data)
    o.location = loc
    o.rotation_euler = (0.0, 0.0, yaw)
    o.scale = (scale_val, scale_val, scale_val)
    export_coll.objects.link(o)
    added_inst += 1
    total_polys += len(mesh_data.polygons)

# Scatter moss & tropical shrubs onto Pyramid stepped ledges (leaving stair face & front stonework visible!)
if pyr_obj and (moss_protos or grass_protos):
    pyr_pts = sample_faces(pyr_obj, min_nz=0.65, max_count=320, filter_fn=lambda p: p.z > -1.0)
    pool = moss_protos + grass_protos
    for pt in pyr_pts:
        pm = rng.choice(pool)
        place_inst("PyrFol", pm, pt, rng.uniform(0.9, 1.8), rng.uniform(0, 6.283))

# Scatter lush jungle ground cover (grass, ferns, shrubs) across clearing in front of pyramid
terr_ground_pts = []
for to in terr_objs:
    terr_ground_pts.extend(sample_faces(to, min_nz=0.70, max_count=220, filter_fn=lambda p: math.hypot(p.x, p.y) < 72.0 and not (abs(p.x) < 20.0 and abs(p.y) < 20.0)))
rng.shuffle(terr_ground_pts)
if grass_protos:
    for pt in terr_ground_pts[:360]:
        pm = rng.choice(grass_protos)
        place_inst("GrndFol", pm, pt, rng.uniform(1.1, 2.4), rng.uniform(0, 6.283))

# Place tropical jungle canopy trees (Queen palms + River Forest trees) framing the pyramid behind & on sides!
if tree_protos:
    tree_pts = []
    for to in terr_objs:
        tree_pts.extend(sample_faces(to, min_nz=0.65, max_count=120, filter_fn=lambda p: 24.0 < math.hypot(p.x, p.y) < 82.0))
    rng.shuffle(tree_pts)
    placed_trees = []
    for pt in tree_pts:
        # Keep clear sightline cone from camera (28.8, -43.3) to pyramid (0, 0)!
        v_cam = pt - CAM_REL
        dist_cam = math.hypot(v_cam.x, v_cam.y)
        if dist_cam < 8.5:
            continue
        # Line distance to camera->pyramid axis
        seg = mathutils.Vector((-CAM_REL.x, -CAM_REL.y, 0.0))
        seg_len = seg.length
        t = max(0.0, min(1.0, (v_cam.x * seg.x + v_cam.y * seg.y) / (seg_len * seg_len)))
        closest = mathutils.Vector((CAM_REL.x + seg.x * t, CAM_REL.y + seg.y * t, 0.0))
        perp = math.hypot(pt.x - closest.x, pt.y - closest.y)
        if t > 0.05 and t < 0.95 and perp < 14.0:
            continue
        if any(math.hypot(pt.x - q.x, pt.y - q.y) < 7.5 for q in placed_trees):
            continue
        placed_trees.append(pt)
        tm = rng.choice(tree_protos)
        place_inst("Tree", tm, pt, rng.uniform(0.85, 1.35), rng.uniform(0, 6.283))
        if len(placed_trees) >= 38 or total_polys > 7_500_000:
            break
    print(f"  Placed {len(placed_trees)} framing jungle trees", flush=True)

print(f"  Final hero scene: {len(export_coll.objects)} objects, total_polys={total_polys:,}", flush=True)

# 4. Remove old scenes so ONLY export_scene gets exported!
for sc in old_scenes:
    bpy.data.scenes.remove(sc)

# Remove old huge bin/gltf before re-exporting
for old_f in ("jungle_ruins.gltf", "jungle_ruins.bin"):
    p = os.path.join(OUT_DIR, old_f)
    if os.path.exists(p):
        os.remove(p)

out_gltf = os.path.join(OUT_DIR, "jungle_ruins.gltf")
print(f"Exporting focused glTF 2.0 to {out_gltf}...", flush=True)
bpy.ops.export_scene.gltf(
    filepath=out_gltf,
    export_format="GLTF_SEPARATE",
    export_texture_dir="textures",
    export_image_format="AUTO",
    export_texcoords=True,
    export_normals=True,
    export_tangents=False,
    export_materials="EXPORT",
    export_yup=True,
    export_apply=False,
    use_active_scene=True,
    export_cameras=False,
    export_lights=False,
    export_animations=False,
)

meta = {
    "scene": "jungle_ruins",
    "gltf": out_gltf,
    "origin_blender": [ORIGIN.x, ORIGIN.y, ORIGIN.z],
    "orig_cam_godot": [CAM_REL.x, CAM_REL.z, -CAM_REL.y],
    "objects": len(export_coll.objects),
    "added_instances": added_inst,
    "total_polys": total_polys,
}
with open(os.path.join(OUT_DIR, "meta.json"), "w") as f:
    json.dump(meta, f, indent=2)
print("HERO EXPORT COMPLETE:", json.dumps(meta), flush=True)
