"""Import glTF 2.0 scenes (BistroPBR, AncientValley, JungleRuins) into RealScenesUE59
with Nanite enabled on ALL StaticMeshes, and configure clean multi-angle camera actors
+ matched Sun/Sky/Manual-Exposure PPV across all 5 comparison scenes."""
import json
import math
import os
import sys
import unreal


def g2u_pos(p):
    return unreal.Vector(float(p[0]) * 100.0, float(p[2]) * 100.0, float(p[1]) * 100.0)


def g2u_vec(v):
    return unreal.Vector(float(v[0]), float(v[2]), float(v[1]))


def vfov_to_hfov(vfov_deg, aspect=16.0 / 9.0):
    return math.degrees(2.0 * math.atan(math.tan(math.radians(vfov_deg) * 0.5) * aspect))


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
                if not ns.enabled:
                    ns.enabled = True
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


def configure_lighting_and_camera(actor_sub, sun_dir_g, sun_intensity, sky_intensity, exposure_bias, cam_cfg):
    # Remove old lights, sky, PPV, cameras, PlayerStart, SkySphere
    for a in list(actor_sub.get_all_level_actors()):
        cn = a.get_class().get_name()
        lbl = a.get_actor_label()
        if any(k in cn for k in ("Light", "Sky", "Atmosphere", "Fog", "PostProcess", "CameraActor", "CineCameraActor", "PlayerStart", "ReflectionCapture")) or "skysphere" in lbl.lower():
            try:
                actor_sub.destroy_actor(a)
            except Exception:
                pass
        else:
            # Also hide any SM_SkySphere component inside grouped blueprints
            for smc in a.get_components_by_class(unreal.StaticMeshComponent):
                if smc.static_mesh and "skysphere" in smc.static_mesh.get_name().lower():
                    smc.set_visibility(False, False)
                    smc.set_hidden_in_game(True, False)

    # Exact world sun direction vector (light travel direction)
    svec = g2u_vec(sun_dir_g)
    srot = unreal.MathLibrary.make_rot_from_x(svec)
    sun = actor_sub.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 2000), srot)
    sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sc.set_mobility(unreal.ComponentMobility.MOVABLE)
    sc.set_intensity(float(sun_intensity))
    sc.set_light_color(unreal.LinearColor(1.0, 0.96, 0.90, 1.0))
    sc.set_editor_property("cast_shadows", True)
    if hasattr(sc, "set_atmosphere_sun_light"):
        sc.set_atmosphere_sun_light(True)

    sky = actor_sub.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 2100), unreal.Rotator(0, 0, 0))
    skyc = sky.get_component_by_class(unreal.SkyLightComponent)
    skyc.set_mobility(unreal.ComponentMobility.MOVABLE)
    skyc.set_editor_property("real_time_capture", True)
    skyc.set_intensity(float(sky_intensity))
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
        ("override_auto_exposure_apply_physical_camera_exposure", True),
        ("auto_exposure_apply_physical_camera_exposure", False),
        ("override_auto_exposure_bias", True),
        ("auto_exposure_bias", float(exposure_bias)),
        ("override_motion_blur_amount", True),
        ("motion_blur_amount", 0.0),
        ("override_bloom_intensity", True),
        ("bloom_intensity", 0.0),
    ]:
        try:
            s.set_editor_property(prop, val)
        except Exception as e:
            print(f"  PPV prop {prop} warn: {e}")
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

    if action == "import_new":
        to_import = json.loads(os.environ["UE_IMPORT_LIST"])
        for item in to_import:
            dest = item["dest"]
            map_path = item["map"]
            gltf = item["gltf"]
            if unreal.EditorAssetLibrary.does_asset_exist(map_path):
                unreal.EditorAssetLibrary.delete_asset(map_path)
            level_sub.new_level(map_path)
            for a in list(actor_sub.get_all_level_actors()):
                cn = a.get_class().get_name()
                if any(k in cn for k in ("Light", "Floor", "Sky", "Atmosphere", "Fog", "PlayerStart", "PostProcess")):
                    actor_sub.destroy_actor(a)
            import_gltf_to_level(gltf, dest + "/Assets")
            enable_nanite_under(dest)
            # Strip imported punctual lights
            for a in list(actor_sub.get_all_level_actors()):
                cn = a.get_class().get_name()
                if any(k in cn for k in ("PointLight", "SpotLight", "DirectionalLight", "CameraActor", "CineCameraActor")):
                    actor_sub.destroy_actor(a)
            saved = unreal.EditorLoadingAndSavingUtils.save_map(ue_sub.get_editor_world(), map_path)
            print(f"IMPORTED & SAVED {map_path} => {saved}", flush=True)
    elif action == "setup_cam_maps":
        # Create one lightweight map copy per camera angle (or update camera on the base map)
        spec = json.loads(open(os.environ["SCENE_SPEC_JSON"]).read())
        base_map = spec["ue_base_map"]
        for cam in spec["cameras"]:
            cam_map = f"{spec['ue_cam_map_prefix']}_{cam['id']}"
            level_sub.load_level(base_map)
            configure_lighting_and_camera(
                actor_sub,
                spec["sun_dir"],
                spec["ue_sun_intensity"],
                spec["ue_sky_intensity"],
                cam.get("ue_exposure_bias", spec.get("ue_exposure_bias", 0.0)),
                cam,
            )
            saved = unreal.EditorLoadingAndSavingUtils.save_map(ue_sub.get_editor_world(), cam_map)
            print(f"SAVED CAM MAP {cam_map} => {saved}", flush=True)


if __name__ == "__main__":
    main()
