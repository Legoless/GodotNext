"""Configure ClassicSponzaUnreal for UE 5.9 Vulkan SM6 Nanite + Lumen HWRT + VSM,
save SponzaNaniteLumen.umap, and export ClassicSponza.glb + camera metadata for GodotNext."""
import json
import math
import os
import unreal

EXPORT_DIR = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/exports/classic_sponza"
os.makedirs(EXPORT_DIR, exist_ok=True)

reg = unreal.AssetRegistryHelpers.get_asset_registry()
sm_sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
ue_sub = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)

# 1. Enable Nanite on ALL StaticMeshes under /Game/ClassicSponza
assets = reg.get_assets_by_path("/Game/ClassicSponza", recursive=True)
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
print(f"ClassicSponza: Nanite enabled on {nanite_count} static meshes")

# 2. Load SponzaPersistent (which streams SponzaGeometry) and inspect actors across ALL loaded sublevels
level_sub.load_level("/Game/ClassicSponza/Maps/SponzaPersistent")
actors = actor_sub.get_all_level_actors()
class_counts = {}
min_v = [1e9, 1e9, 1e9]
max_v = [-1e9, -1e9, -1e9]
sm_comps = 0
for a in actors:
    cn = a.get_class().get_name()
    class_counts[cn] = class_counts.get(cn, 0) + 1
    comps = a.get_components_by_class(unreal.StaticMeshComponent)
    if comps:
        sm_comps += len(comps)
        origin, extent = a.get_actor_bounds(False)
        if 1.0 < max(extent.x, extent.y, extent.z) < 50000.0:
            for i, (o, e) in enumerate([(origin.x, extent.x), (origin.y, extent.y), (origin.z, extent.z)]):
                min_v[i] = min(min_v[i], o - e)
                max_v[i] = max(max_v[i], o + e)
print(f"ClassicSponza SponzaPersistent: actors={len(actors)} sm_comps={sm_comps} classes={class_counts}")
print(f"ClassicSponza bounds: min={min_v} max={max_v}")

# Also check Sponza.umap just to compare
if sm_comps < 10 and unreal.EditorAssetLibrary.does_asset_exist("/Game/ClassicSponza/Maps/Sponza"):
    level_sub.load_level("/Game/ClassicSponza/Maps/Sponza")
    a2 = actor_sub.get_all_level_actors()
    c2 = sum(len(a.get_components_by_class(unreal.StaticMeshComponent)) for a in a2)
    print(f"ClassicSponza Sponza.umap: actors={len(a2)} sm_comps={c2}")
    if c2 > sm_comps:
        actors = a2
        sm_comps = c2
        min_v = [1e9, 1e9, 1e9]; max_v = [-1e9, -1e9, -1e9]
        for a in actors:
            if a.get_components_by_class(unreal.StaticMeshComponent):
                origin, extent = a.get_actor_bounds(False)
                if 1.0 < max(extent.x, extent.y, extent.z) < 50000.0:
                    for i, (o, e) in enumerate([(origin.x, extent.x), (origin.y, extent.y), (origin.z, extent.z)]):
                        min_v[i] = min(min_v[i], o - e)
                        max_v[i] = max(max_v[i], o + e)
    else:
        level_sub.load_level("/Game/ClassicSponza/Maps/SponzaPersistent")

if min_v[0] > 1e8:
    min_v = [-1400.0, -700.0, 0.0]
    max_v = [1400.0, 700.0, 1100.0]

# Remove old baked lights/reflection captures/cameras in persistent level and add Movable Sun + Sky + PPV + BenchCamera
for a in list(actor_sub.get_all_level_actors()):
    cn = a.get_class().get_name()
    if any(k in cn for k in ("DirectionalLight", "SkyLight", "ReflectionCapture", "PostProcessVolume", "CameraActor", "CineCameraActor", "PlayerStart")):
        try:
            actor_sub.destroy_actor(a)
        except Exception:
            pass

sun = actor_sub.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500), unreal.Rotator(0.0, -55.0, -35.0))
sc = sun.get_component_by_class(unreal.DirectionalLightComponent)
sc.set_mobility(unreal.ComponentMobility.MOVABLE)
sc.set_intensity(6.0)
sc.set_light_color(unreal.LinearColor(1.0, 0.96, 0.90, 1.0))
sc.set_editor_property("cast_shadows", True)
if hasattr(sc, "set_atmosphere_sun_light"):
    sc.set_atmosphere_sun_light(True)

sky = actor_sub.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 600), unreal.Rotator(0, 0, 0))
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

cx = (min_v[0] + max_v[0]) * 0.5
cy = (min_v[1] + max_v[1]) * 0.5
cz = min_v[2] + 220.0
span_x = max_v[0] - min_v[0]
span_y = max_v[1] - min_v[1]
if span_x >= span_y:
    cam_loc = unreal.Vector(min_v[0] + span_x * 0.18, cy, cz)
    cam_tgt = unreal.Vector(max_v[0] - span_x * 0.18, cy, cz + 80.0)
else:
    cam_loc = unreal.Vector(cx, min_v[1] + span_y * 0.18, cz)
    cam_tgt = unreal.Vector(cx, max_v[1] - span_y * 0.18, cz + 80.0)

cam_rot = unreal.MathLibrary.find_look_at_rotation(cam_loc, cam_tgt)
cam = actor_sub.spawn_actor_from_class(unreal.CameraActor, cam_loc, cam_rot)
cam.set_actor_label("BenchCamera")
cc = cam.get_component_by_class(unreal.CameraComponent)
cc.set_field_of_view(68.0)
cc.set_aspect_ratio(16.0 / 9.0)
cc.set_constraint_aspect_ratio(False)
try:
    cam.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
except Exception as e:
    print("auto_activate_for_player err:", e)
actor_sub.spawn_actor_from_class(unreal.PlayerStart, cam_loc, cam_rot)

level_sub.save_current_level()

# Export the loaded world to GLB via UE's built-in GLTFExporter!
glb_path = os.path.join(EXPORT_DIR, "ClassicSponza.glb")
try:
    opts = unreal.GLTFExportOptions()
    opts.export_uniform_scale = 0.01
    opts.export_vertex_colors = False
    opts.export_material_variants = unreal.GLTFMaterialVariantMode.NONE
    opts.bake_material_inputs = unreal.GLTFMaterialBakeMode.USE_MESH_DATA
    world = ue_sub.get_editor_world()
    ok = unreal.GLTFExporter.export_to_gltf(world, glb_path, opts, set())
    print(f"GLTFExporter.export_to_gltf -> ok={ok} path={glb_path} size={os.path.getsize(glb_path) if os.path.isfile(glb_path) else 0}")
except Exception as e:
    print(f"GLTFExporter exception: {e}")

meta = {
    "scene": "classic_sponza",
    "nanite_meshes": nanite_count,
    "sm_comps": sm_comps,
    "ue_map": ue_sub.get_editor_world().get_path_name(),
    "ue_cam_loc_cm": [cam_loc.x, cam_loc.y, cam_loc.z],
    "ue_cam_tgt_cm": [cam_tgt.x, cam_tgt.y, cam_tgt.z],
    "bounds_cm": {"min": min_v, "max": max_v},
}
with open(os.path.join(EXPORT_DIR, "meta.json"), "w") as f:
    json.dump(meta, f, indent=2)
print("ClassicSponza setup complete:", json.dumps(meta))
