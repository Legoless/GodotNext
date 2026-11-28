#!/usr/bin/env python3
"""Assemble the 4K Megascans-grade CC0 Photogrammetry 'Red Desert Campfire Overhang' scene
in Blender 4.2.3 LTS and export to glTF 2.0 (shared instanced photogrammetry meshes + 4K PBR).
Matches the iconic Valley of the Ancient Red Desert overhang + campfire + Echo composition."""
import math
import os
import random
from pathlib import Path
import numpy as np
import bpy
import bmesh
from mathutils import Vector, Euler


def _tools_dir() -> Path:
    """The gitignored data dir: GODOTNEXT_TOOLS_DIR, else <repo>/local/godotnext-tools."""
    if os.environ.get("GODOTNEXT_TOOLS_DIR"):
        return Path(os.environ["GODOTNEXT_TOOLS_DIR"])
    root = os.environ.get("GODOTNEXT_ROOT")
    if not root:
        try:
            root = Path(__file__).resolve().parents[2]
        except NameError:
            raise RuntimeError("Set GODOTNEXT_TOOLS_DIR or GODOTNEXT_ROOT: this interpreter did not set __file__.")
    return Path(root) / "local/godotnext-tools"


TOOLS_DIR = _tools_dir()

SRC = TOOLS_DIR / "ue59-real-scenes/photogrammetry_sources"
MODELS = SRC / "models"
TEX = SRC / "textures"
HERO_GLB = SRC / "hero_character/hero_campfire.glb"
OUT_DIR = TOOLS_DIR / "ue59-real-scenes/red_desert_campfire_4k"
TINT_DIR = OUT_DIR / "_tinted_tex"
OUT_DIR.mkdir(parents=True, exist_ok=True)
TINT_DIR.mkdir(parents=True, exist_ok=True)


def g2b(gx, gy, gz):
    """Map Godot/glTF world coordinates (X right, Y up, -Z forward) to Blender (X right, Y back, Z up)."""
    return Vector((float(gx), float(-gz), float(gy)))


def g_rot_to_b(rx_deg=0.0, ry_deg=0.0, rz_deg=0.0):
    """Euler angles in Blender space (degrees)."""
    return Euler((math.radians(rx_deg), math.radians(ry_deg), math.radians(rz_deg)), "XYZ")


def tint_image_cached(src_path: Path, tag: str, mul_rgb=(1.0, 1.0, 1.0), gamma=1.0, desat=0.0) -> Path:
    dst = TINT_DIR / f"{src_path.stem}_{tag}.jpg"
    if dst.is_file() and dst.stat().st_size > 1000:
        return dst
    im = bpy.data.images.load(str(src_path))
    w, h = im.size[0], im.size[1]
    arr = np.empty(w * h * 4, dtype=np.float32)
    im.pixels.foreach_get(arr)
    arr = arr.reshape((h, w, 4))
    rgb = arr[..., :3]
    if desat > 0.0:
        lum = 0.299 * rgb[..., 0:1] + 0.587 * rgb[..., 1:2] + 0.114 * rgb[..., 2:3]
        rgb = rgb * (1.0 - desat) + lum * desat
    if abs(gamma - 1.0) > 1e-3:
        rgb = np.power(np.clip(rgb, 0.0, 1.0), gamma)
    rgb[..., 0] = np.clip(rgb[..., 0] * mul_rgb[0], 0.0, 1.0)
    rgb[..., 1] = np.clip(rgb[..., 1] * mul_rgb[1], 0.0, 1.0)
    rgb[..., 2] = np.clip(rgb[..., 2] * mul_rgb[2], 0.0, 1.0)
    arr[..., :3] = rgb
    im.pixels.foreach_set(arr.ravel())
    im.filepath_raw = str(dst)
    im.file_format = "JPEG"
    im.save()
    bpy.data.images.remove(im)
    return dst


def load_img(path: Path, non_color=False):
    key = str(path)
    for im in bpy.data.images:
        if im.filepath == key:
            return im
    im = bpy.data.images.load(key)
    if non_color:
        im.colorspace_settings.name = "Non-Color"
    return im


def import_model_meshes(model_name: str, center_submeshes=False, tint_tag=None, tint_rgb=None, tint_gamma=1.0, desat=0.0):
    """Import a Poly Haven 4K glTF model and return list of template Mesh Objects (unlinked from scene)."""
    gltf = next((MODELS / model_name).glob("*.gltf"))
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(gltf))
    added = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in added if o.type == "MESH"]

    # Apply transforms cleanly and optionally re-center each sub-mesh at its own bottom-center
    templates = []
    seen_mats = set()
    for o in meshes:
        mat_w = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = Matrix_id()
        o.data.transform(mat_w)
        if center_submeshes:
            vs = [v.co for v in o.data.vertices]
            cx = 0.5 * (min(p.x for p in vs) + max(p.x for p in vs))
            cy = 0.5 * (min(p.y for p in vs) + max(p.y for p in vs))
            z0 = min(p.z for p in vs)
            shift = Vector((-cx, -cy, -z0))
            for v in o.data.vertices:
                v.co += shift
            o.data.update()
        # Optional exact baked diffuse tint (once per unique material!)
        if tint_tag and tint_rgb:
            for slot in o.data.materials:
                if slot is None or not slot.use_nodes or slot.name in seen_mats:
                    continue
                seen_mats.add(slot.name)
                for n in slot.node_tree.nodes:
                    if n.type == "TEX_IMAGE" and n.image is not None:
                        fp = Path(bpy.path.abspath(n.image.filepath))
                        if "diff" in fp.name.lower() and fp.is_file():
                            new_fp = tint_image_cached(fp, tint_tag, tint_rgb, tint_gamma, desat)
                            n.image = load_img(new_fp, non_color=False)
        for p in o.data.polygons:
            p.use_smooth = True
        # Remove template from scene collection (only instances will live in scene)
        for col in list(o.users_collection):
            col.objects.unlink(o)
        templates.append(o)

    for o in added:
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    return templates


def Matrix_id():
    from mathutils import Matrix
    return Matrix.Identity(4)


def place_inst(template_obj, name, g_pos, rot_deg=(0, 0, 0), scale=(1, 1, 1)):
    obj = bpy.data.objects.new(name, template_obj.data)
    obj.location = g2b(*g_pos)
    obj.rotation_euler = g_rot_to_b(*rot_deg)
    if isinstance(scale, (int, float)):
        obj.scale = (float(scale), float(scale), float(scale))
    else:
        obj.scale = (float(scale[0]), float(scale[1]), float(scale[2]))
    bpy.context.scene.collection.objects.link(obj)
    return obj


def terrain_height_at(gx, gz):
    """Smooth macro desert canyon floor elevation (in Godot Y meters), flat shelter basin around campfire (0, -5.8)."""
    # Keep campfire shelter zone gently level around y = 0.0
    d_camp = math.hypot(gx - 0.3, gz + 5.7)
    shelter_flat = min(1.0, max(0.0, (d_camp - 2.2) / 3.2))
    # Gentle natural desert floor wash rising slightly toward left boulders and back cliff
    h = 0.0
    h += 0.14 * math.sin(gx * 0.28 + 0.5) * math.cos(gz * 0.24 - 0.3) * shelter_flat
    h += 0.08 * math.sin(gx * 0.72 - gz * 0.61) * shelter_flat
    # Slight upward slope toward canyon walls at far sides & back
    if gz < -7.2:
        h += 0.38 * ((-7.2 - gz) ** 1.25)
    if gx < -3.5:
        h += 0.14 * ((-3.5 - gx) ** 1.15)
    if gx > 5.0:
        h += 0.16 * ((gx - 5.0) ** 1.15)
    # Slight natural dip right under foreground camera so floor sweeps down cleanly
    if gz > 0.5:
        h += 0.06 * (gz - 0.5)
    return h


def build_desert_terrain():
    """High-resolution height-displaced 4K PBR warm red-ochre sandy gravel desert canyon floor."""
    disp_path = TEX / "sandy_gravel_02/sandy_gravel_02_Displacement_4k.png"
    dim = bpy.data.images.load(str(disp_path))
    dim.scale(512, 512)
    darr = np.empty(512 * 512 * 4, dtype=np.float32)
    dim.pixels.foreach_get(darr)
    disp_arr = darr.reshape((512, 512, 4))[..., 0] - 0.5
    bpy.data.images.remove(dim)

    nx, nz = 180, 180
    x_min, x_max = -18.0, 18.0
    z_min, z_max = -18.0, 14.0
    uv_tile = 10.0

    bm = bmesh.new()
    grid = []
    for iz in range(nz + 1):
        vz = iz / float(nz)
        gz = z_min + vz * (z_max - z_min)
        row = []
        for ix in range(nx + 1):
            vx = ix / float(nx)
            gx = x_min + vx * (x_max - x_min)
            u = vx * uv_tile
            v = vz * uv_tile
            px = int((u % 1.0) * 511)
            py = int((v % 1.0) * 511)
            micro = float(disp_arr[py, px]) * 0.12
            d_camp = math.hypot(gx - 0.2, gz + 5.6)
            if d_camp < 1.8:
                micro *= 0.35
            gy = terrain_height_at(gx, gz) + micro
            row.append(bm.verts.new(g2b(gx, gy, gz)))
        grid.append(row)

    uv_layer = bm.loops.layers.uv.new("UVMap")
    for iz in range(nz):
        for ix in range(nx):
            f = bm.faces.new([grid[iz][ix], grid[iz][ix + 1], grid[iz + 1][ix + 1], grid[iz + 1][ix]])
            f.smooth = True
            u0, u1 = (ix / nx) * uv_tile, ((ix + 1) / nx) * uv_tile
            v0, v1 = (iz / nz) * uv_tile, ((iz + 1) / nz) * uv_tile
            for lp, uv in zip(f.loops, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]):
                lp[uv_layer].uv = uv

    me = bpy.data.meshes.new("DesertFloorMesh")
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new("DesertFloor", me)
    bpy.context.scene.collection.objects.link(obj)

    # Warm dusty red-sandstone gravel floor harmonizing seamlessly with Namaqualand boulders & cliffs!
    mat = bpy.data.materials.new("RedDesertFloorMat")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    diff_src = TEX / "sandy_gravel_02/sandy_gravel_02_Diffuse_4k.jpg"
    diff_tinted = tint_image_cached(diff_src, "red_canyon_gravel_v2", mul_rgb=(1.16, 0.80, 0.60), gamma=1.08)
    td = nt.nodes.new("ShaderNodeTexImage")
    td.image = load_img(diff_tinted, non_color=False)
    nt.links.new(td.outputs["Color"], bsdf.inputs["Base Color"])

    tr = nt.nodes.new("ShaderNodeTexImage")
    tr.image = load_img(TEX / "sandy_gravel_02/sandy_gravel_02_Rough_4k.png", non_color=True)
    nt.links.new(tr.outputs["Color"], bsdf.inputs["Roughness"])

    tn = nt.nodes.new("ShaderNodeTexImage")
    tn.image = load_img(TEX / "sandy_gravel_02/sandy_gravel_02_nor_gl_4k.png", non_color=True)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nm.inputs["Strength"].default_value = 1.25
    nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])

    obj.data.materials.append(mat)
    return obj


def make_emissive_mat(name, color_rgb, strength, base_rgb=(0.05, 0.02, 0.01), rough=0.8, double_sided=False):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.use_backface_culling = (not double_sided)
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (base_rgb[0], base_rgb[1], base_rgb[2], 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Emission Color"].default_value = (color_rgb[0], color_rgb[1], color_rgb[2], 1.0)
    bsdf.inputs["Emission Strength"].default_value = float(strength)
    return mat


def build_campfire(camp_gx, camp_gz, stones_tpl, branches_tpl):
    """Build a realistic crackling campfire: photogrammetry stones, charred quiver branches,
    incandescent ember bed, and sculpted layered HDR flame tongues."""
    cy = terrain_height_at(camp_gx, camp_gz)
    rng = random.Random(77)

    # 1. Ring of real photogrammetry stones around the fire pit
    for i in range(13):
        ang = (i / 13.0) * math.tau + rng.uniform(-0.12, 0.12)
        rad = rng.uniform(0.46, 0.58)
        sx = camp_gx + math.cos(ang) * rad
        sz = camp_gz + math.sin(ang) * rad
        sy = terrain_height_at(sx, sz) - 0.02
        tpl = stones_tpl[i % len(stones_tpl)]
        sc = rng.uniform(1.3, 2.2)
        place_inst(tpl, f"CampStone_{i:02d}", (sx, sy, sz), (rng.uniform(-12, 12), rng.uniform(-12, 12), rng.uniform(0, 360)), (sc, sc * rng.uniform(0.85, 1.2), sc * rng.uniform(0.8, 1.3)))

    # 2. Crossed real photogrammetry dead quiver branches as firewood logs
    for i in range(7):
        ang = (i / 7.0) * math.tau + rng.uniform(-0.18, 0.18)
        tpl = branches_tpl[i % len(branches_tpl)]
        lx = camp_gx + math.cos(ang) * 0.16
        lz = camp_gz + math.sin(ang) * 0.16
        place_inst(
            tpl, f"CampLog_{i:02d}",
            (lx, cy + 0.04 + (i % 2) * 0.04, lz),
            (rng.uniform(52, 74), rng.uniform(-18, 18), math.degrees(ang) + 90.0),
            (rng.uniform(1.1, 1.55), rng.uniform(1.1, 1.55), rng.uniform(1.25, 1.65)),
        )

    # 3. Charred dark ash mound & glowing incandescent embers + warm orange-gold campfire flames
    mat_ash = make_emissive_mat("Campfire_CharredAsh", (0.25, 0.04, 0.005), 0.10, base_rgb=(0.025, 0.020, 0.018), rough=0.96)
    mat_ember = make_emissive_mat("Campfire_HotEmbers", (1.0, 0.24, 0.015), 10.0, base_rgb=(0.10, 0.025, 0.01), rough=0.85)
    mat_flame_core = make_emissive_mat("Campfire_FlameCore", (1.0, 0.72, 0.16), 24.0, base_rgb=(1.0, 0.76, 0.20), rough=0.5, double_sided=True)
    mat_flame_mid = make_emissive_mat("Campfire_FlameMid", (1.0, 0.36, 0.03), 15.0, base_rgb=(1.0, 0.40, 0.04), rough=0.5, double_sided=True)
    mat_flame_outer = make_emissive_mat("Campfire_FlameOuter", (0.96, 0.16, 0.01), 9.0, base_rgb=(0.92, 0.18, 0.015), rough=0.5, double_sided=True)

    # Dark charred charcoal/ash bed mound
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=3, radius=0.34, location=g2b(camp_gx, cy + 0.01, camp_gz))
    ash = bpy.context.active_object
    ash.name = "Campfire_AshBed"
    ash.scale = (1.0, 0.95, 0.16)
    bpy.ops.object.transform_apply(scale=True)
    for v in ash.data.vertices:
        v.co.z += rng.uniform(-0.012, 0.018)
    ash.data.materials.append(mat_ash)

    # Glowing coal chunks scattered across the center
    for i in range(28):
        r = rng.uniform(0.0, 0.30)
        a = rng.uniform(0, math.tau)
        ex = camp_gx + math.cos(a) * r
        ez = camp_gz + math.sin(a) * r
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=rng.uniform(0.025, 0.055), location=g2b(ex, cy + rng.uniform(0.04, 0.10), ez))
        c = bpy.context.active_object
        c.name = f"CampEmber_{i:02d}"
        c.scale = (rng.uniform(0.8, 1.4), rng.uniform(0.7, 1.3), rng.uniform(0.5, 0.9))
        c.data.materials.append(mat_ember)

    # Sculpted organic flickering flame tongues (16 layered twisting ribbons/teardrops!)
    def make_flame_tongue(name, offset_gx, offset_gz, base_h, height, rad, bend_x, bend_z, twist, mat):
        bm = bmesh.new()
        rings = 10
        segs = 10
        verts_grid = []
        for ir in range(rings + 1):
            t = ir / float(rings)  # 0 = base, 1 = sharp tip
            # Teardrop flame profile widening near t=0.22 then tapering sharply to flickering tip
            r = rad * (math.sin(t * math.pi) ** 0.75) * (1.0 - 0.55 * t)
            if ir == rings:
                r = 0.002
            wiggle_x = bend_x * (t ** 1.5) + 0.028 * math.sin(t * 7.0 + twist) * t
            wiggle_z = bend_z * (t ** 1.5) + 0.022 * math.cos(t * 6.0 - twist) * t
            py = cy + base_h + t * height
            ring_v = []
            for iseg in range(segs):
                th = (iseg / float(segs)) * math.tau + twist * t
                # Flatten into ribbon-like flickering flame tongue
                lx = math.cos(th) * r * 1.15
                lz = math.sin(th) * r * 0.52
                p = g2b(camp_gx + offset_gx + wiggle_x + lx, py, camp_gz + offset_gz + wiggle_z + lz)
                ring_v.append(bm.verts.new(p))
            verts_grid.append(ring_v)
        for ir in range(rings):
            for iseg in range(segs):
                jn = (iseg + 1) % segs
                f = bm.faces.new([verts_grid[ir][iseg], verts_grid[ir + 1][iseg], verts_grid[ir + 1][jn], verts_grid[ir][jn]])
                f.smooth = True
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        o = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(o)
        o.data.materials.append(mat)
        return o

    # Inner bright white-gold flame tongues
    for i in range(4):
        a = (i / 4.0) * math.tau + 0.3
        make_flame_tongue(
            f"FlameCore_{i}", math.cos(a) * 0.04, math.sin(a) * 0.04,
            0.09, rng.uniform(0.28, 0.42), rng.uniform(0.055, 0.075),
            rng.uniform(-0.04, 0.04), rng.uniform(-0.04, 0.04), rng.uniform(0, 3.0), mat_flame_core
        )
    # Mid vivid orange flames (taller licking tongues visible clearly from wide cam1!)
    for i in range(7):
        a = (i / 7.0) * math.tau + 0.15
        make_flame_tongue(
            f"FlameMid_{i}", math.cos(a) * rng.uniform(0.05, 0.13), math.sin(a) * rng.uniform(0.05, 0.13),
            0.08, rng.uniform(0.42, 0.64), rng.uniform(0.065, 0.095),
            rng.uniform(-0.08, 0.08), rng.uniform(-0.08, 0.08), rng.uniform(0, 4.0), mat_flame_mid
        )
    # Outer deep red-orange wisps & rising spark embers
    for i in range(6):
        a = (i / 6.0) * math.tau + 0.7
        make_flame_tongue(
            f"FlameOuter_{i}", math.cos(a) * rng.uniform(0.10, 0.19), math.sin(a) * rng.uniform(0.10, 0.19),
            0.07, rng.uniform(0.32, 0.56), rng.uniform(0.045, 0.075),
            rng.uniform(-0.11, 0.11), rng.uniform(-0.11, 0.11), rng.uniform(0, 5.0), mat_flame_outer
        )


def import_hero_seated(gx, gz, yaw_deg):
    gy = terrain_height_at(gx, gz)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(HERO_GLB))
    added = [o for o in bpy.data.objects if o not in before]
    root = bpy.data.objects.new("Hero_Campfire_Root", None)
    root.location = g2b(gx, gy - 0.01, gz)
    root.rotation_euler = Euler((0.0, 0.0, math.radians(yaw_deg)), "XYZ")
    bpy.context.scene.collection.objects.link(root)
    for o in added:
        if o.parent is None:
            o.parent = root
    return root


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print("Building 4K Red Desert Campfire Overhang scene...", flush=True)

    # 1. Load real Poly Haven 4K photogrammetry templates — harmonized to warm Utah/Red Desert sandstone!
    cliff_01 = import_model_meshes("namaqualand_cliff_01", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.12, 0.78, 0.60), tint_gamma=1.10)[0]
    cliff_02 = import_model_meshes("namaqualand_cliff_02", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.14, 0.76, 0.58), tint_gamma=1.12)[0]
    big_cliff = import_model_meshes("coastal_cliff_02", center_submeshes=True, tint_tag="utah_red_big", tint_rgb=(1.12, 0.66, 0.46), tint_gamma=1.18)[0]
    rock_f1 = import_model_meshes("rock_face_01", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.14, 0.72, 0.52), tint_gamma=1.12)[0]
    rock_f2 = import_model_meshes("rock_face_02", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.14, 0.72, 0.52), tint_gamma=1.12)[0]

    boulders = [
        import_model_meshes("namaqualand_boulder_02", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.10, 0.80, 0.64), tint_gamma=1.08)[0],
        import_model_meshes("namaqualand_boulder_03", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.10, 0.80, 0.64), tint_gamma=1.08)[0],
        import_model_meshes("namaqualand_boulder_04", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.08, 0.80, 0.64), tint_gamma=1.08)[0],
        import_model_meshes("namaqualand_boulder_05", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.10, 0.80, 0.64), tint_gamma=1.08)[0],
        import_model_meshes("namaqualand_boulder_06", center_submeshes=True, tint_tag="utah_red", tint_rgb=(1.10, 0.80, 0.64), tint_gamma=1.08)[0],
    ]
    boulders_small = import_model_meshes("namaqualand_boulders_01", center_submeshes=True, tint_tag="utah_sand", tint_rgb=(0.94, 0.72, 0.56), tint_gamma=1.12, desat=0.28)
    rocks_01 = import_model_meshes("namaqualand_rocks_01", center_submeshes=True, tint_tag="utah_sand", tint_rgb=(0.90, 0.68, 0.52), tint_gamma=1.18, desat=0.35)
    stones_01 = import_model_meshes("namaqualand_stones_01", center_submeshes=True, tint_tag="utah_sand", tint_rgb=(0.90, 0.68, 0.52), tint_gamma=1.18, desat=0.35)
    bushes = import_model_meshes("wild_rooibos_bush", center_submeshes=True)
    branches = [
        import_model_meshes("dead_quiver_branch_01", center_submeshes=True, tint_tag="char_wood", tint_rgb=(0.52, 0.36, 0.25), tint_gamma=1.2)[0],
        import_model_meshes("dead_quiver_branch_02", center_submeshes=True, tint_tag="char_wood", tint_rgb=(0.52, 0.36, 0.25), tint_gamma=1.2)[0],
    ]
    bark = import_model_meshes("bark_debris_01", center_submeshes=True, tint_tag="char_bark", tint_rgb=(0.78, 0.56, 0.42), tint_gamma=1.12)

    # 2. Displaced 4K PBR red desert floor
    build_desert_terrain()

    # 3. HERO OVERHANGING RED SANDSTONE SLABS — dramatic diagonal cantilevered shelter arching cleanly over campfire & Echo!
    place_inst(cliff_02, "Overhang_MainSlab_A", (-3.1, 2.85, -7.8), rot_deg=(10.0, 32.0, -8.0), scale=(0.55, 0.58, 0.44))
    place_inst(cliff_02, "Overhang_UpperSlab_B", (-0.5, 4.35, -9.2), rot_deg=(8.0, 34.0, -4.0), scale=(0.66, 0.60, 0.48))
    place_inst(cliff_02, "Overhang_LeftBrow_C", (-6.0, 2.65, -4.8), rot_deg=(-4.0, -24.0, 22.0), scale=(0.44, 0.50, 0.44))

    # Deep dark back-wall cave recess behind campfire
    place_inst(cliff_01, "CaveBackWall_Center", (-0.6, -0.35, -11.4), rot_deg=(4.0, 4.0, 6.0), scale=(1.35, 1.15, 1.35))
    place_inst(cliff_01, "CaveBackWall_Left", (-6.4, -0.35, -9.6), rot_deg=(4.0, -8.0, 28.0), scale=(1.25, 1.10, 1.35))
    place_inst(cliff_01, "CaveBackWall_Right", (5.2, -0.30, -10.8), rot_deg=(4.0, 8.0, -22.0), scale=(1.20, 1.05, 1.20))

    # Massive stratified upper-right & upper-back red canyon cliff wall filling entire top of frame!
    place_inst(big_cliff, "UpperCanyonWall_Main", (3.8, 2.2, -13.5), rot_deg=(3.0, -3.0, -10.0), scale=(0.72, 0.72, 0.92))
    place_inst(cliff_02, "UpperCanyonWall_Right", (8.8, 0.5, -9.8), rot_deg=(4.0, -12.0, -46.0), scale=(0.56, 0.64, 0.80))
    place_inst(cliff_01, "UpperCanyonCap_Left", (-6.5, 5.8, -9.5), rot_deg=(-4.0, 10.0, 14.0), scale=(1.45, 1.15, 1.15))
    place_inst(rock_f1, "RightAlcoveWall_A", (7.6, -0.2, -5.5), rot_deg=(0.0, -6.0, -78.0), scale=(1.5, 1.2, 1.4))
    place_inst(rock_f2, "LeftAlcoveWall_A", (-8.4, -0.2, -2.5), rot_deg=(0.0, 8.0, 72.0), scale=(1.5, 1.3, 1.45))

    # 4. HERO CAMPFIRE + SEATED TRAVELER — facing INTO the flames on a flat weathered desert rock seat!
    camp_gx, camp_gz = -0.38, -5.85
    build_campfire(camp_gx, camp_gz, stones_01 + rocks_01, branches)
    # Flat weathered desert sandstone seat boulder directly under Echo's hips so she sits solidly flush on stone!
    seat_y = terrain_height_at(0.58, -5.60)
    place_inst(boulders[3], "Hero_SeatBoulder", (0.60, seat_y + 0.02, -5.56), rot_deg=(2, -4, 34), scale=(0.74, 0.68, 0.62))
    # Character front is -Y in Blender (= +Z in Godot); yaw = -72° rotates front (-Y) toward (-X, +Y_bl = -Z_gd) = straight into the campfire!!
    import_hero_seated(gx=0.58, gz=-5.62, yaw_deg=-72.0)

    # 5. HERO FOREGROUND SUNLIT RED SANDSTONE BOULDERS (left & edge framing ONLY — clean open sightline to campfire!)
    hero_boulders = [
        # Prominent sunlit slanted fractured slab in lower-left foreground (exact Screenshot 2 left diagonal rock!)
        (0, (-3.15, -0.18, -0.15), (14, -18, 28), (1.35, 1.25, 1.05)),
        (2, (-4.55, -0.12, -1.75), (6, 12, 54), (1.15, 1.10, 1.00)),
        (4, (-2.65, -0.16, 1.45), (-6, 14, -42), (1.45, 1.35, 1.10)),
        (1, (-4.95, -0.10, 0.75), (4, -10, 112), (1.05, 1.00, 0.92)),
        # Midground flanking boulders framing cave mouth cleanly (NEVER blocking campfire or Echo!)
        (0, (-3.65, -0.10, -4.85), (8, -22, 16), (0.95, 0.90, 0.82)),
        (3, (-2.15, -0.10, -3.95), (-6, 14, 78), (0.82, 0.78, 0.72)),
        (4, (2.35, -0.10, -5.25), (4, -12, -32), (1.05, 1.00, 0.85)),
        (2, (4.85, -0.15, -4.20), (-8, 10, -68), (0.92, 0.88, 0.82)),
        (1, (5.45, -0.12, -1.25), (5, -8, -110), (0.85, 0.92, 0.76)),
        (3, (3.45, -0.14, 1.85), (-5, -14, 142), (0.95, 1.05, 0.72)),
        # Flanking cliff talus boulders stepping up left & right
        (1, (-5.45, 0.15, -6.25), (12, 24, 42), (1.15, 1.15, 1.10)),
        (2, (4.15, 0.25, -7.25), (-10, -18, -45), (1.10, 1.05, 1.15)),
        (0, (6.05, 0.65, -8.25), (14, -16, -24), (1.25, 1.20, 1.10)),
    ]
    for idx, (bi, pos, rot, sc) in enumerate(hero_boulders):
        y_ground = terrain_height_at(pos[0], pos[2]) + pos[1]
        place_inst(boulders[bi], f"HeroBoulder_{idx:02d}", (pos[0], y_ground, pos[2]), rot, sc)

    # 6. DENSE PHOTOGRAMMETRY RUBBLE, COBBLES, GRAVEL STONES & DRY DESERT SCRUB SCATTER
    rng = random.Random(2026)
    scatter_pool = boulders_small + rocks_01 + stones_01

    # 380 real photogrammetry medium/small rocks & flat embedded sandstone slabs for dense Megascans desert floor!
    placed_rocks = 0
    for i in range(480):
        gx = rng.uniform(-7.2, 7.2)
        gz = rng.uniform(-8.2, 3.4)
        if math.hypot(gx - camp_gx, gz - camp_gz) < 0.85:
            continue
        if math.hypot(gx - 0.58, gz + 5.62) < 0.80:
            continue
        if math.hypot(gx - 0.55, gz - 4.10) < 1.3:
            continue
        if math.hypot(gx - 2.35, gz + 2.50) < 0.9:
            continue
        gy = terrain_height_at(gx, gz) - rng.uniform(0.02, 0.06)
        tpl = scatter_pool[rng.randrange(len(scatter_pool))]
        dist_wall = min(abs(gx + 5.0), abs(gz + 7.5))
        base_sc = rng.uniform(0.75, 2.05) if dist_wall < 3.0 else rng.uniform(0.65, 1.75)
        place_inst(
            tpl, f"DesertRock_{placed_rocks:03d}",
            (gx, gy, gz),
            (rng.uniform(-22, 22), rng.uniform(-22, 22), rng.uniform(0, 360)),
            (base_sc * rng.uniform(0.85, 1.4), base_sc * rng.uniform(0.85, 1.4), base_sc * rng.uniform(0.55, 1.15)),
        )
        placed_rocks += 1
        if placed_rocks >= 380:
            break

    # 34 low flat sunk sandstone boulders embedded directly into the foreground & right sandy wash (Megascans scree slabs!)
    for i in range(34):
        gx = rng.uniform(-4.5, 6.2)
        gz = rng.uniform(-5.5, 2.8)
        if math.hypot(gx - camp_gx, gz - camp_gz) < 1.65:
            continue
        if math.hypot(gx - 0.58, gz + 5.62) < 1.35:
            continue
        if math.hypot(gx - 0.55, gz - 4.10) < 1.6:
            continue
        gy = terrain_height_at(gx, gz) - rng.uniform(0.14, 0.24)
        tpl = boulders[rng.randrange(len(boulders))]
        sc = rng.uniform(0.28, 0.58)
        place_inst(tpl, f"EmbeddedSlab_{i:02d}", (gx, gy, gz), (rng.uniform(-12, 12), rng.uniform(-12, 12), rng.uniform(0, 360)), (sc * 1.25, sc * 1.15, sc * 0.52))

    # Extra larger talus boulders scattered on outer slopes ONLY (keep open central sightline to campfire!)
    for i in range(18):
        gx = rng.uniform(-6.5, 7.5)
        gz = rng.uniform(-8.2, 2.5)
        if abs(gx) < 2.2 and gz > -6.5 and gz < 3.5:
            continue
        if math.hypot(gx - 0.2, gz + 5.7) < 2.5:
            continue
        if math.hypot(gx - 0.65, gz - 4.0) < 2.2:
            continue
        if math.hypot(gx - 2.35, gz + 2.45) < 1.8:
            continue
        gy = terrain_height_at(gx, gz) - rng.uniform(0.05, 0.14)
        tpl = boulders[rng.randrange(len(boulders))]
        sc = rng.uniform(0.28, 0.58)
        place_inst(tpl, f"TalusBoulder_{i:02d}", (gx, gy, gz), (rng.uniform(-18, 18), rng.uniform(-18, 18), rng.uniform(0, 360)), sc)

    # 20 dry desert scrub tufts (wild_rooibos_bush) + scattered dead desert twigs/bark
    scrub_spots = [
        (0.95, 0.35, 0.65), (1.35, -0.45, 0.52), (3.15, -1.85, 0.72), (4.10, -0.35, 0.85),
        (-0.85, -1.35, 0.48), (-2.15, -2.15, 0.58), (2.35, -3.95, 0.55), (3.85, -5.65, 0.75),
        (-3.45, 1.15, 0.68), (1.85, 1.85, 0.56), (4.85, -3.10, 0.78), (0.35, -2.75, 0.42),
        (5.45, -6.20, 0.88), (-1.45, 0.45, 0.45), (3.15, 0.65, 0.62), (2.75, -6.75, 0.60),
    ]
    for i, (gx, gz, sc) in enumerate(scrub_spots):
        gy = terrain_height_at(gx, gz) - 0.03
        place_inst(bushes[i % len(bushes)], f"DesertScrub_{i:02d}", (gx, gy, gz), (rng.uniform(-6, 6), rng.uniform(-6, 6), rng.uniform(0, 360)), sc)

    for i in range(14):
        gx = rng.uniform(-3.6, 3.8)
        gz = rng.uniform(-6.8, -1.8)
        if math.hypot(gx - camp_gx, gz - camp_gz) < 0.75:
            continue
        gy = terrain_height_at(gx, gz) + 0.01
        tpl = bark[i % len(bark)]
        sc = rng.uniform(0.75, 1.25)
        place_inst(tpl, f"DesertDebris_{i:02d}", (gx, gy, gz), (rng.uniform(-8, 8), rng.uniform(-12, 12), rng.uniform(0, 360)), sc)

    # Stats
    objs = [o for o in bpy.context.scene.collection.objects if o.type == "MESH"]
    uniq = set(o.data for o in objs)
    uniq_tris = sum(len(m.polygons) for m in uniq)
    inst_tris = sum(len(o.data.polygons) for o in objs)
    print(f"[red_desert_campfire] objects={len(objs)} unique_meshes={len(uniq)} unique_polys={uniq_tris:,} instanced_polys={inst_tris:,}", flush=True)

    # Export clean glTF 2.0 (separate bin & 4K JPEG textures for crisp UE 5.9 + GodotNext parity)
    out_gltf = OUT_DIR / "red_desert_campfire.gltf"
    bpy.ops.export_scene.gltf(
        filepath=str(out_gltf),
        export_format="GLTF_SEPARATE",
        export_texcoords=True,
        export_normals=True,
        export_materials="EXPORT",
        export_image_format="JPEG",
        export_image_quality=92,
    )
    print(f"[red_desert_campfire] exported -> {out_gltf}", flush=True)

    # Quick EEVEE composition preview of cam1 & cam2 so we verify framing BEFORE 4K UE/GN capture!
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1280
    scene.render.resolution_y = 720
    world = bpy.data.worlds.new("PrevW")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (0.35, 0.45, 0.65, 1.0)
    bg.inputs["Strength"].default_value = 0.7
    scene.world = world

    sun_d = bpy.data.lights.new("Sun", "SUN")
    sun_d.energy = 6.5
    sun_d.color = (1.0, 0.90, 0.76)
    sun_o = bpy.data.objects.new("Sun", sun_d)
    sun_o.rotation_euler = g2b(-0.46, -0.76, -0.45).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(sun_o)

    fire_d = bpy.data.lights.new("Fire", "POINT")
    fire_d.energy = 420.0
    fire_d.color = (1.0, 0.50, 0.14)
    fire_d.shadow_soft_size = 0.18
    fire_o = bpy.data.objects.new("Fire", fire_d)
    fire_o.location = g2b(camp_gx, terrain_height_at(camp_gx, camp_gz) + 0.46, camp_gz)
    scene.collection.objects.link(fire_o)

    for cid, pos, tgt, vfov in [
        ("cam1", (0.55, 2.05, 4.10), (-0.20, 1.15, -6.0), 40.0),
        ("cam2", (2.35, 1.18, -2.50), (0.05, 0.68, -5.80), 38.0),
    ]:
        cd = bpy.data.cameras.new(cid)
        cd.sensor_fit = "VERTICAL"
        cd.sensor_height = 24.0
        cd.lens = 12.0 / math.tan(math.radians(vfov) * 0.5)
        co = bpy.data.objects.new(cid, cd)
        co.location = g2b(*pos)
        co.rotation_euler = (g2b(*tgt) - co.location).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(co)
        scene.camera = co
        scene.render.filepath = str(OUT_DIR / f"preview_{cid}.png")
        bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
