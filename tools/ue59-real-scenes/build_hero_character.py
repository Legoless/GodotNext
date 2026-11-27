#!/usr/bin/env python3
"""Build a studio-quality CC0-based desert adventurer hero character in Blender 4.2.3 LTS
from Blender Foundation's GEO-body_female_realistic + Poly Haven PBR leather/linen/hessian
textures, skinned with deterministic capsule-blended armature weights (100% consistent across
body + garments!) and exported in TWO poses:
  1) hero_overlook.glb  (standing relaxed contrapposto, looking out over the canyon)
  2) hero_campfire.glb  (seated cross-legged/knees-bent beside a campfire)
"""
import math
import random
from pathlib import Path
import numpy as np
import bpy
import bmesh
from mathutils import Vector, Euler, Matrix

SRC = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/photogrammetry_sources")
BUNDLE = SRC / "human_bundle/human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend"
TEX_ROOT = SRC / "textures"
OUT_DIR = SRC / "hero_character"
TEX_2K = OUT_DIR / "_tex2k"
OUT_DIR.mkdir(parents=True, exist_ok=True)
TEX_2K.mkdir(parents=True, exist_ok=True)


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def get_2k_jpg(src_path: Path, tag: str, mul_rgb=None, is_gray=False) -> Path:
    dst = TEX_2K / f"{src_path.stem}_{tag}.jpg"
    if dst.is_file() and dst.stat().st_size > 500:
        return dst
    im = bpy.data.images.load(str(src_path))
    if max(im.size[0], im.size[1]) > 2048:
        im.scale(2048, 2048)
    if mul_rgb is not None and not is_gray:
        w, h = im.size[0], im.size[1]
        arr = np.empty(w * h * 4, dtype=np.float32)
        im.pixels.foreach_get(arr)
        arr = arr.reshape((h, w, 4))
        arr[..., 0] = np.clip(arr[..., 0] * mul_rgb[0], 0.0, 1.0)
        arr[..., 1] = np.clip(arr[..., 1] * mul_rgb[1], 0.0, 1.0)
        arr[..., 2] = np.clip(arr[..., 2] * mul_rgb[2], 0.0, 1.0)
        im.pixels.foreach_set(arr.ravel())
    im.filepath_raw = str(dst)
    im.file_format = "JPEG"
    im.save()
    bpy.data.images.remove(im)
    return dst


def load_image_once(path: Path, non_color: bool = False):
    key = str(path)
    for im in bpy.data.images:
        if im.filepath == key:
            return im
    im = bpy.data.images.load(key)
    if non_color:
        im.colorspace_settings.name = "Non-Color"
    return im


def make_pbr_tex_material(name: str, tex_folder: str, tint=(1.0, 1.0, 1.0), uv_scale=4.0, rough_val=0.72, normal_strength=0.8):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()

    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Roughness"].default_value = rough_val
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (uv_scale, uv_scale, uv_scale)
    nt.links.new(tc.outputs["UV"], mp.inputs["Vector"])

    tdir = TEX_ROOT / tex_folder
    diff_files = list(tdir.glob("*Diffuse*"))
    rough_files = list(tdir.glob("*Rough*"))
    nor_files = list(tdir.glob("*nor_gl*"))

    tag = name.lower()
    if diff_files:
        p = get_2k_jpg(diff_files[0], tag, mul_rgb=tint, is_gray=False)
        td = nt.nodes.new("ShaderNodeTexImage")
        td.image = load_image_once(p, non_color=False)
        nt.links.new(mp.outputs["Vector"], td.inputs["Vector"])
        nt.links.new(td.outputs["Color"], bsdf.inputs["Base Color"])
    else:
        bsdf.inputs["Base Color"].default_value = (tint[0], tint[1], tint[2], 1.0)

    if rough_files:
        p = get_2k_jpg(rough_files[0], "rough", is_gray=True)
        tr = nt.nodes.new("ShaderNodeTexImage")
        tr.image = load_image_once(p, non_color=True)
        nt.links.new(mp.outputs["Vector"], tr.inputs["Vector"])
        # Blend texture roughness with target range so cloth stays matte & leather has rich sheen
        mr = nt.nodes.new("ShaderNodeMapRange")
        mr.inputs["From Min"].default_value = 0.0
        mr.inputs["From Max"].default_value = 1.0
        mr.inputs["To Min"].default_value = max(0.25, rough_val - 0.16)
        mr.inputs["To Max"].default_value = min(0.96, rough_val + 0.16)
        nt.links.new(tr.outputs["Color"], mr.inputs["Value"])
        nt.links.new(mr.outputs["Result"], bsdf.inputs["Roughness"])

    if nor_files:
        p = get_2k_jpg(nor_files[0], "nor", is_gray=False)
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = load_image_once(p, non_color=True)
        nt.links.new(mp.outputs["Vector"], tn.inputs["Vector"])
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nm.inputs["Strength"].default_value = normal_strength
        nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])

    return mat


def make_skin_material():
    mat = bpy.data.materials.new(name="Hero_Skin")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    # Warm sun-tanned natural skin tone (rich warm undertones, matte-natural!)
    bsdf.inputs["Base Color"].default_value = (0.54, 0.29, 0.175, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.58
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.30
    nor_files = list((TEX_ROOT / "rough_linen").glob("*nor_gl*"))
    if nor_files:
        p = get_2k_jpg(nor_files[0], "nor", is_gray=False)
        tc = nt.nodes.new("ShaderNodeTexCoord")
        mp = nt.nodes.new("ShaderNodeMapping")
        mp.inputs["Scale"].default_value = (22.0, 22.0, 22.0)
        nt.links.new(tc.outputs["UV"], mp.inputs["Vector"])
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = load_image_once(p, non_color=True)
        nt.links.new(mp.outputs["Vector"], tn.inputs["Vector"])
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nm.inputs["Strength"].default_value = 0.08
        nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def make_hair_material():
    mat = bpy.data.materials.new(name="Hero_Hair")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    # Deep rich dark espresso-black hair with fine brushed strand relief
    bsdf.inputs["Base Color"].default_value = (0.025, 0.017, 0.013, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.56
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = 0.38
    nor_files = list((TEX_ROOT / "hessian_230").glob("*nor_gl*"))
    if nor_files:
        p = get_2k_jpg(nor_files[0], "nor", is_gray=False)
        tc = nt.nodes.new("ShaderNodeTexCoord")
        mp = nt.nodes.new("ShaderNodeMapping")
        mp.inputs["Scale"].default_value = (2.0, 18.0, 1.0)
        nt.links.new(tc.outputs["UV"], mp.inputs["Vector"])
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = load_image_once(p, non_color=True)
        nt.links.new(mp.outputs["Vector"], tn.inputs["Vector"])
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nm.inputs["Strength"].default_value = 0.55
        nt.links.new(tn.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def make_eye_material():
    mat = bpy.data.materials.new(name="Hero_Eye")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (0.14, 0.09, 0.06, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.08
    return mat


def append_female_body():
    objs_to_get = [
        "GEO-body_female_realistic",
        "GEO-body_female_realistic.eye.L",
        "GEO-body_female_realistic.eye.R",
    ]
    with bpy.data.libraries.load(str(BUNDLE), link=False) as (data_from, data_to):
        data_to.objects = [n for n in objs_to_get if n in data_from.objects]
    loaded = []
    for o in data_to.objects:
        if o is not None:
            bpy.context.scene.collection.objects.link(o)
            loaded.append(o)

    body = bpy.data.objects["GEO-body_female_realistic"]
    # Clear pre-existing Multires modifier so Armature deforms & applies 100% cleanly!
    body.modifiers.clear()
    for o in loaded:
        o.modifiers.clear()
    bpy.context.view_layer.update()
    vs = [body.matrix_world @ v.co for v in body.data.vertices]
    cx = 0.5 * (min(p.x for p in vs) + max(p.x for p in vs))
    cy = 0.5 * (min(p.y for p in vs) + max(p.y for p in vs))
    z0 = min(p.z for p in vs)
    shift = Vector((-cx, -cy, -z0))

    for o in loaded:
        o.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in loaded:
        for v in o.data.vertices:
            v.co += shift
        o.data.update()
        o.select_set(False)
    for p in body.data.polygons:
        p.use_smooth = True
    return body, [o for o in loaded if o != body]


def clean_cut_garment(body, name, cut_planes, face_keep_fn, offset=0.008, thickness=0.009, smooth_iters=4, mat=None):
    """Copy body mesh, slice along exact smooth mathematical cut planes with bmesh.ops.bisect_plane
    so collars, armholes, waistlines, and hems are RAZOR-CLEAN tailored seams (zero jagged quads!)."""
    bm = bmesh.new()
    bm.from_mesh(body.data)
    bm.verts.ensure_lookup_table()
    bm.faces.ensure_lookup_table()

    # Slice cleanly along each tailoring plane
    for pt, normal in cut_planes:
        geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=Vector(pt), plane_no=Vector(normal), clear_inner=False, clear_outer=False)

    bm.faces.ensure_lookup_table()
    del_faces = [f for f in bm.faces if not face_keep_fn(f.calc_center_median())]
    bmesh.ops.delete(bm, geom=del_faces, context="FACES")

    # Remove loose verts/edges
    loose_v = [v for v in bm.verts if not v.link_faces]
    if loose_v:
        bmesh.ops.delete(bm, geom=loose_v, context="VERTS")

    # Merge tiny sliver vertices created by bisect_plane before solidify (eliminates all spikes!)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=0.0025)

    bm.normal_update()
    # Push cleanly outward along surface normal
    for v in bm.verts:
        v.co += v.normal * offset

    # Smooth boundary seam vertices gently so armholes & collars look softly tailored
    for _ in range(5):
        b_verts = [v for v in bm.verts if v.is_boundary]
        if b_verts:
            bmesh.ops.smooth_vert(bm, verts=b_verts, factor=0.45, use_axis_x=True, use_axis_y=True, use_axis_z=True)

    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)

    if thickness > 0.0:
        sol = obj.modifiers.new("Solidify", "SOLIDIFY")
        sol.thickness = thickness
        sol.offset = 1.0
        sol.use_even_offset = False
        sol.thickness_clamp = 1.5
    if smooth_iters > 0:
        sm = obj.modifiers.new("Smooth", "SMOOTH")
        sm.factor = 0.35
        sm.iterations = smooth_iters
    sub = obj.modifiers.new("Subsurf", "SUBSURF")
    sub.levels = 1
    sub.render_levels = 1

    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    for p in obj.data.polygons:
        p.use_smooth = True
    obj.select_set(False)
    if mat is not None:
        obj.data.materials.append(mat)
    return obj


def build_clothing_in_rest_pose(body, mats):
    """Build crisp, cleanly-tailored Echo-style desert adventurer garments with exact bisected seams."""
    # 1. Pale cream inner sleeveless tunic (snug anatomical fit, peeks cleanly at collar & armholes)
    shirt = clean_cut_garment(
        body, "Hero_LinenShirt",
        cut_planes=[
            ((0, 0, 0.87), (0, 0, 1)),
            ((0, 0, 1.375), (0, 0, 1)),
            ((0.155, 0, 1.24), (1, 0, -0.15)),
            ((-0.155, 0, 1.24), (-1, 0, -0.15)),
        ],
        face_keep_fn=lambda c: (0.87 < c.z < 1.375 and abs(c.x) < 0.152),
        offset=0.0055, thickness=0.0055, smooth_iters=4, mat=mats["linen"]
    )

    # 2. Dark weathered leather adventurer sleeveless vest / cuirass (trimmed slightly narrower at shoulder straps)
    vest = clean_cut_garment(
        body, "Hero_LeatherVest",
        cut_planes=[
            ((0, 0, 0.89), (0, 0, 1)),
            ((0, 0, 1.360), (0, 0, 1)),
            ((0.140, 0, 1.22), (1, 0, -0.18)),
            ((-0.140, 0, 1.22), (-1, 0, -0.18)),
        ],
        face_keep_fn=lambda c: (0.89 < c.z < 1.360 and abs(c.x) < (0.132 if c.z > 1.22 else 0.158)),
        offset=0.0115, thickness=0.010, smooth_iters=4, mat=mats["leather_dark"]
    )

    # 3. Dark weathered riding trousers (clean cut waist -> ankle, generous offset for deep seated knee bend!)
    pants = clean_cut_garment(
        body, "Hero_Trousers",
        cut_planes=[
            ((0, 0, 0.94), (0, 0, 1)),
            ((0, 0, 0.14), (0, 0, 1)),
        ],
        face_keep_fn=lambda c: (0.14 < c.z < 0.94 and abs(c.x) < 0.24),
        offset=0.014, thickness=0.011, smooth_iters=4, mat=mats["trousers"]
    )

    # 4. Rugged leather desert boots (clean mid-calf top seam -> sole)
    boots = clean_cut_garment(
        body, "Hero_Boots",
        cut_planes=[
            ((0, 0, 0.31), (0, 0, 1)),
        ],
        face_keep_fn=lambda c: (c.z < 0.31 and abs(c.x) < 0.24),
        offset=0.013, thickness=0.010, smooth_iters=3, mat=mats["leather_dark"]
    )

    # 5. Tailored waist utility belt (crisp top & bottom straight cuts)
    belt = clean_cut_garment(
        body, "Hero_Belt",
        cut_planes=[
            ((0, 0, 0.885), (0, 0, 1)),
            ((0, 0, 0.960), (0, 0, 1)),
        ],
        face_keep_fn=lambda c: (0.885 < c.z < 0.960 and abs(c.x) < 0.20),
        offset=0.022, thickness=0.012, smooth_iters=2, mat=mats["leather_warm"]
    )

    # 6. Forearm leather bracers (crisp cuts perpendicular to forearm axis)
    bracers = clean_cut_garment(
        body, "Hero_Bracers",
        cut_planes=[
            ((0.275, 0.02, 1.01), (0.38, -0.18, -0.90)),
            ((0.335, 0.00, 0.88), (0.38, -0.18, -0.90)),
            ((-0.275, 0.02, 1.01), (-0.38, -0.18, -0.90)),
            ((-0.335, 0.00, 0.88), (-0.38, -0.18, -0.90)),
        ],
        face_keep_fn=lambda c: (0.88 < c.z < 1.02 and 0.270 < abs(c.x) < 0.355),
        offset=0.007, thickness=0.008, smooth_iters=2, mat=mats["leather_warm"]
    )
    return [shirt, vest, pants, boots, belt, bracers]


# Exact anatomical skeleton matching GEO-body_female_realistic
BONE_DEFS = [
    # (name, head, tail, parent, connected, radius_falloff, limb_group)
    ("pelvis",      (0.0,    0.018, 0.84), (0.0,    0.015, 0.95), None,         False, 0.14, "torso"),
    ("spine1",      (0.0,    0.015, 0.95), (0.0,    0.015, 1.12), "pelvis",     True,  0.14, "torso"),
    ("spine2",      (0.0,    0.015, 1.12), (0.0,    0.010, 1.32), "spine1",     True,  0.15, "torso"),
    ("neck",        (0.0,    0.010, 1.32), (0.0,   -0.012, 1.43), "spine2",     True,  0.08, "torso"),
    ("head",        (0.0,   -0.012, 1.43), (0.0,   -0.035, 1.64), "neck",       True,  0.13, "head"),

    ("clavicle.L",  (0.04,   0.012, 1.31), (0.165,  0.025, 1.32), "spine2",     False, 0.08, "arm_L"),
    ("upper_arm.L", (0.165,  0.025, 1.32), (0.252,  0.048, 1.08), "clavicle.L", True,  0.075,"arm_L"),
    ("forearm.L",   (0.252,  0.048, 1.08), (0.344, -0.004, 0.86), "upper_arm.L",True,  0.065,"arm_L"),
    ("hand.L",      (0.344, -0.004, 0.86), (0.402, -0.046, 0.74), "forearm.L",  True,  0.070,"arm_L"),

    ("clavicle.R",  (-0.04,  0.012, 1.31), (-0.165, 0.025, 1.32), "spine2",     False, 0.08, "arm_R"),
    ("upper_arm.R", (-0.165, 0.025, 1.32), (-0.252, 0.048, 1.08), "clavicle.R", True,  0.075,"arm_R"),
    ("forearm.R",   (-0.252, 0.048, 1.08), (-0.344,-0.004, 0.86), "upper_arm.R",True,  0.065,"arm_R"),
    ("hand.R",      (-0.344,-0.004, 0.86), (-0.402,-0.046, 0.74), "forearm.R",  True,  0.070,"arm_R"),

    ("thigh.L",     (0.105,  0.008, 0.82), (0.112,  0.032, 0.44), "pelvis",     False, 0.11, "leg_L"),
    ("shin.L",      (0.112,  0.032, 0.44), (0.122,  0.052, 0.08), "thigh.L",    True,  0.085,"leg_L"),
    ("foot.L",      (0.122,  0.052, 0.08), (0.145, -0.095, 0.02), "shin.L",     True,  0.085,"leg_L"),

    ("thigh.R",     (-0.105, 0.008, 0.82), (-0.112, 0.032, 0.44), "pelvis",     False, 0.11, "leg_R"),
    ("shin.R",      (-0.112, 0.032, 0.44), (-0.122, 0.052, 0.08), "thigh.R",    True,  0.085,"leg_R"),
    ("foot.R",      (-0.122, 0.052, 0.08), (-0.145,-0.095, 0.02), "shin.R",     True,  0.085,"leg_R"),
]


def build_armature():
    arm_data = bpy.data.armatures.new("HeroArmature")
    arm_obj = bpy.data.objects.new("HeroArmature", arm_data)
    bpy.context.scene.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    for name, head, tail, parent, connected, _rad, _grp in BONE_DEFS:
        b = eb.new(name)
        b.head = Vector(head)
        b.tail = Vector(tail)
        if parent is not None:
            b.parent = eb[parent]
            b.use_connect = connected
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm_obj


def point_seg_dist(p: Vector, a: Vector, b: Vector) -> float:
    ab = b - a
    l2 = ab.length_squared
    if l2 < 1e-8:
        return (p - a).length
    t = max(0.0, min(1.0, (p - a).dot(ab) / l2))
    return (p - (a + ab * t)).length


def assign_deterministic_weights(arm_obj, meshes, eyes):
    """Assign smooth, topologically-aware capsule-blended skinning weights directly in Python!
    100% immune to Blender bone-heat failures and ensures body + garments share identical deformation."""
    seg_info = [(name, Vector(h), Vector(t), rad, grp) for (name, h, t, _p, _c, rad, grp) in BONE_DEFS]

    for obj in meshes:
        obj.vertex_groups.clear()
        vgs = {name: obj.vertex_groups.new(name=name) for (name, *_rest) in BONE_DEFS}
        mod = obj.modifiers.new("Armature", "ARMATURE")
        mod.object = arm_obj
        mod.use_deform_preserve_volume = True  # Dual-quaternion skinning -> smooth elbows/knees/shoulders!

        for v in obj.data.vertices:
            p = v.co
            # Determine allowed anatomical groups to prevent cross-bleed (e.g. arms never pull hips; left leg never pulls right leg!)
            in_arm_L = (p.x > 0.165 and p.z > 0.65 and p.z < 1.40) or (p.x > 0.19 and p.z > 0.60)
            in_arm_R = (p.x < -0.165 and p.z > 0.65 and p.z < 1.40) or (p.x < -0.19 and p.z > 0.60)
            in_head = (p.z > 1.425)
            in_leg_L = (p.z < 0.785 and p.x >= 0.0 and not in_arm_L)
            in_leg_R = (p.z < 0.785 and p.x < 0.0 and not in_arm_R)

            weights = []
            for name, a, b, rad, grp in seg_info:
                if in_head and grp not in ("head", "torso"):
                    continue
                if in_arm_L and grp not in ("arm_L", "torso"):
                    continue
                if in_arm_R and grp not in ("arm_R", "torso"):
                    continue
                if in_leg_L and grp not in ("leg_L", "torso"):
                    continue
                if in_leg_R and grp not in ("leg_R", "torso"):
                    continue
                if not in_arm_L and not in_arm_R and p.z > 0.785 and grp in ("leg_L", "leg_R") and p.z > 0.89:
                    continue
                # Prevent arm bones from grabbing torso waist/chest verts inside |x| < 0.135
                if grp in ("arm_L", "arm_R") and abs(p.x) < 0.135 and name != f"clavicle.{'L' if grp=='arm_L' else 'R'}":
                    continue
                # Prevent opposite-side limb bleed
                if grp.endswith("_L") and p.x < -0.015:
                    continue
                if grp.endswith("_R") and p.x > 0.015:
                    continue

                d = point_seg_dist(p, a, b)
                # Sharp smooth inverse-quartic/Gaussian kernel
                w = math.exp(-((d / (rad * 0.62)) ** 2.3))
                if w > 1e-4:
                    weights.append((name, w))

            if not weights:
                # Fallback to nearest bone
                best_n = min(seg_info, key=lambda s: point_seg_dist(p, s[1], s[2]))[0]
                vgs[best_n].add([v.index], 1.0, "REPLACE")
            else:
                weights.sort(key=lambda x: -x[1])
                top = weights[:4]
                tot = sum(w for _, w in top)
                for name, w in top:
                    vgs[name].add([v.index], w / tot, "REPLACE")

    # Parent eyes cleanly to head bone
    for e in eyes:
        e.parent = arm_obj
        e.parent_type = "BONE"
        e.parent_bone = "head"
        b = arm_obj.data.bones["head"]
        e.matrix_parent_inverse = (arm_obj.matrix_world @ Matrix.Translation(b.tail_local)).inverted()


def apply_pose(arm_obj, pose_name: str):
    """Pose bones cleanly in WORLD coordinates using quaternion delta rotations (100% predictable
    regardless of individual bone roll/orientation!). Remember character front = -Y, up = +Z, left = +X."""
    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="POSE")
    pb = arm_obj.pose.bones
    for b in pb:
        b.rotation_mode = "QUATERNION"

    def rot_world(name, wx_deg=0.0, wy_deg=0.0, wz_deg=0.0):
        """Apply a world-space XYZ Euler rotation to pose bone `name` around its head pivot."""
        bpy.context.view_layer.update()
        pbone = pb[name]
        # Current bone matrix in armature space
        m_cur = pbone.matrix.copy()
        head = m_cur.translation.copy()
        r_world = Euler((math.radians(wx_deg), math.radians(wy_deg), math.radians(wz_deg)), "XYZ").to_matrix().to_4x4()
        m_new = Matrix.Translation(head) @ r_world @ Matrix.Translation(-head) @ m_cur
        pbone.matrix = m_new
        bpy.context.view_layer.update()

    if pose_name == "overlook":
        # Natural relaxed standing contrapposto looking slightly right over the sunset canyon
        rot_world("spine1", wx_deg=2.0, wy_deg=1.0, wz_deg=-3.0)
        rot_world("spine2", wx_deg=-2.0, wy_deg=1.0, wz_deg=-5.0)
        rot_world("neck",   wx_deg=1.0, wy_deg=-1.5, wz_deg=-7.0)
        rot_world("head",   wx_deg=-3.0, wy_deg=-2.0, wz_deg=-10.0)
        # Drop A-pose arms comfortably straight down by her sides!
        # Left arm (+X) rotates AROUND +Y negatively (wy=-16) to move down toward -X/body; Right arm (-X) wy=+16!
        rot_world("upper_arm.L", wx_deg=-4.0, wy_deg=16.0, wz_deg=0.0)
        rot_world("forearm.L",   wx_deg=-14.0, wy_deg=2.0, wz_deg=0.0)
        rot_world("upper_arm.R", wx_deg=-4.0, wy_deg=-16.0, wz_deg=0.0)
        rot_world("forearm.R",   wx_deg=-16.0, wy_deg=-2.0, wz_deg=0.0)
        rot_world("thigh.L",     wx_deg=3.0, wy_deg=0.0, wz_deg=2.0)
        rot_world("shin.L",      wx_deg=-4.0, wy_deg=0.0, wz_deg=0.0)
        rot_world("thigh.R",     wx_deg=-4.0, wy_deg=0.0, wz_deg=-3.0)
        rot_world("shin.R",      wx_deg=6.0, wy_deg=0.0, wz_deg=0.0)
    elif pose_name == "campfire":
        # Seated calmly cross-legged on desert sandstone facing -Y toward the crackling campfire!
        rot_world("pelvis", wx_deg=6.0, wy_deg=0.0, wz_deg=0.0)
        rot_world("spine1", wx_deg=-8.0, wy_deg=0.0, wz_deg=-2.0)
        rot_world("spine2", wx_deg=-7.0, wy_deg=1.0, wz_deg=-3.0)
        rot_world("neck",   wx_deg=-5.0, wy_deg=0.0, wz_deg=3.0)
        rot_world("head",   wx_deg=-9.0, wy_deg=-2.0, wz_deg=5.0)

        # Cross-legged seated pose so thighs, knees, and boots all rest cleanly at ground level!
        rot_world("thigh.L", wx_deg=-86.0, wy_deg=10.0, wz_deg=34.0)
        rot_world("shin.L",  wx_deg=32.0,  wy_deg=-10.0, wz_deg=-95.0)
        rot_world("foot.L",  wx_deg=10.0,  wy_deg=0.0, wz_deg=0.0)

        rot_world("thigh.R", wx_deg=-86.0, wy_deg=-10.0, wz_deg=-34.0)
        rot_world("shin.R",  wx_deg=36.0,  wy_deg=10.0, wz_deg=95.0)
        rot_world("foot.R",  wx_deg=10.0,  wy_deg=0.0, wz_deg=0.0)

        # Relaxed arms lowered comfortably toward knees & warming palms gently at campfire height
        rot_world("upper_arm.L", wx_deg=-24.0, wy_deg=18.0, wz_deg=-12.0)
        rot_world("forearm.L",   wx_deg=-24.0, wy_deg=0.0, wz_deg=-28.0)
        rot_world("hand.L",      wx_deg=10.0,  wy_deg=0.0, wz_deg=0.0)

        rot_world("upper_arm.R", wx_deg=-26.0, wy_deg=-18.0, wz_deg=12.0)
        rot_world("forearm.R",   wx_deg=-26.0, wy_deg=0.0, wz_deg=28.0)
        rot_world("hand.R",      wx_deg=10.0,  wy_deg=0.0, wz_deg=0.0)

    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.context.view_layer.update()


def bake_armature_and_ground(arm_obj, meshes, eyes):
    eye_mats = [e.matrix_world.copy() for e in eyes]
    for e, m in zip(eyes, eye_mats):
        e.parent = None
        e.matrix_world = m

    for o in meshes:
        bpy.ops.object.select_all(action="DESELECT")
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
        for mod in list(o.modifiers):
            if mod.type == "ARMATURE":
                bpy.ops.object.modifier_apply(modifier=mod.name)
        o.parent = None
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    bpy.data.objects.remove(arm_obj, do_unlink=True)

    all_objs = meshes + eyes
    min_z = min(min((o.matrix_world @ v.co).z for v in o.data.vertices) for o in all_objs)
    for o in all_objs:
        for v in o.data.vertices:
            v.co.z -= min_z
        o.data.update()


def find_posed_landmarks(body):
    vs = [v.co for v in body.data.vertices]
    max_z = max(p.z for p in vs)
    head_pts = [p for p in vs if p.z > max_z - 0.19]
    head_c = sum(head_pts, Vector((0, 0, 0))) / max(1, len(head_pts))
    neck_pts = [p for p in vs if (max_z - 0.27) < p.z < (max_z - 0.21) and (p - head_c).xy.length < 0.095]
    neck_c = sum(neck_pts, Vector((0, 0, 0))) / max(1, len(neck_pts)) if neck_pts else (head_c - Vector((0, 0, 0.14)))
    mid_head = [p for p in head_pts if (max_z - 0.15) < p.z < (max_z - 0.07)]
    nose = min(mid_head, key=lambda p: p.y) if mid_head else (head_c + Vector((0, -0.10, -0.03)))
    fwd = Vector((nose.x - head_c.x, nose.y - head_c.y, 0.0))
    if fwd.length < 1e-4:
        fwd = Vector((0.0, -1.0, 0.0))
    fwd.normalize()
    right = Vector((fwd.y, -fwd.x, 0.0)).normalized()
    return {"max_z": max_z, "head_c": head_c, "neck_c": neck_c, "fwd": fwd, "right": right}


def add_swept_strand_to_bm(bm, uv_layer, pts, radii, aspect=0.55, segs=8):
    """Build a 100% bounded, smooth elliptic hair strand directly in bmesh (zero NURBS spikes!)."""
    rings = []
    n = len(pts)
    for i in range(n):
        p = pts[i]
        if i == 0:
            t = (pts[1] - pts[0]).normalized()
        elif i == n - 1:
            t = (pts[-1] - pts[-2]).normalized()
        else:
            t = (pts[i + 1] - pts[i - 1]).normalized()
        ref = Vector((0, 0, 1)) if abs(t.z) < 0.92 else Vector((1, 0, 0))
        u = t.cross(ref).normalized()
        v = t.cross(u).normalized()
        r = radii[i]
        ring = []
        for s in range(segs):
            th = (s / float(segs)) * math.tau
            off = u * (math.cos(th) * r) + v * (math.sin(th) * r * aspect)
            ring.append(bm.verts.new(p + off))
        rings.append(ring)

    for i in range(n - 1):
        v0 = i / float(n - 1)
        v1 = (i + 1) / float(n - 1)
        for s in range(segs):
            sn = (s + 1) % segs
            f = bm.faces.new([rings[i][s], rings[i][sn], rings[i + 1][sn], rings[i + 1][s]])
            f.smooth = True
            u0 = s / float(segs)
            u1 = (s + 1) / float(segs)
            for lp, uv in zip(f.loops, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]):
                lp[uv_layer].uv = uv


def eval_catmull(pts, steps=12):
    out = []
    for k in range(steps + 1):
        t = k / float(steps)
        s = t * (len(pts) - 1)
        i = min(int(s), len(pts) - 2)
        f = s - i
        p0 = pts[max(0, i - 1)]
        p1 = pts[i]
        p2 = pts[i + 1]
        p3 = pts[min(len(pts) - 1, i + 2)]
        out.append(0.5 * (
            2.0 * p1 + (-p0 + p2) * f
            + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * f * f
            + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * f * f * f
        ))
    return out


def build_sculpted_hair_bun(body, lm, hair_mat):
    """Build rich, organic swept-up dark hair wrapping 100% of the scalp surface by sampling
    actual surface vertices of the posed scalp cap and sweeping thick overlapping locks up into
    a lush Echo-style high messy topknot bun!"""
    head_c = lm["head_c"]
    fwd = lm["fwd"]
    right = lm["right"]
    back = -fwd
    up = Vector((0.0, 0.0, 1.0))

    # 1. Cleanly trimmed scalp base cap hugging cranium (prevents any skin peeking between strands)
    cap = clean_cut_garment(
        body, "Hero_HairCap",
        cut_planes=[
            (head_c + Vector((0, 0, -0.055)), Vector((0, 0, 1))),
            (head_c + fwd * 0.015 + Vector((0, 0, 0.02)), back * 0.85 + up * 0.52),
        ],
        face_keep_fn=lambda c: (
            c.z > head_c.z - 0.060
            and ((c - head_c).dot(back) > -0.020 or c.z > lm["max_z"] - 0.068)
            and not ((c - head_c).dot(fwd) > 0.048 and c.z < lm["max_z"] - 0.058)
        ),
        offset=0.005, thickness=0.005, smooth_iters=3, mat=hair_mat
    )

    # Measure exact scalp cap center so strands hug 360 degrees symmetrically regardless of head turn!
    cap_vs = [v.co.copy() for v in cap.data.vertices]
    cap_ns = [v.normal.copy() for v in cap.data.vertices]
    cap_c = sum(cap_vs, Vector((0, 0, 0))) / max(1, len(cap_vs))
    cap_top_z = max(p.z for p in cap_vs)
    bun_c = Vector((cap_c.x, cap_c.y, cap_top_z - 0.015)) + back * 0.032 + up * 0.042

    bm = bmesh.new()
    uv_layer = bm.loops.layers.uv.new("UVMap")
    rng = random.Random(42)

    # Pick 160 well-distributed anchor vertices from the lower/outer scalp cap perimeter & body!
    # Every strand starts RIGHT ON the real scalp cap surface (+0.006 along normal), hugs the cranium
    # up to the crown, and wraps organically into the high bun -> ZERO bald helmet ever visible!
    candidates = [(p, n) for (p, n) in zip(cap_vs, cap_ns) if n.dot(p - cap_c) > 0.0]
    rng.shuffle(candidates)
    # Sort into angular buckets around the head so coverage is 100% uniform 360 deg!
    buckets = [[] for _ in range(72)]
    for p, n in candidates:
        rel = p - cap_c
        ang = math.atan2(rel.dot(right), rel.dot(back))
        bi = int(((ang + math.pi) / math.tau) * 72.0) % 72
        buckets[bi].append((p, n))

    for bi in range(72):
        pts_in_b = buckets[bi]
        if not pts_in_b:
            continue
        # Full continuous nape-to-crown strand coverage (every vertical band covered cleanly!)
        pts_in_b.sort(key=lambda pn: pn[0].z)
        chosen = [pts_in_b[0]]
        for frac in (0.20, 0.42, 0.65):
            idx = min(len(pts_in_b) - 1, int(len(pts_in_b) * frac))
            chosen.append(pts_in_b[idx])

        for p_start, n_start in chosen:
            p0 = p_start + n_start.normalized() * 0.009
            rel = (p0 - cap_c)
            rad_xy = Vector((rel.x, rel.y, 0.0))
            r_len = max(0.03, rad_xy.length)
            r_dir = rad_xy / r_len
            # Smoothly hug outer scalp contour right up into the high bun (zero gaps, zero smooth bald patch!)
            p1 = Vector((cap_c.x, cap_c.y, 0.48 * p0.z + 0.52 * cap_top_z)) + r_dir * (r_len + 0.010) + up * 0.005
            p2 = bun_c + r_dir * 0.022 + up * rng.uniform(-0.012, 0.016)
            ang = math.atan2(r_dir.dot(right), r_dir.dot(back)) + rng.uniform(0.9, 1.8)
            c_dir = (back * math.cos(ang) + right * math.sin(ang)).normalized()
            p3 = bun_c + c_dir * rng.uniform(0.032, 0.050) + up * rng.uniform(0.010, 0.050)
            p4 = bun_c + c_dir * rng.uniform(0.012, 0.028) + up * rng.uniform(0.030, 0.064)

            curve_pts = eval_catmull([p0, p1, p2, p3, p4], steps=11)
            base_r = rng.uniform(0.010, 0.015)
            rads = [base_r * (0.65 + 0.45 * math.sin((k / 11.0) * math.pi)) for k in range(12)]
            rads[0] *= 0.72
            rads[-1] = 0.0025
            add_swept_strand_to_bm(bm, uv_layer, curve_pts, rads, aspect=0.48, segs=6)

    # 34 chunky sculpted high-bun coils + organic tousled crown loops
    for j in range(34):
        t0 = (j / 34.0) * math.tau + rng.uniform(-0.1, 0.1)
        r_bun = rng.uniform(0.024, 0.054)
        z_lo = rng.uniform(-0.022, 0.026)
        z_hi = z_lo + rng.uniform(0.030, 0.064)
        ctrl = []
        for k in range(5):
            a = t0 + k * 0.85
            frac = k / 4.0
            rr = r_bun * (0.75 + 0.35 * math.sin(frac * math.pi))
            zz = z_lo + (z_hi - z_lo) * frac + 0.009 * math.sin(k * 1.4)
            ctrl.append(bun_c + (back * math.cos(a) + right * math.sin(a)) * rr + up * zz)
        curve_pts = eval_catmull(ctrl, steps=10)
        base_r = rng.uniform(0.012, 0.018)
        rads = [base_r * (0.45 + 0.65 * math.sin((k / 10.0) * math.pi)) for k in range(11)]
        add_swept_strand_to_bm(bm, uv_layer, curve_pts, rads, aspect=0.62, segs=7)

    me_strands = bpy.data.meshes.new("Hero_HairStrands")
    bm.to_mesh(me_strands)
    bm.free()
    strands = bpy.data.objects.new("Hero_HairStrands", me_strands)
    bpy.context.scene.collection.objects.link(strands)
    strands.data.materials.append(hair_mat)
    return [cap, strands]


def build_ochre_scarf(lm, pose_name, scarf_mat):
    """Build Echo's iconic golden-ochre woven scarf: soft layered neck wrap + gently billowing back-left tail."""
    neck_c = lm["neck_c"]
    fwd = lm["fwd"]
    back = -fwd
    right = lm["right"]
    left = -right
    up = Vector((0.0, 0.0, 1.0))

    # 1. Layered wrapped neck cowl with crisp sculpted cloth folds
    bm = bmesh.new()
    segs_u = 48
    segs_v = 16
    rings = []
    for iv in range(segs_v + 1):
        v = iv / float(segs_v)
        z = neck_c.z - 0.068 + v * 0.102
        r_base = 0.092 - 0.020 * v + 0.008 * math.sin(v * math.pi * 2.6)
        ring = []
        for iu in range(segs_u):
            th = (iu / float(segs_u)) * math.tau
            fold = 0.0065 * math.sin(th * 6.0 + v * 5.0) + 0.0035 * math.cos(th * 11.0 - v * 3.5)
            drop = -0.016 * (1.0 - v) * max(0.0, math.cos(th - 0.5))
            rx = r_base * 1.04 + fold
            ry = r_base * 1.10 + fold
            p = Vector((neck_c.x, neck_c.y, z + drop)) + right * (rx * math.cos(th)) + back * (ry * math.sin(th))
            ring.append(bm.verts.new(p))
        rings.append(ring)

    uv_layer = bm.loops.layers.uv.new("UVMap")
    for iv in range(segs_v):
        for iu in range(segs_u):
            inext = (iu + 1) % segs_u
            f = bm.faces.new([rings[iv][iu], rings[iv][inext], rings[iv + 1][inext], rings[iv + 1][iu]])
            f.smooth = True
            for loop, (uu, vv) in zip(f.loops, [(iu / segs_u, iv / segs_v), ((iu + 1) / segs_u, iv / segs_v), ((iu + 1) / segs_u, (iv + 1) / segs_v), (iu / segs_u, (iv + 1) / segs_v)]):
                loop[uv_layer].uv = (uu * 3.0, vv * 1.2)

    me_cowl = bpy.data.meshes.new("Hero_ScarfCowl")
    bm.to_mesh(me_cowl)
    bm.free()
    cowl = bpy.data.objects.new("Hero_ScarfCowl", me_cowl)
    bpy.context.scene.collection.objects.link(cowl)
    sol = cowl.modifiers.new("Solidify", "SOLIDIFY")
    sol.thickness = 0.011
    sol.offset = 1.0
    sub = cowl.modifiers.new("Subsurf", "SUBSURF")
    sub.levels = 1
    sub.render_levels = 1
    bpy.context.view_layer.objects.active = cowl
    cowl.select_set(True)
    for m in list(cowl.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    cowl.data.materials.append(scarf_mat)
    cowl.select_set(False)

    # 2. Flowing cloth ribbon tail draping organically over left shoulder & down back
    bm2 = bmesh.new()
    nu = 36
    nv = 10
    if pose_name == "overlook":
        spine_pts = [
            neck_c + left * 0.105 + back * 0.095 - up * 0.008,
            neck_c + left * 0.195 + back * 0.145 - up * 0.080,
            neck_c + left * 0.245 + back * 0.185 - up * 0.220,
            neck_c + left * 0.270 + back * 0.215 - up * 0.390,
            neck_c + left * 0.295 + back * 0.245 - up * 0.590,
            neck_c + left * 0.320 + back * 0.275 - up * 0.780,
        ]
    else:
        spine_pts = [
            neck_c + left * 0.035 + back * 0.095 - up * 0.012,
            neck_c + left * 0.025 + back * 0.175 - up * 0.115,
            neck_c + left * 0.020 + back * 0.235 - up * 0.265,
            neck_c + left * 0.025 + back * 0.285 - up * 0.435,
            neck_c + left * 0.045 + back * 0.340 - up * 0.590,
            Vector((neck_c.x + 0.05, neck_c.y + 0.40, 0.030)),
        ]

    dense_spine = eval_catmull(spine_pts, steps=nu)
    grid = []
    for iu in range(nu + 1):
        u = iu / float(nu)
        c = dense_spine[iu]
        width = 0.062 + 0.038 * math.sin(u * math.pi * 0.85)
        twist = 0.34 * math.sin(u * 4.5 + 0.4)
        wdir = (right * math.cos(twist) + back * math.sin(twist)).normalized()
        ndir = (back * math.cos(twist) - right * math.sin(twist)).normalized()
        row = []
        for iv in range(nv + 1):
            v = (iv / float(nv)) * 2.0 - 1.0
            ripple = 0.014 * math.sin(u * 14.0 + v * 2.4) * u + 0.010 * math.cos(v * 3.14)
            p = c + wdir * (v * width) + ndir * ripple
            if p.z < 0.025:
                p.z = 0.025 + 0.005 * abs(v)
            row.append(bm2.verts.new(p))
        grid.append(row)

    uv2 = bm2.loops.layers.uv.new("UVMap")
    for iu in range(nu):
        for iv in range(nv):
            f = bm2.faces.new([grid[iu][iv], grid[iu][iv + 1], grid[iu + 1][iv + 1], grid[iu + 1][iv]])
            f.smooth = True
            for loop, (uu, vv) in zip(f.loops, [(iu / nu, iv / nv), (iu / nu, (iv + 1) / nv), ((iu + 1) / nu, (iv + 1) / nv), ((iu + 1) / nu, iv / nv)]):
                loop[uv2].uv = (vv * 1.0, uu * 4.0)

    me_tail = bpy.data.meshes.new("Hero_ScarfTail")
    bm2.to_mesh(me_tail)
    bm2.free()
    tail = bpy.data.objects.new("Hero_ScarfTail", me_tail)
    bpy.context.scene.collection.objects.link(tail)
    sol2 = tail.modifiers.new("Solidify", "SOLIDIFY")
    sol2.thickness = 0.007
    sub2 = tail.modifiers.new("Subsurf", "SUBSURF")
    sub2.levels = 1
    sub2.render_levels = 1
    bpy.context.view_layer.objects.active = tail
    tail.select_set(True)
    for m in list(tail.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    tail.data.materials.append(scarf_mat)
    tail.select_set(False)
    return [cowl, tail]


def build_character_pose(pose_name: str, out_glb: Path):
    clear_scene()
    mats = {
        "skin": make_skin_material(),
        "hair": make_hair_material(),
        "eye": make_eye_material(),
        "linen": make_pbr_tex_material("Hero_LinenShirt", "rough_linen", tint=(0.88, 0.82, 0.72), uv_scale=7.0, rough_val=0.84, normal_strength=0.60),
        "leather_dark": make_pbr_tex_material("Hero_LeatherVest", "brown_leather", tint=(0.34, 0.23, 0.16), uv_scale=4.5, rough_val=0.62, normal_strength=0.80),
        "leather_warm": make_pbr_tex_material("Hero_LeatherBelt", "brown_leather", tint=(0.54, 0.33, 0.18), uv_scale=5.5, rough_val=0.56, normal_strength=0.85),
        "trousers": make_pbr_tex_material("Hero_Trousers", "rough_linen", tint=(0.24, 0.21, 0.18), uv_scale=6.0, rough_val=0.88, normal_strength=0.65),
        "scarf": make_pbr_tex_material("Hero_OchreScarf", "hessian_230", tint=(0.88, 0.62, 0.19), uv_scale=6.5, rough_val=0.88, normal_strength=0.85),
    }

    body, eyes = append_female_body()
    body.data.materials.clear()
    body.data.materials.append(mats["skin"])
    for e in eyes:
        e.data.materials.clear()
        e.data.materials.append(mats["eye"])

    garments = build_clothing_in_rest_pose(body, mats)
    arm_obj = build_armature()
    all_deform = [body] + garments
    assign_deterministic_weights(arm_obj, all_deform, eyes)
    apply_pose(arm_obj, pose_name)
    bake_armature_and_ground(arm_obj, all_deform, eyes)

    # Smooth subdivide body once AFTER posing for crisp 4K shoulders/arms/neck/ears
    sub = body.modifiers.new("Subsurf", "SUBSURF")
    sub.levels = 1
    sub.render_levels = 1
    bpy.context.view_layer.objects.active = body
    body.select_set(True)
    bpy.ops.object.modifier_apply(modifier=sub.name)
    body.select_set(False)

    lm = find_posed_landmarks(body)
    build_sculpted_hair_bun(body, lm, mats["hair"])
    build_ochre_scarf(lm, pose_name, mats["scarf"])

    all_meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    total_polys = sum(len(o.data.polygons) for o in all_meshes)
    print(f"[{pose_name}] meshes={len(all_meshes)} total_polys={total_polys:,} max_z={lm['max_z']:.3f}", flush=True)

    bpy.ops.export_scene.gltf(
        filepath=str(out_glb),
        export_format="GLB",
        export_texcoords=True,
        export_normals=True,
        export_materials="EXPORT",
        export_image_format="JPEG",
        export_image_quality=90,
    )
    print(f"[{pose_name}] exported -> {out_glb} ({out_glb.stat().st_size / (1024*1024):.1f} MB)", flush=True)

    # Render full verification preview
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 960
    scene.render.resolution_y = 680
    scene.render.filepath = str(OUT_DIR / f"preview_{pose_name}.png")
    world = bpy.data.worlds.new("PreviewWorld")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (0.14, 0.16, 0.22, 1.0)
    bg.inputs["Strength"].default_value = 0.85
    scene.world = world

    sun_d = bpy.data.lights.new("Sun", "SUN")
    sun_d.energy = 4.5
    sun_d.color = (1.0, 0.84, 0.66)
    sun_o = bpy.data.objects.new("Sun", sun_d)
    sun_o.rotation_euler = Euler((math.radians(58), 0.0, math.radians(-130)), "XYZ")
    scene.collection.objects.link(sun_o)

    cam_d = bpy.data.cameras.new("Cam")
    cam_d.lens = 45.0
    cam_o = bpy.data.objects.new("Cam", cam_d)
    scene.collection.objects.link(cam_o)
    if pose_name == "overlook":
        cam_o.location = Vector((-0.55, 1.35, 1.48))
        tgt = Vector((0.0, 0.0, 1.32))
    else:
        cam_o.location = Vector((-1.75, 1.45, 1.05))
        tgt = Vector((0.0, -0.05, 0.55))
    cam_o.rotation_euler = (tgt - cam_o.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam_o
    bpy.ops.render.render(write_still=True)


def main():
    build_character_pose("overlook", OUT_DIR / "hero_overlook.glb")
    build_character_pose("campfire", OUT_DIR / "hero_campfire.glb")


if __name__ == "__main__":
    main()
