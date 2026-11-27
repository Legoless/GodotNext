import csv
import json
import math
import os
import unreal

# Godot (+X right, +Y up, +Z back, meters) -> UE (+X right, +Y back, +Z up, cm)
# Calibrated from axes.glb: axis_x=(100,0,0), axis_y=(0,0,100), axis_z=(0,100,0).
def g2u_pos(x, y, z):
    return unreal.Vector(float(x) * 100.0, float(z) * 100.0, float(y) * 100.0)

def g2u_vec(x, y, z):
    return unreal.Vector(float(x), float(z), float(y))

def mat3x4_to_ue_transform(m):
    # m is 12 row-major floats: [m00,m01,m02,m03, m10,m11,m12,m13, m20,m21,m22,m23]
    # Godot world = R_g * p_g + t_g
    # UE world = S * R_g * S * p_ue + 100 * S * t_g, where S swaps Y and Z.
    # Columns of R_ue = S * R_g * S:
    #   col0 (UE X) = S * col0(R_g) = (m00, m20, m10)
    #   col1 (UE Y) = S * col2(R_g) = (m02, m22, m12)
    #   col2 (UE Z) = S * col1(R_g) = (m01, m21, m11)
    cx = unreal.Vector(m[0], m[8], m[4])
    cy = unreal.Vector(m[2], m[10], m[6])
    cz = unreal.Vector(m[1], m[9], m[5])
    sx = cx.length()
    sy = cy.length()
    sz = cz.length()
    if sx < 1e-6: sx = 1.0
    if sy < 1e-6: sy = 1.0
    if sz < 1e-6: sz = 1.0
    ux = cx / sx
    uy = cy / sy
    uz = cz / sz
    # Check handedness: in UE left-handed frame, det([ux, uy, uz]) should be +1 for proper rot
    det = ux.dot(uy.cross(uz))
    if det < 0.0:
        sz = -sz
        uz = -uz
    rot = unreal.MathLibrary.make_rot_from_xz(ux, uz)
    loc = g2u_pos(m[3], m[7], m[11])
    scale = unreal.Vector(sx, sy, sz)
    return unreal.Transform(loc, rot, scale)

def mat3x4_to_cam_light_loc_rot(m):
    # Godot cameras and lights look down local -Z with local +Y up.
    # UE cameras and lights look down local +X with local +Z up.
    fg = (-m[2], -m[6], -m[10])
    ug = (m[1], m[5], m[9])
    f_ue = g2u_vec(*fg)
    u_ue = g2u_vec(*ug)
    rot = unreal.MathLibrary.make_rot_from_xz(f_ue, u_ue)
    loc = g2u_pos(m[3], m[7], m[11])
    return loc, rot

def quat_to_cam_rot(qx, qy, qz, qw):
    # Rotate Godot local -Z (0,0,-1) and +Y (0,1,0) by quaternion (qx,qy,qz,qw)
    # Using standard quaternion rotation v' = v + 2*q.xyz x (q.xyz x v + qw*v)
    def qrot(vx, vy, vz):
        tx = 2.0 * (qy * vz - qz * vy)
        ty = 2.0 * (qz * vx - qx * vz)
        tz = 2.0 * (qx * vy - qy * vx)
        return (
            vx + qw * tx + (qy * tz - qz * ty),
            vy + qw * ty + (qz * tx - qx * tz),
            vz + qw * tz + (qx * ty - qy * tx),
        )
    fg = qrot(0.0, 0.0, -1.0)
    ug = qrot(0.0, 1.0, 0.0)
    return unreal.MathLibrary.make_rot_from_xz(g2u_vec(*fg), g2u_vec(*ug))

def get_or_create_master_material(blend, two_sided, has_tex):
    name = f"M_Base_{blend}_{'2S' if two_sided else '1S'}_{'Tex' if has_tex else 'Flat'}"
    pkg_path = f"/Game/Ports/_BaseMaterials/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(pkg_path):
        return unreal.EditorAssetLibrary.load_asset(pkg_path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat = asset_tools.create_asset(name, "/Game/Ports/_BaseMaterials", unreal.Material, unreal.MaterialFactoryNew())
    mel = unreal.MaterialEditingLibrary

    if blend == "masked":
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
    elif blend == "translucent":
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        tlm = getattr(unreal.TranslucencyLightingMode, "TLM_SURFACE", None) or getattr(unreal.TranslucencyLightingMode, "TLM_VOLUMETRIC_DIRECTIONAL", None)
        if tlm is not None:
            mat.set_editor_property("translucency_lighting_mode", tlm)
    else:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)

    mat.set_editor_property("two_sided", bool(two_sided))

    base_col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, -200)
    base_col.set_editor_property("parameter_name", "AlbedoColor")
    base_col.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    if has_tex:
        uv = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -900, 0)
        uv_scale = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, 150)
        uv_scale.set_editor_property("parameter_name", "UVScaleOffset")
        uv_scale.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 0.0, 0.0))
        mask_rg = mel.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -700, 120)
        mask_rg.set_editor_property("r", True); mask_rg.set_editor_property("g", True)
        mask_rg.set_editor_property("b", False); mask_rg.set_editor_property("a", False)
        mel.connect_material_expressions(uv_scale, "", mask_rg, "")
        mask_ba = mel.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -700, 220)
        mask_ba.set_editor_property("r", False); mask_ba.set_editor_property("g", False)
        mask_ba.set_editor_property("b", True); mask_ba.set_editor_property("a", True)
        mel.connect_material_expressions(uv_scale, "", mask_ba, "")
        mul_uv = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -550, 50)
        mel.connect_material_expressions(uv, "", mul_uv, "A")
        mel.connect_material_expressions(mask_rg, "", mul_uv, "B")
        add_uv = mel.create_material_expression(mat, unreal.MaterialExpressionAdd, -420, 80)
        mel.connect_material_expressions(mul_uv, "", add_uv, "A")
        mel.connect_material_expressions(mask_ba, "", add_uv, "B")

        tex_param = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -260, 0)
        tex_param.set_editor_property("parameter_name", "AlbedoTex")
        mel.connect_material_expressions(add_uv, "", tex_param, "UVs")

        mul_col = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -40, -120)
        mel.connect_material_expressions(base_col, "", mul_col, "A")
        mel.connect_material_expressions(tex_param, "RGB", mul_col, "B")
        mel.connect_material_property(mul_col, "", unreal.MaterialProperty.MP_BASE_COLOR)

        if blend in ("masked", "translucent"):
            mul_a = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -40, 80)
            mel.connect_material_expressions(base_col, "A", mul_a, "A")
            mel.connect_material_expressions(tex_param, "A", mul_a, "B")
            prop = unreal.MaterialProperty.MP_OPACITY_MASK if blend == "masked" else unreal.MaterialProperty.MP_OPACITY
            mel.connect_material_property(mul_a, "", prop)
    else:
        mel.connect_material_property(base_col, "", unreal.MaterialProperty.MP_BASE_COLOR)
        if blend in ("masked", "translucent"):
            prop = unreal.MaterialProperty.MP_OPACITY_MASK if blend == "masked" else unreal.MaterialProperty.MP_OPACITY
            mel.connect_material_property(base_col, "A", prop)

    met = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -200, 260)
    met.set_editor_property("parameter_name", "Metallic")
    met.set_editor_property("default_value", 0.0)
    mel.connect_material_property(met, "", unreal.MaterialProperty.MP_METALLIC)

    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -200, 340)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.8)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    emis = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -200, 440)
    emis.set_editor_property("parameter_name", "EmissiveColor")
    emis.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    mel.connect_material_property(emis, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    try:
        mel.recompile_material(mat)
    except Exception:
        pass
    unreal.EditorAssetLibrary.save_asset(pkg_path)
    return mat

def import_file(src, dest_dir):
    if not os.path.isfile(src):
        return []
    task = unreal.AssetImportTask()
    task.filename = src
    task.destination_path = dest_dir
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return list(task.imported_object_paths)

def set_nanite_static(sm, enabled):
    ns = sm.get_editor_property("nanite_settings")
    if bool(ns.enabled) != bool(enabled):
        ns.enabled = bool(enabled)
        sm_sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        if sm_sub and hasattr(sm_sub, "set_nanite_settings"):
            sm_sub.set_nanite_settings(sm, ns, apply_changes=True)
        else:
            sm.set_editor_property("nanite_settings", ns)
        unreal.EditorAssetLibrary.save_loaded_asset(sm)

def set_nanite_skeletal(skm, enabled):
    try:
        ns = skm.get_editor_property("nanite_settings")
        if bool(ns.enabled) != bool(enabled):
            ns.enabled = bool(enabled)
            skm.set_editor_property("nanite_settings", ns)
            unreal.EditorAssetLibrary.save_loaded_asset(skm)
    except Exception as e:
        unreal.log_warning(f"Skeletal Nanite set failed: {e}")

def add_transform_keys(binding, frames_loc_rot_scale):
    track = binding.add_track(unreal.MovieScene3DTransformTrack)
    section = track.add_section()
    if not frames_loc_rot_scale:
        return
    section.set_range(0, max(1, frames_loc_rot_scale[-1][0] + 1))
    ch = section.get_all_channels()
    # Channels: 0,1,2=Loc X,Y,Z; 3,4,5=Rot X(roll),Y(pitch),Z(yaw); 6,7,8=Scale X,Y,Z
    for f, loc, rot, scale in frames_loc_rot_scale:
        fn = unreal.FrameNumber(int(f))
        ch[0].add_key(fn, float(loc.x))
        ch[1].add_key(fn, float(loc.y))
        ch[2].add_key(fn, float(loc.z))
        ch[3].add_key(fn, float(rot.roll))
        ch[4].add_key(fn, float(rot.pitch))
        ch[5].add_key(fn, float(rot.yaw))
        ch[6].add_key(fn, float(scale.x))
        ch[7].add_key(fn, float(scale.y))
        ch[8].add_key(fn, float(scale.z))

def read_camera_csv(path):
    rows = []
    with open(path, "r", newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append((
                int(r["frame"]),
                g2u_pos(float(r["px"]), float(r["py"]), float(r["pz"])),
                quat_to_cam_rot(float(r["qx"]), float(r["qy"]), float(r["qz"]), float(r["qw"])),
                float(r["fov_deg"]),
                float(r["near"]),
                float(r["far"]),
            ))
    return rows

def read_track_csv(path, is_light=False):
    rows = []
    with open(path, "r", newline="") as fh:
        for r in csv.DictReader(fh):
            m = [float(r[f"m{i}{j}"]) for i in range(3) for j in range(4)]
            f = int(r["frame"])
            if is_light:
                loc, rot = mat3x4_to_cam_light_loc_rot(m)
                rows.append((f, loc, rot, unreal.Vector(1.0, 1.0, 1.0)))
            else:
                tf = mat3x4_to_ue_transform(m)
                rows.append((f, tf.translation, tf.rotation.rotator(), tf.scale3d))
    return rows

def import_scene(export_dir, map_name, force_nanite=False, no_lumen=False, enable_megalights=False):
    print(f"=== IMPORTING {map_name} from {export_dir} (force_nanite={force_nanite} no_lumen={no_lumen} ml={enable_megalights}) ===")
    scene = json.load(open(os.path.join(export_dir, "scene.json"), "r"))
    root_pkg = f"/Game/Ports/{map_name}"
    map_pkg = f"{root_pkg}/{map_name}"
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    mel = unreal.MaterialEditingLibrary

    # 1. Import textures
    tex_map = {}
    tex_dir = f"{root_pkg}/Textures"
    for tex in scene.get("textures", []):
        tid = tex["id"]
        src = os.path.join(export_dir, tex["file"])
        paths = import_file(src, tex_dir)
        for p in paths:
            a = unreal.EditorAssetLibrary.load_asset(p)
            if isinstance(a, unreal.Texture2D):
                tex_map[tid] = a
                break

    # 2. Import sky.hdr
    sky_cube = None
    sky_path = os.path.join(export_dir, "sky.hdr")
    if os.path.isfile(sky_path):
        paths = import_file(sky_path, f"{root_pkg}/Sky")
        for p in paths:
            a = unreal.EditorAssetLibrary.load_asset(p)
            if isinstance(a, unreal.TextureCube):
                sky_cube = a
                break

    # 3. Import library.glb (static meshes)
    sm_map = {}
    lib_glb = os.path.join(export_dir, "library.glb")
    if os.path.isfile(lib_glb) and scene.get("meshes"):
        import_file(lib_glb, f"{root_pkg}/Meshes")
        mesh_meta = {m["id"]: m for m in scene["meshes"]}
        for ad in reg.get_assets_by_path(f"{root_pkg}/Meshes", recursive=True):
            if str(ad.asset_class_path.asset_name) == "StaticMesh":
                aname = str(ad.asset_name)
                sm = ad.get_asset()
                if aname in mesh_meta:
                    sm_map[aname] = sm
                    want_nanite = force_nanite or bool(mesh_meta[aname].get("nextgeo", False))
                    set_nanite_static(sm, want_nanite)

    # 4. Import skeletal meshes
    skm_map = {}
    anim_map = {}
    for sk in scene.get("skeletal_meshes", []):
        sid = sk["id"]
        src = os.path.join(export_dir, sk["file"])
        dest = f"{root_pkg}/Skinned/{sid}"
        import_file(src, dest)
        for ad in reg.get_assets_by_path(dest, recursive=True):
            cls = str(ad.asset_class_path.asset_name)
            aname = str(ad.asset_name)
            if cls == "SkeletalMesh":
                skm = ad.get_asset()
                skm_map[sid] = skm
                if force_nanite:
                    set_nanite_skeletal(skm, True)
            elif cls == "AnimSequence":
                anim = ad.get_asset()
                # Match clip name c0000 etc. or default
                anim_map[(sid, aname)] = anim
                if (sid, "__default__") not in anim_map:
                    anim_map[(sid, "__default__")] = anim

    # 5. Build MaterialInstanceConstants
    mat_map = {}
    mat_dir = f"{root_pkg}/Materials"
    for m in scene.get("materials", []):
        mid = m["id"]
        blend = m.get("blend", "opaque")
        if blend not in ("opaque", "masked", "translucent"):
            blend = "opaque"
        two_sided = bool(m.get("two_sided", False))
        tid = m.get("albedo_texture")
        tex_obj = tex_map.get(tid) if tid else None
        master = get_or_create_master_material(blend, two_sided, tex_obj is not None)

        mi_name = f"MI_{mid}"
        mi_pkg = f"{mat_dir}/{mi_name}"
        if unreal.EditorAssetLibrary.does_asset_exist(mi_pkg):
            mi = unreal.EditorAssetLibrary.load_asset(mi_pkg)
        else:
            mi = asset_tools.create_asset(mi_name, mat_dir, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mel.set_material_instance_parent(mi, master)

        alb = m.get("albedo_linear", [0.8, 0.8, 0.8, 1.0])
        if len(alb) < 4: alb = list(alb) + [1.0] * (4 - len(alb))
        mel.set_material_instance_vector_parameter_value(mi, "AlbedoColor", unreal.LinearColor(alb[0], alb[1], alb[2], alb[3]))
        props = m.get("properties", {})
        mel.set_material_instance_scalar_parameter_value(mi, "Metallic", float(props.get("metallic", 0.0)))
        mel.set_material_instance_scalar_parameter_value(mi, "Roughness", float(props.get("roughness", 0.8)))
        em = m.get("emission_linear", [0.0, 0.0, 0.0])
        mel.set_material_instance_vector_parameter_value(mi, "EmissiveColor", unreal.LinearColor(em[0], em[1], em[2], 1.0))
        if tex_obj is not None:
            mel.set_material_instance_texture_parameter_value(mi, "AlbedoTex", tex_obj)
            uvs = m.get("uv1_scale", [1.0, 1.0, 1.0])
            uvo = m.get("uv1_offset", [0.0, 0.0, 0.0])
            mel.set_material_instance_vector_parameter_value(mi, "UVScaleOffset", unreal.LinearColor(uvs[0], uvs[1], uvo[0], uvo[1]))
        try:
            mel.update_material_instance(mi)
        except Exception:
            pass
        unreal.EditorAssetLibrary.save_asset(mi_pkg)
        mat_map[mid] = mi

    # 6. Create new level
    level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    subobj_sub = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist("/Game/TestEmpty"):
        level_sub.load_level("/Game/TestEmpty")
    if unreal.EditorAssetLibrary.does_asset_exist(map_pkg):
        unreal.EditorAssetLibrary.delete_asset(map_pkg)
    level_sub.new_level(map_pkg)

    mover_actors = []  # (actor, track_file, is_light)
    inst_idx = 0

    # 7. Place geometry instances
    for inst in scene.get("instances", []):
        kind = inst["kind"]
        cast_shadow = bool(inst.get("cast_shadow", True))
        mats = [mat_map.get(mid) for mid in inst.get("materials", [])]

        if kind == "node":
            sm = sm_map.get(inst.get("mesh"))
            if not sm:
                continue
            tf = mat3x4_to_ue_transform(inst["transform"])
            actor = actor_sub.spawn_actor_from_class(unreal.StaticMeshActor, tf.translation, tf.rotation.rotator())
            actor.set_actor_scale3d(tf.scale3d)
            smc = actor.static_mesh_component
            is_mover = bool(inst.get("track"))
            smc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE if is_mover else unreal.ComponentMobility.STATIC)
            smc.set_static_mesh(sm)
            smc.set_editor_property("cast_shadow", cast_shadow)
            for si, mobj in enumerate(mats):
                if mobj:
                    smc.set_material(si, mobj)
            if is_mover:
                mover_actors.append((actor, os.path.join(export_dir, inst["track"]), False))
        elif kind in ("multimesh", "gridmap"):
            sm = sm_map.get(inst.get("mesh"))
            if not sm:
                continue
            inst_list = inst.get("instances", [])
            if not inst_list:
                continue
            actor = actor_sub.spawn_actor_from_class(unreal.Actor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
            actor.set_actor_label(f"HISM_{inst_idx}_{inst.get('mesh')}")
            handles = subobj_sub.k2_gather_subobject_data_for_instance(actor)
            params = unreal.AddNewSubobjectParams(
                parent_handle=handles[0],
                new_class=unreal.HierarchicalInstancedStaticMeshComponent,
            )
            nh, _reason = subobj_sub.add_new_subobject(params)
            hism = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(
                unreal.SubobjectDataBlueprintFunctionLibrary.get_data(nh)
            )
            hism.set_editor_property("mobility", unreal.ComponentMobility.STATIC)
            hism.set_static_mesh(sm)
            hism.set_editor_property("cast_shadow", cast_shadow)
            for si, mobj in enumerate(mats):
                if mobj:
                    hism.set_material(si, mobj)
            tfs = [mat3x4_to_ue_transform(m) for m in inst_list]
            # Bulk add in chunks of 25000
            for ci in range(0, len(tfs), 25000):
                hism.add_instances(tfs[ci:ci+25000], False, True)
            inst_idx += 1
        elif kind == "skeletal":
            sid = inst.get("skeletal")
            skm = skm_map.get(sid)
            if not skm:
                continue
            tf = mat3x4_to_ue_transform(inst["transform"])
            actor = actor_sub.spawn_actor_from_class(unreal.SkeletalMeshActor, tf.translation, tf.rotation.rotator())
            actor.set_actor_scale3d(tf.scale3d)
            skc = actor.skeletal_mesh_component
            skc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            skc.set_skeletal_mesh_asset(skm)
            skc.set_editor_property("cast_shadow", cast_shadow)
            for si, mobj in enumerate(mats):
                if mobj:
                    skc.set_material(si, mobj)
            # Attach animation clip
            clip = inst.get("clip", "")
            anim = anim_map.get((sid, clip)) or anim_map.get((sid, f"{sid}_{clip}"))
            if anim is None:
                # Search substring match
                for (k_sid, k_an), v in anim_map.items():
                    if k_sid == sid and clip and clip in k_an:
                        anim = v
                        break
            if anim is None:
                anim = anim_map.get((sid, "__default__"))
            if anim is not None:
                skc.set_editor_property("animation_mode", unreal.AnimationMode.ANIMATION_SINGLE_NODE)
                ad = skc.get_editor_property("animation_data")
                ad.set_editor_property("anim_to_play", anim)
                ad.set_editor_property("saved_playing", True)
                ad.set_editor_property("saved_looping", True)
                skc.set_editor_property("animation_data", ad)
            if inst.get("track"):
                mover_actors.append((actor, os.path.join(export_dir, inst["track"]), False))

    # 8. Place lights
    for lt in scene.get("lights", []):
        ltype = lt["type"]
        props = lt.get("properties", {})
        loc, rot = mat3x4_to_cam_light_loc_rot(lt["transform"])
        col = lt.get("color_linear", [1.0, 1.0, 1.0])
        lcol = unreal.LinearColor(col[0], col[1], col[2], 1.0)
        energy = float(props.get("light_energy", 1.0))
        shadows = bool(props.get("shadow_enabled", False))
        is_moving = bool(lt.get("track"))

        if ltype == "directional":
            actor = actor_sub.spawn_actor_from_class(unreal.DirectionalLight, loc, rot)
            lc = actor.light_component
            lc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            lc.set_intensity(energy * 3.14159)
            lc.set_light_color(lcol)
            lc.set_cast_shadows(shadows)
            if is_moving:
                mover_actors.append((actor, os.path.join(export_dir, lt["track"]), True))
        elif ltype == "omni":
            actor = actor_sub.spawn_actor_from_class(unreal.PointLight, loc, rot)
            lc = actor.point_light_component
            lc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            lc.set_intensity(energy * 1000.0)
            lc.set_light_color(lcol)
            lc.set_attenuation_radius(float(props.get("omni_range", 5.0)) * 100.0)
            lc.set_cast_shadows(shadows)
        elif ltype == "spot":
            actor = actor_sub.spawn_actor_from_class(unreal.SpotLight, loc, rot)
            lc = actor.spot_light_component
            lc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            lc.set_intensity(energy * 1000.0)
            lc.set_light_color(lcol)
            lc.set_attenuation_radius(float(props.get("spot_range", 5.0)) * 100.0)
            lc.set_outer_cone_angle(float(props.get("spot_angle", 45.0)))
            lc.set_cast_shadows(shadows)
        elif ltype == "area":
            actor = actor_sub.spawn_actor_from_class(unreal.RectLight, loc, rot)
            lc = actor.rect_light_component
            lc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
            lc.set_intensity(energy * 1000.0)
            lc.set_light_color(lcol)
            area = lt.get("area", {})
            sz = area.get("size", [1.0, 1.0])
            lc.set_source_width(float(sz[0]) * 100.0)
            lc.set_source_height(float(sz[1]) * 100.0)
            lc.set_attenuation_radius(float(props.get("area_range", 20.0)) * 100.0)
            if area.get("texture") and area["texture"] in tex_map:
                lc.set_source_texture(tex_map[area["texture"]])
            lc.set_cast_shadows(shadows)

    # 9. Environment: SkyLight, ExponentialHeightFog, PostProcessVolume
    env = scene.get("environment") or {}
    env_props = env.get("properties") or {}
    if sky_cube is not None:
        sl = actor_sub.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 500), unreal.Rotator(0, 0, 0))
        slc = sl.light_component
        slc.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        slc.set_editor_property("real_time_capture", False)
        slc.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_SPECIFIED_CUBEMAP)
        slc.set_editor_property("cubemap", sky_cube)
        slc.set_intensity(float(env_props.get("background_energy_multiplier", 1.0)))

    if env_props.get("volumetric_fog_enabled", False):
        fog = actor_sub.spawn_actor_from_class(unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
        fc = fog.component
        fc.set_editor_property("fog_density", float(env_props.get("volumetric_fog_density", 0.02)))
        fc.set_editor_property("enable_volumetric_fog", True)
        fc.set_editor_property("volumetric_fog_distance", float(env_props.get("volumetric_fog_length", 64.0)) * 100.0)

    ppv = actor_sub.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    ppv.set_editor_property("unbound", True)
    pps = ppv.get_editor_property("settings")
    if no_lumen:
        pps.set_editor_property("override_dynamic_global_illumination_method", True)
        pps.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.NONE)
        pps.set_editor_property("override_reflection_method", True)
        pps.set_editor_property("reflection_method", unreal.ReflectionMethod.NONE)
    if enable_megalights:
        for prop_ovr, prop_val in [
            ("override_b_mega_lights_enabled", "b_mega_lights_enabled"),
            ("override_mega_lights_enabled", "mega_lights_enabled"),
        ]:
            try:
                pps.set_editor_property(prop_ovr, True)
                pps.set_editor_property(prop_val, True)
                break
            except Exception:
                pass
    ppv.set_editor_property("settings", pps)

    # 10. Camera + LevelSequence (AutoPlay + Loop)
    cam_rows = read_camera_csv(os.path.join(export_dir, scene["camera"]["track"]))
    fps = int(scene.get("fps", 60))
    f0, loc0, rot0, fov0, near0, far0 = cam_rows[0]
    cam = actor_sub.spawn_actor_from_class(unreal.CineCameraActor, loc0, rot0)
    cc = cam.get_cine_camera_component()
    # Match vertical FOV (Godot default KEEP_HEIGHT) on 16:9 sensor (36x20.25mm)
    cc.filmback.sensor_width = 36.0
    cc.filmback.sensor_height = 20.25
    if scene["camera"].get("fov_axis", "vertical") == "vertical":
        focal = (cc.filmback.sensor_height * 0.5) / math.tan(math.radians(fov0 * 0.5))
    else:
        focal = (cc.filmback.sensor_width * 0.5) / math.tan(math.radians(fov0 * 0.5))
    cc.current_focal_length = focal
    cc.focus_settings.focus_method = unreal.CameraFocusMethod.DISABLE

    # Detect whether camera actually moves across rows
    cam_moves = any(
        (r[1] - loc0).length() > 1e-3 or abs(r[2].pitch - rot0.pitch) > 1e-3 or abs(r[2].yaw - rot0.yaw) > 1e-3 or abs(r[2].roll - rot0.roll) > 1e-3
        for r in cam_rows[1:]
    )
    seq_frames = len(cam_rows) if (cam_moves or mover_actors) else 1

    seq_name = f"{map_name}_Seq"
    seq_pkg = f"{root_pkg}/{seq_name}"
    if unreal.EditorAssetLibrary.does_asset_exist(seq_pkg):
        unreal.EditorAssetLibrary.delete_asset(seq_pkg)
    seq = asset_tools.create_asset(seq_name, root_pkg, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(fps, 1))
    seq.set_playback_start(0)
    seq.set_playback_end(seq_frames)

    cam_binding = seq.add_possessable(cam)
    cut_track = seq.add_track(unreal.MovieSceneCameraCutTrack)
    cut_sec = cut_track.add_section()
    cut_sec.set_range(0, seq_frames)
    binding_id = unreal.MovieSceneObjectBindingID()
    try:
        binding_id.set_editor_property("guid", cam_binding.get_id())
    except Exception:
        binding_id = unreal.MovieSceneSequenceExtensions.make_binding_id(seq, cam_binding)
    cut_sec.set_camera_binding_id(binding_id)

    if cam_moves:
        cam_keys = [(r[0], r[1], r[2], unreal.Vector(1.0, 1.0, 1.0)) for r in cam_rows]
    else:
        cam_keys = [(0, loc0, rot0, unreal.Vector(1.0, 1.0, 1.0))]
    add_transform_keys(cam_binding, cam_keys)

    for mactor, tpath, is_light in mover_actors:
        if os.path.isfile(tpath):
            mb = seq.add_possessable(mactor)
            add_transform_keys(mb, read_track_csv(tpath, is_light=is_light))

    unreal.EditorAssetLibrary.save_asset(seq_pkg)

    lsa = actor_sub.spawn_actor_from_class(unreal.LevelSequenceActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    lsa.set_sequence(seq)
    pb = lsa.get_editor_property("playback_settings")
    pb.set_editor_property("auto_play", True)
    lc_loop = pb.get_editor_property("loop_count")
    lc_loop.set_editor_property("value", -1)
    pb.set_editor_property("loop_count", lc_loop)
    lsa.set_editor_property("playback_settings", pb)

    level_sub.save_current_level()
    unreal.EditorAssetLibrary.save_directory(root_pkg, only_if_is_dirty=True, recursive=True)
    print(f"=== SAVED {map_pkg} OK (meshes={len(sm_map)} skm={len(skm_map)} mats={len(mat_map)} movers={len(mover_actors)} seq_frames={seq_frames}) ===")

def main():
    spec = os.environ.get("IMPORT_LIST", "")
    exports_root = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-compare/exports"
    for item in [x.strip() for x in spec.split(",") if x.strip()]:
        parts = item.split(":")
        exp_name, map_name = parts[0], parts[1]
        force_nanite = bool(int(parts[2])) if len(parts) > 2 else False
        no_lumen = bool(int(parts[3])) if len(parts) > 3 else False
        enable_ml = bool(int(parts[4])) if len(parts) > 4 else False
        import_scene(os.path.join(exports_root, exp_name), map_name, force_nanite, no_lumen, enable_ml)
    print("IMPORT_BATCH_DONE")

main()
