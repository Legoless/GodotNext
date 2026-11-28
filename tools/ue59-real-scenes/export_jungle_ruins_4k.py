"""Export a true Render_Cover.png-quality Jungle Ruins glTF 2.0 for 4K UE 5.9 & GodotNext comparison.
Reads exact artist-authored PointInstancer transforms from USD/elements/*/PI_*.usd via pxr,
maps every prototype 1:1 to textured meshes in JungleRuins_Main.blend, cleans terrain Principled BSDF
materials so both UE Interchange and GodotNext render identical mossy jungle ground, merges dense
small scatter layers (Pyramid_Moss, Pyramid_Grass_B, Grass_A, Grass_B, Shrub, RiverSeedling) into
compact Nanite/NextGeo meshes, and instances full high-poly RiverForest/QueenForest/RiverSapling trees."""
import glob
import math
import os
import random
import bpy
import bmesh
import mathutils
from pxr import Usd, UsdGeom


def _tools_dir():
    """The gitignored data dir: GODOTNEXT_TOOLS_DIR, else <repo>/local/godotnext-tools."""
    tools_dir = os.environ.get("GODOTNEXT_TOOLS_DIR")
    if tools_dir:
        return tools_dir
    root = os.environ.get("GODOTNEXT_ROOT")
    if not root:
        try:
            root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
        except NameError:
            raise RuntimeError("Set GODOTNEXT_TOOLS_DIR or GODOTNEXT_ROOT: this interpreter did not set __file__.")
    return os.path.join(root, "local", "godotnext-tools")


TOOLS_DIR = _tools_dir()

JUNGLE_RUINS = os.path.join(TOOLS_DIR, "ue59-real-scenes", "intel_jungle_ruins", "extracted", "JungleRuins")
USD_ROOT = os.path.join(JUNGLE_RUINS, "USD", "elements")
TEX_TERRAIN = os.path.join(JUNGLE_RUINS, "textures", "Terrain")
OUT_DIR = os.path.join(TOOLS_DIR, "ue59-real-scenes", "jungle_ruins_4k")
os.makedirs(OUT_DIR, exist_ok=True)

# Pyramid center in Blender meters (matches USD 54370, 80977 cm -> 543.7, 809.8 m)
ORIGIN = mathutils.Vector((543.7, 809.8, 0.0))
# Official Render_Cover camera relative to ORIGIN: (28.8, -43.3, 1.385)
CAM_REL = mathutils.Vector((28.8, -43.3, 1.385))

# 1. Clean up materials so glTF 2.0 exports clean PBR + Alpha MASK for foliage + clean Terrain textures
for mat in bpy.data.materials:
    if not mat.use_nodes or not mat.node_tree:
        continue
    bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        continue
    # Fix Terrain materials: replace complex mix graph with direct BaseColor + Normal Principled BSDF
    if mat.name.startswith("MI_Terrain_X"):
        # Parse tile coords e.g. MI_Terrain_X4_Y3 -> 4_3
        parts = mat.name.replace("MI_Terrain_X", "").split("_Y")
        if len(parts) == 2:
            tx, ty = parts[0], parts[1]
            bc_path = os.path.join(TEX_TERRAIN, f"Terrain_Cinematic_BaseColor_{tx}_{ty}.jpg")
            nrm_path = os.path.join(TEX_TERRAIN, f"Terrain_Cinematic_Normal_{tx}_{ty}.jpg")
            if os.path.isfile(bc_path):
                nt = mat.node_tree
                nt.nodes.clear()
                out_n = nt.nodes.new("ShaderNodeOutputMaterial")
                p_n = nt.nodes.new("ShaderNodeBsdfPrincipled")
                p_n.inputs["Roughness"].default_value = 0.92
                p_n.inputs["Specular IOR Level"].default_value = 0.25
                bc_n = nt.nodes.new("ShaderNodeTexImage")
                bc_n.image = bpy.data.images.load(bc_path, check_existing=True)
                bc_n.image.colorspace_settings.name = "sRGB"
                nt.links.new(bc_n.outputs["Color"], p_n.inputs["Base Color"])
                if os.path.isfile(nrm_path):
                    nrm_tex = nt.nodes.new("ShaderNodeTexImage")
                    nrm_tex.image = bpy.data.images.load(nrm_path, check_existing=True)
                    nrm_tex.image.colorspace_settings.name = "Non-Color"
                    nrm_map = nt.nodes.new("ShaderNodeNormalMap")
                    nrm_map.inputs["Strength"].default_value = 0.85
                    nt.links.new(nrm_tex.outputs["Color"], nrm_map.inputs["Color"])
                    nt.links.new(nrm_map.outputs["Normal"], p_n.inputs["Normal"])
                nt.links.new(p_n.outputs["BSDF"], out_n.inputs["Surface"])
                print(f"  Rebuilt clean PBR terrain material: {mat.name} -> {os.path.basename(bc_path)}", flush=True)
        continue

    alpha_in = bsdf.inputs.get("Alpha")
    if alpha_in and alpha_in.is_linked:
        mat.blend_method = "CLIP"
        mat.alpha_threshold = 0.42
        mat.use_backface_culling = False
    for inp_name in ("Subsurface Weight", "Transmission Weight"):
        inp = bsdf.inputs.get(inp_name)
        if inp and not inp.is_linked and inp.default_value > 0.0:
            inp.default_value = 0.0

# Map prototype name -> Blender Mesh object
proto_objs = {}
for o in bpy.data.objects:
    if o.type == "MESH" and o.data and len(o.data.polygons) > 0:
        proto_objs[o.name] = o

def resolve_proto(pname: str):
    if pname in proto_objs:
        return proto_objs[pname]
    pl = pname.lower()
    for k, v in proto_objs.items():
        if k.lower() == pl or k.lower().startswith(pl):
            return v
    return None

# Helper: read USD PointInstancer transforms in Blender meters relative to ORIGIN
def read_usd_instances(subdir: str, filter_fn, max_keep: int, rng_seed: int):
    paths = sorted(glob.glob(os.path.join(USD_ROOT, subdir, "PI_*.usd")))
    items = []
    for p in paths:
        st = Usd.Stage.Open(p)
        for prim in st.Traverse():
            if not prim.IsA(UsdGeom.PointInstancer):
                continue
            pi = UsdGeom.PointInstancer(prim)
            xf = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
            pos_arr = pi.GetPositionsAttr().Get()
            if not pos_arr:
                continue
            ori_arr = pi.GetOrientationsAttr().Get()
            scl_arr = pi.GetScalesAttr().Get()
            idx_arr = pi.GetProtoIndicesAttr().Get()
            protos = [str(t).rsplit("/", 1)[-1] for t in pi.GetPrototypesRel().GetTargets()]
            if not protos:
                continue
            n = len(pos_arr)
            for i in range(n):
                wp_cm = xf.Transform(pos_arr[i])
                rel = mathutils.Vector((wp_cm[0] * 0.01 - ORIGIN.x, wp_cm[1] * 0.01 - ORIGIN.y, wp_cm[2] * 0.01 - ORIGIN.z))
                if not filter_fn(rel):
                    continue
                pidx = int(idx_arr[i]) if idx_arr is not None and i < len(idx_arr) else 0
                pname = protos[pidx % len(protos)]
                if ori_arr is not None and i < len(ori_arr):
                    q = ori_arr[i]
                    im = q.GetImaginary()
                    quat = mathutils.Quaternion((q.GetReal(), im[0], im[1], im[2]))
                else:
                    quat = mathutils.Quaternion((1.0, 0.0, 0.0, 0.0))
                if scl_arr is not None and i < len(scl_arr):
                    s = scl_arr[i]
                    # USD stage is metersPerUnit=0.01 (cm), while Blender prototypes are in meters -> scale * 0.01!
                    scl = mathutils.Vector((float(s[0]) * 0.01, float(s[1]) * 0.01, float(s[2]) * 0.01))
                else:
                    scl = mathutils.Vector((1.0, 1.0, 1.0))
                items.append((pname, rel, quat, scl))
    rng = random.Random(rng_seed)
    if len(items) > max_keep:
        rng.shuffle(items)
        items = items[:max_keep]
    print(f"  USD {subdir:18s}: kept {len(items):5d} instances", flush=True)
    return items

# Build clean export scene AND make it active immediately so matrix_world updates cleanly!
export_scene = bpy.data.scenes.new("JungleRuins4KExport")
export_coll = export_scene.collection
dg = bpy.context.evaluated_depsgraph_get()

def copy_mesh_cropped(obj, max_radius_xy=None, triangulate=False, smooth_subdiv=0):
    eo = obj.evaluated_get(dg)
    m = eo.data.copy()
    mw = eo.matrix_world.copy()
    if max_radius_xy is not None or triangulate or smooth_subdiv > 0:
        bm = bmesh.new()
        bm.from_mesh(m)
        if max_radius_xy is not None:
            to_del = []
            for f in bm.faces:
                c = (mw @ f.calc_center_median()) - ORIGIN
                if math.hypot(c.x, c.y) > max_radius_xy:
                    to_del.append(f)
            if to_del:
                bmesh.ops.delete(bm, geom=to_del, context="FACES")
        if smooth_subdiv > 0 and len(bm.edges) > 0:
            bmesh.ops.subdivide_edges(bm, edges=list(bm.edges), cuts=smooth_subdiv, use_grid_fill=True, smooth=0.5)
        if triangulate and len(bm.faces) > 0:
            bmesh.ops.triangulate(bm, faces=list(bm.faces))
        bm.to_mesh(m)
        bm.free()
    if smooth_subdiv > 0:
        for p in m.polygons:
            p.use_smooth = True
    return m, mw

# 2. Hero static meshes (Pyramid un-triangulated so VG stays solid + smooth-subdivided Terrain tiles)
hero_specs = [
    ("Pyramid_EmitterShell", None, False, 0),
    ("T_M_4x3_01", 130.0, False, 2),
    ("T_M_4x3_03", 130.0, False, 2),
    ("T_M_3x3_02", 130.0, False, 2),
    ("T_M_3x3_04", 130.0, False, 2),
]
for name, crop_r, do_tri, subdiv in hero_specs:
    o = bpy.data.objects.get(name)
    if o is None or o.type != "MESH":
        continue
    m, mw = copy_mesh_cropped(o, crop_r, triangulate=do_tri, smooth_subdiv=subdiv)
    if len(m.polygons) == 0:
        continue
    mw.translation -= ORIGIN
    no = bpy.data.objects.new(f"Hero_{name}", m)
    no.matrix_world = mw
    export_coll.objects.link(no)
    print(f"  Static {name}: {len(m.polygons):,} polys", flush=True)

# 3. Instanced large canopy trees (shared glTF mesh data -> fast import, huge Nanite/NextGeo detail!)
tree_mesh_cache = {}
def add_tree_instances(label, items, scale_mult=1.0):
    added = 0
    for idx, (pname, rel, quat, scl) in enumerate(items):
        po = resolve_proto(pname)
        if po is None:
            continue
        if po.name not in tree_mesh_cache:
            mc = po.data.copy()
            mc.name = f"TreeProto_{po.name[:28]}"
            tree_mesh_cache[po.name] = mc
        mdata = tree_mesh_cache[po.name]
        o = bpy.data.objects.new(f"{label}_{added:04d}", mdata)
        export_coll.objects.link(o)
        o.rotation_mode = "QUATERNION"
        o.location = rel
        o.rotation_quaternion = quat
        o.scale = scl * scale_mult
        o.matrix_world = mathutils.Matrix.Translation(rel) @ quat.to_matrix().to_4x4() @ mathutils.Matrix.Diagonal((scl.x * scale_mult, scl.y * scale_mult, scl.z * scale_mult, 1.0))
        added += 1
    print(f"  Added {added} instanced trees for {label}", flush=True)

# Keep trees clear of the immediate 2.8m camera lens bubble, but allow framing trees on left/right!
def clear_of_cams(p, min_cam_d=3.0):
    if (p - CAM_REL).length < min_cam_d:
        return False
    return True

# Queen palms (dense skyline palms behind & around pyramid up to 115m)
queen_items = read_usd_instances("QueenForest", lambda p: math.hypot(p.x, p.y) < 115.0 and clear_of_cams(p, 5.0), 170, 101)
add_tree_instances("QueenPalm", queen_items, scale_mult=1.15)

# RiverForest canopy trees: dense tropical jungle wall carpeting the hillsides & framing left/right!
def river_forest_filter(p):
    r = math.hypot(p.x, p.y)
    if r > 95.0 or r < 18.0:
        return False
    if not clear_of_cams(p, 4.5):
        return False
    ax, ay = CAM_REL.x, CAM_REL.y
    bx, by = 0.0, 0.0
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((p.x - ax) * dx + (p.y - ay) * dy) / (dx * dx + dy * dy)))
    px, py = ax + t * dx, ay + t * dy
    dist_axis = math.hypot(p.x - px, p.y - py)
    if 0.05 < t < 0.85 and dist_axis < 12.5:
        return False
    return True

river_items = read_usd_instances("RiverForest", river_forest_filter, 420, 102)
add_tree_instances("RiverTree", river_items, scale_mult=1.35)

# RiverSapling mid-story trees
sapling_items = read_usd_instances("RiverSapling", river_forest_filter, 240, 103)
add_tree_instances("RiverSapling", sapling_items, scale_mult=1.25)

# 4. Merge dense small foliage layers (Pyramid_Moss, Pyramid_Grass_B, Shrub, Grass_A, Grass_B, RiverSeedling)
# into compact single-material meshes so both Nanite & NextGeo get rich dense foliage carpets!
def build_merged_layer(name, items, scale_mult=1.0, max_polys=1800000):
    bm = bmesh.new()
    mat_list = []
    mat_idx_map = {}
    proto_bm_cache = {}
    used = 0
    for pname, rel, quat, scl in items:
        po = resolve_proto(pname)
        if po is None:
            continue
        if len(bm.faces) >= max_polys:
            break
        if po.name not in proto_bm_cache:
            # Extract vertices, faces, UVs, and material indices once
            mesh = po.data
            verts = [v.co.copy() for v in mesh.vertices]
            uv_layer = mesh.uv_layers.active.data if mesh.uv_layers.active else None
            polys = []
            for poly in mesh.polygons:
                m = mesh.materials[poly.material_index] if poly.material_index < len(mesh.materials) else None
                mname = m.name if m else ""
                if mname not in mat_idx_map:
                    mat_idx_map[mname] = len(mat_list)
                    mat_list.append(m)
                gmi = mat_idx_map[mname]
                uvs = [uv_layer[li].uv.copy() for li in poly.loop_indices] if uv_layer else None
                polys.append((list(poly.vertices), gmi, uvs))
            proto_bm_cache[po.name] = (verts, polys)

        verts, polys = proto_bm_cache[po.name]
        mat4 = mathutils.Matrix.Translation(rel) @ quat.to_matrix().to_4x4() @ mathutils.Matrix.Diagonal((scl.x * scale_mult, scl.y * scale_mult, scl.z * scale_mult, 1.0))
        uv_bm = bm.loops.layers.uv.verify()
        new_vs = [bm.verts.new(mat4 @ v) for v in verts]
        for v_ids, gmi, uvs in polys:
            try:
                f = bm.faces.new([new_vs[vi] for vi in v_ids])
                f.material_index = gmi
                if uvs:
                    for loop, uv in zip(f.loops, uvs):
                        loop[uv_bm].uv = uv
            except ValueError:
                pass
        used += 1

    if len(bm.faces) == 0:
        bm.free()
        return
    out_m = bpy.data.meshes.new(name)
    bm.to_mesh(out_m)
    bm.free()
    for m in mat_list:
        out_m.materials.append(m)
    o = bpy.data.objects.new(name, out_m)
    export_coll.objects.link(o)
    print(f"  Merged {name:24s}: {used:5d} clumps -> {len(out_m.polygons):9,d} polys", flush=True)

# Pyramid Moss: lush thick carpet covering the stepped pyramid tiers
moss_items = read_usd_instances("Pyramid_Moss", lambda p: math.hypot(p.x, p.y) < 30.0, 4400, 201)
build_merged_layer("Layer_Pyramid_Moss", moss_items, scale_mult=2.1, max_polys=1000000)

# Instance ALL remaining foliage layers with true 0.01*USD scale & unobstructed camera bubble!
pgrass_items = read_usd_instances("Pyramid_Grass_B", lambda p: math.hypot(p.x, p.y) < 30.0, 1100, 202)
add_tree_instances("PyramidGrass", pgrass_items, scale_mult=1.35)

# Foreground & clearing ground cover (focused around pyramid & camera clearing!)
def ground_filter(p):
    if not clear_of_cams(p, 3.8):
        return False
    return math.hypot(p.x, p.y) < 58.0 or math.hypot(p.x - CAM_REL.x, p.y - CAM_REL.y) < 36.0

shrub_items = read_usd_instances("Shrub", ground_filter, 900, 203)
add_tree_instances("JungleShrub", shrub_items, scale_mult=1.4)

anthurium_items = read_usd_instances("Anthurium", ground_filter, 300, 207)
add_tree_instances("JungleAnthurium", anthurium_items, scale_mult=1.4)

nettle_items = read_usd_instances("Nettle", ground_filter, 300, 208)
add_tree_instances("JungleNettle", nettle_items, scale_mult=1.4)

sorrel_items = read_usd_instances("ShrubSorrel", ground_filter, 350, 209)
add_tree_instances("JungleSorrel", sorrel_items, scale_mult=1.4)

grass_a_items = read_usd_instances("Grass_A", ground_filter, 900, 204)
add_tree_instances("JungleGrassA", grass_a_items, scale_mult=1.45)

grass_b_items = read_usd_instances("Grass_B", ground_filter, 1400, 205)
add_tree_instances("JungleGrassB", grass_b_items, scale_mult=1.45)

seedling_items = read_usd_instances("RiverSeedling", ground_filter, 500, 206)
add_tree_instances("JungleSeedling", seedling_items, scale_mult=1.25)

# Switch active scene, force depsgraph/view_layer update so ALL instanced node transforms export!
bpy.context.window.scene = export_scene
bpy.context.view_layer.update()
out_gltf = os.path.join(OUT_DIR, "jungle_ruins.gltf")
bpy.ops.export_scene.gltf(
    filepath=out_gltf,
    export_format="GLTF_SEPARATE",
    export_texture_dir="textures",
    use_active_scene=True,
    export_materials="EXPORT",
    export_image_format="AUTO",
    export_texcoords=True,
    export_normals=True,
    export_tangents=False,
    export_yup=True,
    export_apply=False,
)
# Patch ALL foliage BLEND materials -> MASK AND strip KHR_materials_transmission/ior so UE5 doesn't turn leaves into invisible glass!
import json as _json
_d = _json.load(open(out_gltf))
_patched = 0
_stripped_trans = 0
for _m in _d.get("materials", []):
    if _m.get("alphaMode") == "BLEND":
        _m["alphaMode"] = "MASK"
        _m["alphaCutoff"] = 0.42
        _m["doubleSided"] = True
        _patched += 1
    _exts = _m.get("extensions")
    if isinstance(_exts, dict):
        for _k in ("KHR_materials_transmission", "KHR_materials_ior"):
            if _k in _exts:
                del _exts[_k]
                _stripped_trans += 1
        if not _exts:
            del _m["extensions"]
if "extensionsUsed" in _d:
    _d["extensionsUsed"] = [e for e in _d["extensionsUsed"] if e not in ("KHR_materials_transmission", "KHR_materials_ior")]
_json.dump(_d, open(out_gltf, "w"))
_rt = next((n for n in _d.get("nodes", []) if n.get("name") == "RiverTree_0000"), None)
_mm = next((m for m in _d.get("meshes", []) if "Moss" in m.get("name", "")), None)
_macc = _d["accessors"][_mm["primitives"][0]["attributes"]["POSITION"]] if _mm else {}
print(f"DONE EXPORT -> {out_gltf} (objects={len(export_coll.objects)}, masked_mats={_patched}, stripped_trans={_stripped_trans})", flush=True)
print(f"  VERIFY RiverTree_0000={_rt}", flush=True)
print(f"  VERIFY Layer_Pyramid_Moss min={_macc.get('min')} max={_macc.get('max')}", flush=True)
