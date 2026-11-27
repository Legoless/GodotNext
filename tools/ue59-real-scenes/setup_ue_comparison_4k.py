"""4K Best-Quality UE 5.9 scene importer & camera/lighting configurator.
Imports fixed Bistro PBR (reflective dark translucent glass), true Jungle Ruins 4K (realized USD
PointInstancers + baked mossy terrain), and the 8K HDRI sky cubemap (kloppenheim_06_puresky_8k.hdr),
enables Nanite on ALL StaticMeshes, and builds Cinematic Lumen HWRT + Hit Lighting + 8K SkyLight +
matched warm PointLights + subtle ExponentialHeightFog + cinematic Bloom maps."""
import json
import math
import os
import unreal

HDRI_PATH = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/intel_jungle_ruins/extracted/JungleRuins/USD/kloppenheim_06_puresky_8k.hdr"


def g2u_pos(p):
    return unreal.Vector(float(p[0]) * 100.0, float(p[2]) * 100.0, float(p[1]) * 100.0)


def g2u_vec(v):
    return unreal.Vector(float(v[0]), float(v[2]), float(v[1]))


def vfov_to_hfov(vfov_deg, aspect=16.0 / 9.0):
    return math.degrees(2.0 * math.atan(math.tan(math.radians(vfov_deg) * 0.5) * aspect))


def ensure_hdri_cubemap(hdri_path=HDRI_PATH, dest_path="/Game/SharedSky"):
    stem = os.path.splitext(os.path.basename(hdri_path))[0]
    asset_path = f"{dest_path}/{stem}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        tex = unreal.EditorAssetLibrary.load_asset(asset_path)
        if tex is not None:
            return tex
    if not os.path.isfile(hdri_path):
        print(f"WARN: missing HDRI {hdri_path}", flush=True)
        return None
    task = unreal.AssetImportTask()
    task.filename = hdri_path
    task.destination_path = dest_path
    task.destination_name = stem
    task.replace_existing = True
    task.automated = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.EditorAssetLibrary.load_asset(asset_path)
    print(f"Imported HDRI cubemap -> {asset_path} ({tex})", flush=True)
    return tex


def ensure_hdri_skydome_material(hdri_tex, mat_name, brightness=1.0, rot_deg=0.0, dest_path="/Game/SharedSky"):
    mat_path = f"{dest_path}/{mat_name}"
    if unreal.EditorAssetLibrary.does_asset_exist(mat_path):
        unreal.EditorAssetLibrary.delete_asset(mat_path)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    mat_factory = unreal.MaterialFactoryNew()
    mat = asset_tools.create_asset(mat_name, dest_path, unreal.Material, mat_factory)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mel = unreal.MaterialEditingLibrary

    # Direction = normalize(WorldPosition - CameraPosition)
    wp = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -900, 0)
    cp = mel.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -900, 140)
    sub = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -700, 40)
    mel.connect_material_expressions(wp, "", sub, "A")
    mel.connect_material_expressions(cp, "", sub, "B")
    norm = mel.create_material_expression(mat, unreal.MaterialExpressionNormalize, -540, 40)
    mel.connect_material_expressions(sub, "", norm, "VectorInput")

    dir_node = norm
    if abs(rot_deg) > 1e-4:
        rot = mel.create_material_expression(mat, unreal.MaterialExpressionRotateAboutAxis, -380, 40)
        axis = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -620, -140)
        axis.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
        ang = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -620, -20)
        ang.set_editor_property("r", float(rot_deg) / 360.0)
        pivot = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -620, 220)
        pivot.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
        mel.connect_material_expressions(axis, "", rot, "NormalizedRotationAxis")
        mel.connect_material_expressions(ang, "", rot, "RotationAngle")
        mel.connect_material_expressions(pivot, "", rot, "PivotPoint")
        mel.connect_material_expressions(norm, "", rot, "Position")
        add_r = mel.create_material_expression(mat, unreal.MaterialExpressionAdd, -220, 40)
        mel.connect_material_expressions(norm, "", add_r, "A")
        mel.connect_material_expressions(rot, "", add_r, "B")
        dir_node = add_r

    tex_node = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -60, 40)
    tex_node.set_editor_property("texture", hdri_tex)
    try:
        tex_node.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    except Exception:
        pass
    mel.connect_material_expressions(dir_node, "", tex_node, "UVs")

    mult = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, 180, 40)
    bright = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, 0, 200)
    bright.set_editor_property("r", float(brightness))
    mel.connect_material_expressions(tex_node, "RGB", mult, "A")
    mel.connect_material_expressions(bright, "", mult, "B")
    mel.connect_material_property(mult, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    try:
        mel.recompile_material(mat)
    except Exception as e:
        print(f"  Deferred SkyDome material compile until GPU launch ({e})", flush=True)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def import_gltf_to_level(gltf_path, dest_dir):
    if not os.path.isfile(gltf_path):
        print(f"SKIP missing glTF: {gltf_path}", flush=True)
        return False
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    sd = unreal.InterchangeManager.create_source_data(gltf_path)
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    ok = im.import_scene(dest_dir, sd, params)
    print(f"Interchange import_scene({gltf_path} -> {dest_dir}) => {ok}", flush=True)
    return bool(ok)


def enable_nanite_under(dest_dir):
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    sm_sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    assets = reg.get_assets_by_path(dest_dir, recursive=True)
    nanite_count = 0
    for ad in assets:
        if str(ad.asset_class_path.asset_name) == "StaticMesh":
            sm = unreal.EditorAssetLibrary.load_asset(str(ad.package_name))
            if sm is None:
                continue
            try:
                ns = sm.get_editor_property("nanite_settings")
                changed = False
                if not ns.enabled:
                    ns.enabled = True
                    changed = True
                if hasattr(ns, "preserve_area"):
                    want_pa = any(k in str(ad.package_name).lower() for k in ("tree", "grass", "moss", "shrub", "leaf", "leaves", "palm", "ivy", "sapling", "seedling"))
                    if bool(ns.preserve_area) != want_pa:
                        ns.preserve_area = want_pa
                        changed = True
                if changed:
                    if sm_sub and hasattr(sm_sub, "set_nanite_settings"):
                        sm_sub.set_nanite_settings(sm, ns, apply_changes=True)
                    else:
                        sm.set_editor_property("nanite_settings", ns)
                    unreal.EditorAssetLibrary.save_loaded_asset(sm)
                nanite_count += 1
            except Exception as e:
                unreal.log_warning(f"Nanite failed on {ad.package_name}: {e}")
    saved = unreal.EditorAssetLibrary.save_directory(dest_dir, only_if_is_dirty=False, recursive=True)
    print(f"enable_nanite_under({dest_dir}): nanite_meshes={nanite_count} saved={saved}", flush=True)
    return nanite_count


def configure_lighting_and_camera(actor_sub, spec, cam_cfg, hdri_tex):
    # Remove old lights, sky, atmosphere, fog, PPV, cameras, PlayerStart, SkySphere, default Floor
    for a in list(actor_sub.get_all_level_actors()):
        cn = a.get_class().get_name()
        lbl = a.get_actor_label()
        if any(k in cn for k in ("Light", "Sky", "Atmosphere", "Fog", "PostProcess", "CameraActor", "CineCameraActor", "PlayerStart", "ReflectionCapture")) or "skysphere" in lbl.lower() or lbl.lower() == "floor":
            try:
                actor_sub.destroy_actor(a)
            except Exception:
                pass
        else:
            for smc in a.get_components_by_class(unreal.StaticMeshComponent):
                if smc.static_mesh and ("skysphere" in smc.static_mesh.get_name().lower() or smc.static_mesh.get_name().lower() == "floor"):
                    smc.set_visibility(False, False)
                    smc.set_hidden_in_game(True, False)

    # Exact world sun direction vector
    svec = g2u_vec(spec["sun_dir"])
    srot = unreal.MathLibrary.make_rot_from_x(svec)
    sun = actor_sub.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 2000), srot)
    sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sc.set_mobility(unreal.ComponentMobility.MOVABLE)
    sc.set_intensity(float(spec["ue_sun_intensity"]))
    scol = spec.get("sun_color", [1.0, 0.95, 0.86])
    sc.set_light_color(unreal.LinearColor(float(scol[0]), float(scol[1]), float(scol[2]), 1.0))
    sc.set_editor_property("cast_shadows", True)
    try:
        sc.set_editor_property("light_source_angle", 1.2)
    except Exception:
        pass
    if hasattr(sc, "set_atmosphere_sun_light"):
        sc.set_atmosphere_sun_light(True)

    # SkyLight: use 8K HDRI cubemap if imported, else real-time atmosphere capture
    sky_rot_deg = float(spec.get("ue_sky_rotation_deg", spec.get("sky_rotation_deg", 0.0)))
    sky = actor_sub.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 2100), unreal.Rotator(0, 0, 0))
    skyc = sky.get_component_by_class(unreal.SkyLightComponent)
    skyc.set_mobility(unreal.ComponentMobility.MOVABLE)
    skyc.set_intensity(float(spec["ue_sky_intensity"]))
    if hdri_tex is not None:
        try:
            skyc.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_SPECIFIED_CUBEMAP)
            skyc.set_editor_property("cubemap", hdri_tex)
            skyc.set_editor_property("source_cubemap_angle", sky_rot_deg)
            skyc.set_editor_property("real_time_capture", False)
            skyc.recapture_sky()
        except Exception as e:
            print(f"  HDRI SkyLight fallback ({e})", flush=True)
            skyc.set_editor_property("real_time_capture", True)
    else:
        skyc.set_editor_property("real_time_capture", True)

    if spec.get("use_hdri_skydome", False) and hdri_tex is not None:
        dome_mat = ensure_hdri_skydome_material(
            hdri_tex,
            f"M_SkyDome_{spec['id']}",
            brightness=float(spec.get("ue_skydome_brightness", spec["ue_sky_intensity"])),
            rot_deg=float(spec.get("ue_skydome_rotation_deg", sky_rot_deg)),
        )
        sphere_sm = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Sphere")
        if sphere_sm is not None:
            dome = actor_sub.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
            dome.set_actor_label("CompareSkyDome")
            dome.set_actor_scale3d(unreal.Vector(5000.0, 5000.0, 5000.0))
            smc = dome.get_component_by_class(unreal.StaticMeshComponent)
            smc.set_static_mesh(sphere_sm)
            smc.set_material(0, dome_mat)
            smc.set_editor_property("cast_shadow", False)
            for p, v in [
                ("visible_in_ray_tracing", False),
                ("affect_distance_field_lighting", False),
                ("affect_dynamic_indirect_lighting", False),
                ("affect_indirect_lighting_while_hidden", False),
            ]:
                try:
                    smc.set_editor_property(p, v)
                except Exception:
                    pass
    else:
        actor_sub.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))

    # Subtle ExponentialHeightFog for atmospheric depth
    fog_density = float(spec.get("fog_density", 0.0))
    if fog_density > 0.0:
        fog = actor_sub.spawn_actor_from_class(unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
        fc = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
        fc.set_editor_property("fog_density", fog_density * float(spec.get("ue_fog_density_scale", 1.4)))
        fc.set_editor_property("fog_height_falloff", float(spec.get("ue_fog_height_falloff", 0.12)))
        fcol = spec.get("fog_color", [0.78, 0.83, 0.90])
        fog_lum = float(spec.get("ue_fog_lum_scale", 0.45))
        fc.set_editor_property("fog_inscattering_luminance", unreal.LinearColor(float(fcol[0]) * fog_lum, float(fcol[1]) * fog_lum, float(fcol[2]) * fog_lum, 1.0))
        try:
            fc.set_editor_property("start_distance", float(spec.get("ue_fog_start_distance", 600.0)))
            fc.set_editor_property("fog_max_opacity", float(spec.get("ue_fog_max_opacity", 1.0)))
            fc.set_editor_property("enable_volumetric_fog", bool(spec.get("ue_volumetric_fog", True)))
        except Exception:
            pass

    # Matched warm punctual PointLights (streetlamps, cafe lanterns, Sponza braziers)
    for idx, pl_cfg in enumerate(spec.get("point_lights", [])):
        pl = actor_sub.spawn_actor_from_class(unreal.PointLight, g2u_pos(pl_cfg["pos"]), unreal.Rotator(0, 0, 0))
        pl.set_actor_label(f"ComparePointLight_{idx:02d}")
        plc = pl.get_component_by_class(unreal.PointLightComponent)
        plc.set_mobility(unreal.ComponentMobility.MOVABLE)
        plc.set_intensity(float(pl_cfg.get("ue_intensity", 350.0)))
        pc = pl_cfg.get("color", [1.0, 0.74, 0.40])
        plc.set_light_color(unreal.LinearColor(float(pc[0]), float(pc[1]), float(pc[2]), 1.0))
        plc.set_editor_property("attenuation_radius", float(pl_cfg.get("range", 9.0)) * 100.0)
        plc.set_editor_property("cast_shadows", True)
        try:
            plc.set_editor_property("source_radius", 14.0)
            plc.set_editor_property("soft_source_radius", 24.0)
            plc.set_editor_property("use_inverse_squared_falloff", True)
        except Exception:
            pass

    # PostProcessVolume: Cinematic Lumen GI + Hit Lighting Reflections + subtle Bloom + Manual Exposure
    ppv = actor_sub.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    ppv.set_editor_property("unbound", True)
    s = ppv.get_editor_property("settings")
    exposure_bias = float(cam_cfg.get("ue_exposure_bias", spec.get("ue_exposure_bias", 1.0)))
    props = [
        ("override_dynamic_global_illumination_method", True),
        ("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN),
        ("override_reflection_method", True),
        ("reflection_method", unreal.ReflectionMethod.LUMEN),
        ("override_lumen_final_gather_quality", True),
        ("lumen_final_gather_quality", 2.0),
        ("override_lumen_scene_detail", True),
        ("lumen_scene_detail", 2.0),
        ("override_lumen_reflection_quality", True),
        ("lumen_reflection_quality", 2.0),
        ("override_lumen_max_reflection_bounces", True),
        ("lumen_max_reflection_bounces", 2),
        ("override_lumen_max_refraction_bounces", True),
        ("lumen_max_refraction_bounces", 2),
        ("override_lumen_ray_lighting_mode", True),
        ("lumen_ray_lighting_mode", unreal.LumenRayLightingModeOverride.HIT_LIGHTING),
        ("override_auto_exposure_method", True),
        ("auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL),
        ("override_auto_exposure_apply_physical_camera_exposure", True),
        ("auto_exposure_apply_physical_camera_exposure", False),
        ("override_auto_exposure_bias", True),
        ("auto_exposure_bias", exposure_bias),
        ("override_motion_blur_amount", True),
        ("motion_blur_amount", 0.0),
        ("override_bloom_intensity", True),
        ("bloom_intensity", 0.22),
        ("override_bloom_threshold", True),
        ("bloom_threshold", 1.15),
    ]
    for prop, val in props:
        try:
            s.set_editor_property(prop, val)
        except Exception:
            pass
    ppv.set_editor_property("settings", s)

    cam_loc = g2u_pos(cam_cfg["pos"])
    cam_tgt = g2u_pos(cam_cfg["target"])
    cam_rot = unreal.MathLibrary.find_look_at_rotation(cam_loc, cam_tgt)
    cam = actor_sub.spawn_actor_from_class(unreal.CameraActor, cam_loc, cam_rot)
    cam.set_actor_label(f"Cam_{cam_cfg['id']}")
    cc = cam.get_component_by_class(unreal.CameraComponent)
    cc.set_field_of_view(vfov_to_hfov(float(cam_cfg["vfov"])))
    cc.set_aspect_ratio(16.0 / 9.0)
    cc.set_constraint_aspect_ratio(False)
    try:
        cam.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    except Exception:
        pass
    actor_sub.spawn_actor_from_class(unreal.PlayerStart, cam_loc, cam_rot)


def main():
    action = os.environ.get("UE_ACTION", "import_new")
    level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    ue_sub = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)

    hdri_tex = ensure_hdri_cubemap()

    if action == "import_new":
        to_import = json.loads(os.environ["UE_IMPORT_LIST"])
        for item in to_import:
            dest = item["dest"]
            map_path = item["map"]
            gltf = item["gltf"]
            if unreal.EditorAssetLibrary.does_directory_exist(dest):
                unreal.EditorAssetLibrary.delete_directory(dest)
            if unreal.EditorAssetLibrary.does_asset_exist(map_path):
                unreal.EditorAssetLibrary.delete_asset(map_path)
            level_sub.new_level(map_path)
            for a in list(actor_sub.get_all_level_actors()):
                cn = a.get_class().get_name()
                lbl = a.get_actor_label()
                if any(k in cn for k in ("Light", "Floor", "Sky", "Atmosphere", "Fog", "PlayerStart", "PostProcess")) or lbl.lower() == "floor":
                    actor_sub.destroy_actor(a)
            import_gltf_to_level(gltf, dest + "/Assets")
            enable_nanite_under(dest)
            for a in list(actor_sub.get_all_level_actors()):
                cn = a.get_class().get_name()
                if any(k in cn for k in ("PointLight", "SpotLight", "DirectionalLight", "CameraActor", "CineCameraActor")):
                    actor_sub.destroy_actor(a)
            saved = unreal.EditorLoadingAndSavingUtils.save_map(ue_sub.get_editor_world(), map_path)
            print(f"IMPORTED & SAVED {map_path} => {saved}", flush=True)
    elif action == "setup_cam_maps":
        spec = json.loads(open(os.environ["SCENE_SPEC_JSON"]).read())
        scene_hdri_path = spec.get("ue_sky_hdri", HDRI_PATH)
        scene_hdri_tex = ensure_hdri_cubemap(scene_hdri_path)
        base_map = spec["ue_base_map"]
        for cam in spec["cameras"]:
            cam_map = f"{spec['ue_cam_map_prefix']}_{cam['id']}"
            level_sub.load_level(base_map)
            configure_lighting_and_camera(actor_sub, spec, cam, scene_hdri_tex)
            saved = unreal.EditorLoadingAndSavingUtils.save_map(ue_sub.get_editor_world(), cam_map)
            print(f"SAVED 4K CAM MAP {cam_map} => {saved}", flush=True)


if __name__ == "__main__":
    main()
