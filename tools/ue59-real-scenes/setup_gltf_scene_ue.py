"""Import glTF 2.0 real-world scenes (Amazon Lumberyard Bistro, Intel Sponza 2022 + Curtains + Ivy)
into RealScenesUE59 with Nanite enabled on ALL StaticMeshes + Lumen HWRT + VSM + matching camera."""
import json
import math
import os
import sys
import unreal


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

REAL_ROOT = os.path.join(TOOLS_DIR, "ue59-real-scenes")

SCENES = {
    "bistro": {
        "gltfs": [
            os.path.join(REAL_ROOT, "niagara_bistro/bistro.gltf"),
        ],
        "dest": "/Game/RealScenes/BistroV2",
        "map": "/Game/RealScenes/BistroV2/BistroNaniteLumen",
        # From bistro.gltf authored Camera node (-26.43, 3.16, 11.17) looking down the Parisian bistro street
        "use_gltf_cam": True,
        "fallback_cam_pos_g": [-26.43, 3.16, 11.17],
        "fallback_cam_tgt_g": [-5.0, 3.5, 4.0],
        "vfov_deg": 36.0,
        "sun_pitch_yaw_ue": (-50.0, -30.0),
    },
    "intel_sponza": {
        "gltfs": [
            os.path.join(REAL_ROOT, "intel_sponza/extracted/main_sponza/NewSponza_Main_glTF_003.gltf"),
            os.path.join(REAL_ROOT, "intel_sponza/extracted/pkg_a_curtains/NewSponza_Curtains_glTF.gltf"),
            os.path.join(REAL_ROOT, "intel_sponza/extracted/pkg_b_ivy/NewSponza_IvyGrowth_glTF.gltf"),
        ],
        "dest": "/Game/RealScenes/IntelSponzaV2",
        "map": "/Game/RealScenes/IntelSponzaV2/IntelSponzaNaniteLumen",
        "use_gltf_cam": False,
        "fallback_cam_pos_g": [-8.5, 2.2, 0.0],
        "fallback_cam_tgt_g": [6.0, 3.0, 0.0],
        "vfov_deg": 45.0,
        "sun_pitch_yaw_ue": (-65.0, -25.0),
    },
}

def g2u_pos(p):
    return unreal.Vector(float(p[0]) * 100.0, float(p[2]) * 100.0, float(p[1]) * 100.0)

def vfov_to_hfov(vfov_deg, aspect=16.0/9.0):
    return math.degrees(2.0 * math.atan(math.tan(math.radians(vfov_deg) * 0.5) * aspect))

def import_gltf_to_level(gltf_path, dest_dir):
    if not os.path.isfile(gltf_path):
        print(f"SKIP missing glTF: {gltf_path}")
        return False
    im = unreal.InterchangeManager.get_interchange_manager_scripted()
    sd = unreal.InterchangeManager.create_source_data(gltf_path)
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    world = unreal.EditorLevelLibrary.get_editor_world()
    ok = im.import_scene(dest_dir, sd, params)
    print(f"Interchange import_scene({gltf_path} -> {dest_dir}) => {ok}")
    return bool(ok)

def setup_scene(key):
    cfg = SCENES[key]
    level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    sm_sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    reg = unreal.AssetRegistryHelpers.get_asset_registry()

    if unreal.EditorAssetLibrary.does_asset_exist(cfg["map"]):
        unreal.EditorAssetLibrary.delete_asset(cfg["map"])
    ok_lvl = level_sub.new_level(cfg["map"])
    print(f"new_level({cfg['map']}) => {ok_lvl}")
    # Destroy default floor/lights if any
    for a in list(actor_sub.get_all_level_actors()):
        cname = a.get_class().get_name()
        if any(k in cname for k in ("Light", "Floor", "Sky", "Atmosphere", "Fog", "PlayerStart", "PostProcess")):
            actor_sub.destroy_actor(a)

    for idx, gp in enumerate(cfg["gltfs"]):
        sub = f"{cfg['dest']}/Part{idx}"
        import_gltf_to_level(gp, sub)

    # Enable Nanite on ALL imported StaticMeshes!
    assets = reg.get_assets_by_path(cfg["dest"], recursive=True)
    nanite_count = 0
    total_tris = 0
    for ad in assets:
        if str(ad.asset_class_path.asset_name) == "StaticMesh":
            sm = unreal.EditorAssetLibrary.load_asset(str(ad.package_name))
            if sm is None:
                continue
            try:
                ns = sm.get_editor_property("nanite_settings")
                if not ns.enabled:
                    ns.enabled = True
                    if sm_sub and hasattr(sm_sub, "set_nanite_settings"):
                        sm_sub.set_nanite_settings(sm, ns, apply_changes=True)
                    else:
                        sm.set_editor_property("nanite_settings", ns)
                    unreal.EditorAssetLibrary.save_loaded_asset(sm)
                nanite_count += 1
                if sm_sub and hasattr(sm_sub, "get_number_triangles"):
                    total_tris += int(sm_sub.get_number_triangles(sm, 0))
            except Exception as e:
                unreal.log_warning(f"Nanite failed on {ad.package_name}: {e}")

    # Save ALL imported meshes, materials, and textures to disk!
    saved_dir = unreal.EditorAssetLibrary.save_directory(cfg["dest"], only_if_is_dirty=False, recursive=True)
    print(f"save_directory({cfg['dest']}) => {saved_dir}")

    # Inspect imported actors & bounds
    actors = actor_sub.get_all_level_actors()
    sm_actors = 0
    cam_actor = None
    min_v = [1e9, 1e9, 1e9]
    max_v = [-1e9, -1e9, -1e9]
    for a in list(actors):
        cname = a.get_class().get_name()
        if isinstance(a, unreal.StaticMeshActor):
            sm_actors += 1
            origin, extent = a.get_actor_bounds(False)
            for i, (o, e) in enumerate([(origin.x, extent.x), (origin.y, extent.y), (origin.z, extent.z)]):
                min_v[i] = min(min_v[i], o - e)
                max_v[i] = max(max_v[i], o + e)
        elif isinstance(a, (unreal.CameraActor, unreal.CineCameraActor)) and cfg["use_gltf_cam"] and cam_actor is None:
            cam_actor = a
        elif "PointLight" in cname or "SpotLight" in cname or "DirectionalLight" in cname:
            # Remove imported punctual/sun lights so both sides share exact clean Sun + Sky lighting
            actor_sub.destroy_actor(a)

    print(f"{key}: nanite_meshes={nanite_count} total_tris={total_tris} sm_actors={sm_actors} bounds={min_v}..{max_v} cam_actor={cam_actor}")

    # Movable Directional Sun + SkyLight + SkyAtmosphere + PostProcessVolume
    pitch, yaw = cfg["sun_pitch_yaw_ue"]
    sun = actor_sub.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000), unreal.Rotator(0.0, pitch, yaw))
    sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sc.set_mobility(unreal.ComponentMobility.MOVABLE)
    sc.set_intensity(6.0)
    sc.set_light_color(unreal.LinearColor(1.0, 0.96, 0.90, 1.0))
    sc.set_editor_property("cast_shadows", True)
    if hasattr(sc, "set_atmosphere_sun_light"):
        sc.set_atmosphere_sun_light(True)

    sky = actor_sub.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 1100), unreal.Rotator(0, 0, 0))
    skyc = sky.get_component_by_class(unreal.SkyLightComponent)
    skyc.set_mobility(unreal.ComponentMobility.MOVABLE)
    skyc.set_editor_property("real_time_capture", True)
    skyc.set_intensity(1.0)
    actor_sub.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))

    ppv = actor_sub.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    ppv.set_editor_property("unbound", True)
    s = ppv.get_editor_property("settings")
    for prop, val in [
        ("override_dynamic_global_illumination_method", True),
        ("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN),
        ("override_reflection_method", True),
        ("reflection_method", unreal.ReflectionMethod.LUMEN),
        ("override_auto_exposure_method", True),
        ("auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL),
        ("override_auto_exposure_bias", True),
        ("auto_exposure_bias", 0.0),
        ("override_motion_blur_amount", True),
        ("motion_blur_amount", 0.0),
        ("override_bloom_intensity", True),
        ("bloom_intensity", 0.0),
    ]:
        try:
            s.set_editor_property(prop, val)
        except Exception:
            pass
    ppv.set_editor_property("settings", s)

    if cam_actor is None:
        cam_loc = g2u_pos(cfg["fallback_cam_pos_g"])
        cam_tgt = g2u_pos(cfg["fallback_cam_tgt_g"])
        cam_rot = unreal.MathLibrary.find_look_at_rotation(cam_loc, cam_tgt)
        cam_actor = actor_sub.spawn_actor_from_class(unreal.CameraActor, cam_loc, cam_rot)
        cam_actor.set_actor_label("BenchCamera")
        cc = cam_actor.get_component_by_class(unreal.CameraComponent)
        cc.set_field_of_view(vfov_to_hfov(cfg["vfov_deg"]))
        cc.set_aspect_ratio(16.0 / 9.0)
        cc.set_constraint_aspect_ratio(False)
    else:
        cam_loc = cam_actor.get_actor_location()
        cam_rot = cam_actor.get_actor_rotation()

    try:
        cam_actor.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    except Exception as e:
        print("auto_activate_for_player err:", e)

    actor_sub.spawn_actor_from_class(unreal.PlayerStart, cam_loc, cam_rot)
    ue_sub = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    saved = unreal.EditorLoadingAndSavingUtils.save_map(ue_sub.get_editor_world(), cfg["map"])
    print(f"save_map({cfg['map']}) => {saved}")

    out_meta = {
        "key": key,
        "map": cfg["map"],
        "nanite_meshes": nanite_count,
        "total_tris": total_tris,
        "sm_actors": sm_actors,
        "bounds_cm": {"min": min_v, "max": max_v},
        "cam_loc_cm": [cam_loc.x, cam_loc.y, cam_loc.z],
        "cam_rot_ue": [cam_rot.roll, cam_rot.pitch, cam_rot.yaw],
    }
    meta_dir = os.path.join(REAL_ROOT, "exports")
    os.makedirs(meta_dir, exist_ok=True)
    with open(os.path.join(meta_dir, f"{key}_ue_meta.json"), "w") as f:
        json.dump(out_meta, f, indent=2)
    print(f"SAVED {key}: {json.dumps(out_meta)}")

target = os.environ.get("REAL_SCENE_KEY", "all")
keys = list(SCENES.keys()) if target == "all" else [target]
for k in keys:
    setup_scene(k)
