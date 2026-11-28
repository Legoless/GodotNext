#!/usr/bin/env python3
"""Assemble the 4K Photogrammetry 'Dark Canyon Sunset Vista' scene in Blender 4.2.3 LTS
and export to glTF 2.0 (shared instanced photogrammetry cliff/spire scans + ancient stone gate ruins
+ standing hero character over-the-shoulder). Matches the iconic Valley of the Ancient Dark World
sunset vista + Echo overlook composition."""
import math
import os
import random
from pathlib import Path
import numpy as np
import bpy
import bmesh
from mathutils import Vector, Euler, Matrix


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
HERO_GLB = SRC / "hero_character/hero_overlook.glb"
OUT_DIR = TOOLS_DIR / "ue59-real-scenes/ancient_canyon_vista_4k"
TINT_DIR = OUT_DIR / "_tinted_tex"
OUT_DIR.mkdir(parents=True, exist_ok=True)
TINT_DIR.mkdir(parents=True, exist_ok=True)


def g2b(gx, gy, gz):
    return Vector((float(gx), float(-gz), float(gy)))


def g_rot_to_b(rx_deg=0.0, ry_deg=0.0, rz_deg=0.0):
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


def import_model_meshes(model_name: str, center_submeshes=True, tint_tag=None, tint_rgb=None, tint_gamma=1.0, desat=0.0):
    gltf = next((MODELS / model_name).glob("*.gltf"))
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(gltf))
    added = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in added if o.type == "MESH"]

    templates = {}
    seen_mats = set()
    for o in meshes:
        mat_w = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = Matrix.Identity(4)
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
        for col in list(o.users_collection):
            col.objects.unlink(o)
        templates[o.name] = o

    for o in added:
        if o.type != "MESH":
            bpy.data.objects.remove(o, do_unlink=True)
    return templates


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


def make_weathered_stone_mat(name="AncientRuins_Stone"):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    diff_src = TEX / "large_sandstone_blocks_01/large_sandstone_blocks_01_Diffuse_4k.jpg"
    diff_t = tint_image_cached(diff_src, "ancient_dark_stone", mul_rgb=(0.56, 0.50, 0.44), gamma=1.18, desat=0.45)
    td = nt.nodes.new("ShaderNodeTexImage")
    td.image = load_img(diff_t, non_color=False)
    nt.links.new(td.outputs["Color"], bsdf.inputs["Base Color"])

    tr = nt.nodes.new("ShaderNodeTexImage")
    tr.image = load_img(TEX / "large_sandstone_blocks_01/large_sandstone_blocks_01_Rough_4k.png", non_color=True)
    nt.links.new(tr.outputs["Color"], bsdf.inputs["Roughness"])

    tn = nt.nodes.new("ShaderNodeTexImage")
    tn.image = load_img(TEX / "large_sandstone_blocks_01/large_sandstone_blocks_01_nor_gl_4k.png", non_color=True)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nm.inputs["Strength"].default_value = 1.2
    nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def make_canyon_floor_mat():
    mat = bpy.data.materials.new("DarkCanyon_BasinFloor")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    diff_src = TEX / "cliff_side/cliff_side_Diffuse_4k.jpg"
    diff_t = tint_image_cached(diff_src, "volcanic_basin", mul_rgb=(0.36, 0.35, 0.40), gamma=1.25, desat=0.65)
    td = nt.nodes.new("ShaderNodeTexImage")
    td.image = load_img(diff_t, non_color=False)
    nt.links.new(td.outputs["Color"], bsdf.inputs["Base Color"])

    tr = nt.nodes.new("ShaderNodeTexImage")
    tr.image = load_img(TEX / "cliff_side/cliff_side_Rough_4k.png", non_color=True)
    nt.links.new(tr.outputs["Color"], bsdf.inputs["Roughness"])

    tn = nt.nodes.new("ShaderNodeTexImage")
    tn.image = load_img(TEX / "cliff_side/cliff_side_nor_gl_4k.png", non_color=True)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nm.inputs["Strength"].default_value = 1.15
    nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def build_canyon_basin_terrain(mat):
    """Sweeping rugged canyon valley floor far below the overlook so there are never void gaps."""
    nx, nz = 120, 120
    bm = bmesh.new()
    grid = []
    for iz in range(nz + 1):
        vz = iz / float(nz)
        gz = 15.0 - vz * 290.0  # +15 .. -275
        row = []
        for ix in range(nx + 1):
            vx = ix / float(nx)
            gx = -180.0 + vx * 360.0
            # Deep canyon gorge running down the middle (y ~ -28..-18) rising steeply into flanking mountains
            canyon_wall = 0.0028 * (gx * gx)
            ridge = 4.5 * math.sin(gx * 0.045 + gz * 0.032) + 2.8 * math.cos(gx * 0.11 - gz * 0.075)
            # Foreground overlook terrace near z = 0..-6, dropping sharply into valley for z < -8
            if gz > -8.0 and abs(gx) < 16.0:
                gy = -1.6 + 0.15 * math.sin(gx * 0.4)
            else:
                drop = min(1.0, max(0.0, (-8.0 - gz) / 16.0))
                gy = -22.0 * drop + canyon_wall * 0.35 + ridge * drop
            row.append(bm.verts.new(g2b(gx, gy, gz)))
        grid.append(row)

    uv_layer = bm.loops.layers.uv.new("UVMap")
    for iz in range(nz):
        for ix in range(nx):
            f = bm.faces.new([grid[iz][ix], grid[iz][ix + 1], grid[iz + 1][ix + 1], grid[iz + 1][ix]])
            f.smooth = True
            for lp, uv in zip(f.loops, [(ix * 0.25, iz * 0.25), ((ix + 1) * 0.25, iz * 0.25), ((ix + 1) * 0.25, (iz + 1) * 0.25), (ix * 0.25, (iz + 1) * 0.25)]):
                lp[uv_layer].uv = uv

    me = bpy.data.meshes.new("CanyonBasinMesh")
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new("CanyonBasin", me)
    bpy.context.scene.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def build_ornate_sentinel_tower(name, g_pos, yaw_deg, height_scale, stone_mat):
    """Build an intricate weathered ancient carved stone gate pylon/tower (matching the tiered
    carved stone ruins right below Echo in Screenshot 1!) using beveled masonry tiers, cornices,
    carved vertical pilasters, cantilevered corbelled crown, and broken stone top."""
    bm = bmesh.new()

    def add_box(cx, cy, cz, sx, sy, sz, rot_z=0.0):
        m = Matrix.Translation(Vector((cx, cy, cz))) @ Matrix.Rotation(math.radians(rot_z), 4, "Z") @ Matrix.Diagonal((sx, sy, sz, 1.0))
        bmesh.ops.create_cube(bm, size=1.0, matrix=m)

    # Tapered carved masonry tiers from base z=0 to z=11.5
    tiers = [
        (0.0, 2.4, 3.6, 3.6),
        (2.4, 2.6, 3.1, 3.1),
        (5.0, 0.55, 3.7, 3.7),  # projecting cornice
        (5.55, 2.8, 2.6, 2.6),
        (8.35, 0.55, 3.35, 3.35),  # upper cornice
        (8.90, 1.9, 2.3, 2.3),
        (10.80, 0.65, 3.1, 3.1),  # cantilevered weathered crown
    ]
    for z0, h, wx, wy in tiers:
        add_box(0.0, 0.0, z0 + h * 0.5, wx, wy, h)

    # Corner carved buttress pilasters + tiered stone ribs
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            add_box(sx * 1.48, sy * 1.48, 4.2, 0.52, 0.52, 8.2)
            add_box(sx * 1.25, sy * 1.25, 9.6, 0.44, 0.44, 2.5)
    # Recessed decorative stone lintels / carved arch corbels projecting inward toward the gateway
    for z_corb, reach in [(6.8, 1.8), (9.6, 2.4), (10.9, 3.1)]:
        add_box(reach * 0.5 + 0.6, 0.0, z_corb, reach, 1.35, 0.58)
        # Stepped underside bracket
        add_box(reach * 0.35 + 0.55, 0.0, z_corb - 0.52, reach * 0.65, 0.95, 0.48)
    # Jagged broken stone battlements / pinnacles at crown
    rng = random.Random(hash(name) & 0xFFFF)
    for px, py, ph in [(-1.0, -1.0, 1.4), (1.0, -0.9, 1.9), (-0.9, 1.0, 1.6), (0.8, 0.9, 1.1), (0.0, 0.0, 2.2)]:
        add_box(px, py, 11.4 + ph * 0.5, rng.uniform(0.6, 0.95), rng.uniform(0.6, 0.95), ph, rng.uniform(-6, 6))

    # Bevel edges so sunset rim light catches crisp stone masonry chamfers!
    bmesh.ops.bevel(bm, geom=bm.edges[:], offset=0.055, segments=2, affect="EDGES", clamp_overlap=True)

    # Box-project clean world-scale UVs
    uv_layer = bm.loops.layers.uv.new("UVMap")
    for f in bm.faces:
        n = f.normal
        ax, ay, az = abs(n.x), abs(n.y), abs(n.z)
        for lp in f.loops:
            co = lp.vert.co
            if az >= ax and az >= ay:
                lp[uv_layer].uv = (co.x * 0.35, co.y * 0.35)
            elif ax >= ay:
                lp[uv_layer].uv = (co.y * 0.35, co.z * 0.35)
            else:
                lp[uv_layer].uv = (co.x * 0.35, co.z * 0.35)

    me = bpy.data.meshes.new(name + "_Mesh")
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    obj.location = g2b(*g_pos)
    obj.rotation_euler = Euler((0.0, 0.0, math.radians(yaw_deg)), "XYZ")
    obj.scale = (height_scale, height_scale, height_scale)
    bpy.context.scene.collection.objects.link(obj)
    obj.data.materials.append(stone_mat)
    return obj


def build_colossal_ruined_ring(name, g_pos, rot_deg, radius, stone_mat):
    """Colossal half-buried weathered ancient stone ring/rib emerging from the central distant ridge
    (echoing the iconic ancient titan collar silhouette in Screenshot 1)."""
    bm = bmesh.new()
    segs = 40
    # Partial arc emerging out of rock (0..190 degrees)
    for i in range(segs):
        t = i / float(segs)
        ang = math.radians(-15.0 + t * 185.0)
        cx = math.cos(ang) * radius
        cz = math.sin(ang) * radius
        # Alternating thick ribbed ancient stone segments
        thick = 2.6 if (i % 3 != 0) else 3.35
        depth = 4.2 if (i % 3 != 0) else 5.1
        m = (
            Matrix.Translation(Vector((cx, 0.0, cz)))
            @ Matrix.Rotation(-ang, 4, "Y")
            @ Matrix.Diagonal((thick, depth, (math.pi * radius / segs) * 1.12, 1.0))
        )
        bmesh.ops.create_cube(bm, size=1.0, matrix=m)
    bmesh.ops.bevel(bm, geom=bm.edges[:], offset=0.14, segments=1, affect="EDGES", clamp_overlap=True)
    uv_layer = bm.loops.layers.uv.new("UVMap")
    for f in bm.faces:
        for lp in f.loops:
            lp[uv_layer].uv = (lp.vert.co.x * 0.12, lp.vert.co.z * 0.12)
    me = bpy.data.meshes.new(name + "_Mesh")
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    obj.location = g2b(*g_pos)
    obj.rotation_euler = g_rot_to_b(*rot_deg)
    bpy.context.scene.collection.objects.link(obj)
    obj.data.materials.append(stone_mat)
    return obj


def import_hero_overlook(gx, gy, gz, yaw_deg):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(HERO_GLB))
    added = [o for o in bpy.data.objects if o not in before]
    root = bpy.data.objects.new("Hero_Overlook_Root", None)
    root.location = g2b(gx, gy, gz)
    root.rotation_euler = Euler((0.0, 0.0, math.radians(yaw_deg)), "XYZ")
    bpy.context.scene.collection.objects.link(root)
    for o in added:
        if o.parent is None:
            o.parent = root
    return root


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print("Building 4K Dark Canyon Sunset Vista scene...", flush=True)

    # 1. Load real Poly Haven 4K photogrammetry cliffs & mountains, tinted into brooding dark volcanic basalt-slate!
    dark_tint = (0.38, 0.37, 0.42)
    spire_mtn = list(import_model_meshes("mountainside", True, "dark_basalt", dark_tint, 1.25, desat=0.65).values())[0]
    cliff_big4 = list(import_model_meshes("coastal_cliff_04", True, "dark_basalt", (0.36, 0.35, 0.40), 1.25, desat=0.72).values())[0]
    cliff_big2 = list(import_model_meshes("coastal_cliff_02", True, "dark_basalt", (0.40, 0.39, 0.44), 1.22, desat=0.70).values())[0]
    cliff_n2 = list(import_model_meshes("namaqualand_cliff_02", True, "dark_basalt", (0.42, 0.40, 0.44), 1.22, desat=0.72).values())[0]
    cliff_n1 = list(import_model_meshes("namaqualand_cliff_01", True, "dark_basalt", (0.42, 0.40, 0.45), 1.20, desat=0.72).values())[0]
    boulders = [
        list(import_model_meshes("namaqualand_boulder_02", True, "dark_basalt", (0.44, 0.42, 0.46), 1.20, desat=0.70).values())[0],
        list(import_model_meshes("namaqualand_boulder_03", True, "dark_basalt", (0.44, 0.42, 0.46), 1.20, desat=0.70).values())[0],
        list(import_model_meshes("namaqualand_boulder_04", True, "dark_basalt", (0.42, 0.40, 0.44), 1.20, desat=0.70).values())[0],
    ]
    rocks_01 = list(import_model_meshes("namaqualand_rocks_01", True, "dark_basalt", (0.45, 0.43, 0.47), 1.2, desat=0.7).values())
    fort = import_model_meshes("modular_fort_01", True, "dark_ruin", (0.52, 0.47, 0.42), 1.22, desat=0.55)

    stone_mat = make_weathered_stone_mat()
    basin_mat = make_canyon_basin_terrain_mat = make_canyon_floor_mat()
    build_canyon_basin_terrain(basin_mat)

    # 2. TIER 1: FOREGROUND CRAGGY OVERLOOK LEDGE + HERO CHARACTER (left-foreground over-the-shoulder!)
    # Solid craggy rocky cliff terrace under Hero at (-0.42, 0.0, -2.30)
    place_inst(cliff_n1, "Overlook_BaseLedge", (-0.5, -4.35, -3.4), rot_deg=(0, 4, 12), scale=(1.1, 1.0, 1.0))
    place_inst(boulders[0], "Overlook_LeftCrag", (-2.65, -0.35, -4.2), rot_deg=(8, -14, 22), scale=(1.45, 1.35, 1.65))
    place_inst(boulders[2], "Overlook_FarLeftSpire", (-5.4, -0.8, -7.8), rot_deg=(12, -22, 48), scale=(1.9, 1.8, 2.4))
    place_inst(boulders[1], "Overlook_RightDrop", (2.8, -1.6, -5.2), rot_deg=(-8, 18, -32), scale=(1.35, 1.25, 1.55))
    place_inst(cliff_n2, "Overlook_DropShelf", (0.0, -6.4, -7.2), rot_deg=(8, -6, 6), scale=(0.65, 0.65, 0.72))

    # Hero character standing at foreground-left looking out over the sunset ruins & spires!
    # Character front is -Y in Blender (= +Z in Godot); yaw=162° turns her to face away along -Z and slightly right (+X)!
    import_hero_overlook(gx=-0.50, gy=0.0, gz=-2.35, yaw_deg=166.0)

    # 3. TIER 2: TWIN ANCIENT CARVED STONE GATE TOWERS & RUINS — weathered ancient stone sentinel towers
    # rising organically out of craggy valley rocks (capping fort tops with weathered rock/masonry so zero flat modern roofs!)
    if "modular_fort_01_wall_thin_gate_01" in fort:
        place_inst(fort["modular_fort_01_wall_thin_gate_01"], "AncientGate_ArchLeft", (-2.6, -15.5, -36.0), rot_deg=(0, 3, 18), scale=(0.76, 0.76, 1.05))
        place_inst(fort["modular_fort_01_wall_thin_gate_01"], "AncientGate_ArchRight", (5.4, -16.0, -37.5), rot_deg=(0, -3, -18), scale=(0.76, 0.76, 1.08))
    if "modular_fort_01_tower_round" in fort:
        place_inst(fort["modular_fort_01_tower_round"], "FortRuin_KeepLeft", (-18.5, -18.5, -52.0), rot_deg=(4, -6, 22), scale=(0.56, 0.56, 0.76))
        place_inst(fort["modular_fort_01_tower_round"], "FortRuin_KeepRight", (21.5, -19.5, -56.0), rot_deg=(-3, 5, -35), scale=(0.54, 0.54, 0.74))
    if "modular_fort_01_wall_thick_straight_02" in fort:
        place_inst(fort["modular_fort_01_wall_thick_straight_02"], "FortRuin_WallA", (-10.8, -19.8, -42.0), rot_deg=(0, 6, 54), scale=(0.74, 0.74, 1.15))
        place_inst(fort["modular_fort_01_wall_thick_straight_02"], "FortRuin_WallB", (13.8, -20.8, -44.0), rot_deg=(0, -7, -52), scale=(0.74, 0.74, 1.15))
    # Twin carved weathered stone sentinel gate pylons rising cleanly between the jagged crags right below Echo!
    build_ornate_sentinel_tower("AncientGate_SpireLeft", (-2.4, -14.2, -33.5), yaw_deg=-14.0, height_scale=0.68, stone_mat=stone_mat)
    build_ornate_sentinel_tower("AncientGate_SpireRight", (4.4, -14.8, -35.2), yaw_deg=164.0, height_scale=0.72, stone_mat=stone_mat)

    # Colossal half-buried weathered stone collar/arch deeply embedded in the central mountain ridge
    build_colossal_ruined_ring("Colossal_AncientRing", (7.5, -11.5, -94.0), rot_deg=(18, -32, 24), radius=14.0, stone_mat=stone_mat)

    # 4. TIER 3: RAZOR-SHARP JAGGED VOLCANIC MOUNTAIN SPIRES (tilted/angled 35-52 deg so EVERY peak stabs sharply into the sunset sky!)
    spires = [
        # --- Left foreground/midground jagged crags & angled needle spires ---
        (spire_mtn, "Spire_LeftClose", (-15.5, -11.5, -24.0), (8, 36, 24), (1.05, 1.05, 1.45)),
        (spire_mtn, "Spire_LeftMidA", (-34.0, -17.0, -68.0), (4, -40, 38), (1.75, 1.65, 2.85)),
        (spire_mtn, "Spire_LeftFarA", (-58.0, -19.0, -112.0), (6, 42, 16), (2.4, 2.2, 4.10)),
        (spire_mtn, "Spire_LeftFarB", (-82.0, -20.0, -148.0), (-4, -38, 28), (3.0, 2.8, 4.80)),
        (cliff_big2, "Cliff_LeftFlankLow", (-36.0, -21.0, -58.0), (8, 16, 68), (1.25, 1.25, 1.55)),

        # --- Iconic jagged volcanic spire rising just right of Echo's head, tapering to a sharp peak! ---
        (spire_mtn, "Spire_HeroMonolithA", (-4.5, -19.0, -82.0), (-4, 42, -16), (1.55, 1.55, 3.45)),
        (spire_mtn, "Spire_HeroMonolithB", (-1.5, -21.0, -86.0), (6, -34, 14), (1.35, 1.35, 2.75)),
        (cliff_n2, "Ridge_CenterShoulder", (11.0, -18.5, -78.0), (10, -26, -18), (1.65, 1.45, 1.75)),

        # --- Center-right sharp needle volcanic tooth silhouetted dramatically against golden sunset clouds! ---
        (spire_mtn, "Spire_CenterNeedleA", (27.0, -18.0, -98.0), (6, -46, -24), (1.45, 1.45, 3.65)),
        (spire_mtn, "Spire_CenterNeedleB", (34.0, -20.0, -104.0), (-4, 38, -32), (1.25, 1.25, 2.55)),
        (cliff_big4, "CanyonWall_RightLow", (48.0, -24.0, -78.0), (4, -12, -72), (1.15, 1.15, 1.75)),
        (cliff_big2, "Ridge_MidFloorCrags", (16.0, -24.0, -56.0), (8, 6, -24), (1.05, 1.15, 1.30)),
        (cliff_big2, "Ridge_MidLeftCrags", (-18.0, -22.0, -40.0), (6, -8, 32), (0.95, 1.05, 1.20)),

        # --- Far-right iconic soaring diagonal jagged spire stabbing high into the blazing sunset sky! ---
        (spire_mtn, "Spire_FarRightPeakA", (74.0, -20.0, -148.0), (4, -44, -36), (2.9, 2.7, 5.40)),
        (spire_mtn, "Spire_FarRightPeakB", (86.0, -22.0, -144.0), (-6, 36, -24), (2.4, 2.2, 3.85)),
        (spire_mtn, "Spire_FarRightEdge", (112.0, -24.0, -126.0), (6, 40, -52), (2.6, 2.4, 3.80)),
        (spire_mtn, "Spire_FarHorizonToothA", (46.0, -22.0, -195.0), (-2, -45, 14), (3.4, 3.0, 4.60)),
        (spire_mtn, "Spire_FarHorizonToothB", (14.0, -24.0, -188.0), (4, 42, -12), (2.8, 2.6, 3.80)),
        (cliff_big4, "HorizonRange_CenterLow", (8.0, -32.0, -210.0), (0, 6, 8), (2.4, 2.2, 2.35)),
        (cliff_big4, "HorizonRange_LeftLow", (-96.0, -28.0, -185.0), (0, -8, 22), (2.2, 2.0, 2.65)),
        (cliff_big4, "HorizonRange_RightLow", (128.0, -30.0, -195.0), (0, 10, -24), (2.3, 2.0, 2.55)),

        # --- Lower foreground canyon crags framing bottom corners ---
        (cliff_n2, "Crag_FrontRightLow", (13.5, -10.5, -18.0), (12, -18, -42), (0.78, 0.78, 0.88)),
        (spire_mtn, "Crag_MidRightSlope", (32.0, -18.0, -44.0), (8, -36, -48), (1.35, 1.25, 1.75)),
    ]
    for tpl, nm, pos, rot, sc in spires:
        place_inst(tpl, nm, pos, rot, sc)

    # Extra photogrammetry boulders & scree scattered across the overlook ledge & lower canyon slopes
    rng = random.Random(1337)
    for i in range(48):
        gx = rng.uniform(-9.0, 9.0)
        gz = rng.uniform(-12.0, -0.5)
        if math.hypot(gx + 0.50, gz + 2.35) < 1.15:
            continue
        if math.hypot(gx - 0.05, gz + 0.55) < 1.1:
            continue
        gy = -0.35 if gz > -4.8 else (-2.5 - 0.8 * (-4.8 - gz))
        tpl = boulders[i % len(boulders)] if i % 3 == 0 else rocks_01[i % len(rocks_01)]
        sc = rng.uniform(0.35, 0.85) if i % 3 == 0 else rng.uniform(1.2, 2.8)
        place_inst(tpl, f"OverlookRock_{i:02d}", (gx, gy, gz), (rng.uniform(-25, 25), rng.uniform(-25, 25), rng.uniform(0, 360)), sc)

    objs = [o for o in bpy.context.scene.collection.objects if o.type == "MESH"]
    uniq = set(o.data for o in objs)
    uniq_tris = sum(len(m.polygons) for m in uniq)
    inst_tris = sum(len(o.data.polygons) for o in objs)
    print(f"[ancient_canyon_vista] objects={len(objs)} unique_meshes={len(uniq)} unique_polys={uniq_tris:,} instanced_polys={inst_tris:,}", flush=True)

    out_gltf = OUT_DIR / "ancient_canyon_vista.gltf"
    bpy.ops.export_scene.gltf(
        filepath=str(out_gltf),
        export_format="GLTF_SEPARATE",
        export_texcoords=True,
        export_normals=True,
        export_materials="EXPORT",
        export_image_format="JPEG",
        export_image_quality=92,
    )
    print(f"[ancient_canyon_vista] exported -> {out_gltf}", flush=True)

    # Quick EEVEE composition preview of cam1 & cam2 using the real 8K sunset HDRI!
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1280
    scene.render.resolution_y = 720
    world = bpy.data.worlds.new("SunsetWorld")
    world.use_nodes = True
    nt = world.node_tree
    nt.nodes.clear()
    wout = nt.nodes.new("ShaderNodeOutputWorld")
    wbg = nt.nodes.new("ShaderNodeBackground")
    wbg.inputs["Strength"].default_value = 1.6
    wenv = nt.nodes.new("ShaderNodeTexEnvironment")
    wenv.image = load_img(SRC / "hdris/belfast_sunset_puresky_8k.hdr", non_color=False)
    wtc = nt.nodes.new("ShaderNodeTexCoord")
    wmp = nt.nodes.new("ShaderNodeMapping")
    wmp.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(-115.0))
    nt.links.new(wtc.outputs["Generated"], wmp.inputs["Vector"])
    nt.links.new(wmp.outputs["Vector"], wenv.inputs["Vector"])
    nt.links.new(wenv.outputs["Color"], wbg.inputs["Color"])
    nt.links.new(wbg.outputs["Background"], wout.inputs["Surface"])
    scene.world = world

    sun_d = bpy.data.lights.new("SunsetSun", "SUN")
    sun_d.energy = 6.5
    sun_d.color = (1.0, 0.72, 0.46)
    sun_o = bpy.data.objects.new("SunsetSun", sun_d)
    sun_o.rotation_euler = g2b(-0.66, -0.22, 0.72).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(sun_o)

    for cid, pos, tgt, vfov in [
        ("cam1", (0.10, 1.56, -0.78), (-0.08, 2.20, -24.0), 38.0),
        ("cam2", (9.5, 1.8, -12.0), (1.5, -6.5, -44.0), 42.0),
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
