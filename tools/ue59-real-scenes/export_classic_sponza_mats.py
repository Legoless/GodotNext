import json, os, unreal

out_root = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/exports/classic_sponza_tex"
tex_raw_dir = os.path.join(out_root, "raw_tex")
os.makedirs(tex_raw_dir, exist_ok=True)

gltf_path = os.path.join(out_root, "ClassicSponza.gltf")
g = json.load(open(gltf_path))
wanted_mat_names = {m.get("name", "") for m in g.get("materials", [])}
print(f"ClassicSponza glTF wants {len(wanted_mat_names)} materials: {sorted(wanted_mat_names)[:10]}", flush=True)

reg = unreal.AssetRegistryHelpers.get_asset_registry()
all_assets = reg.get_assets_by_path("/Game/ClassicSponza", recursive=True)
mat_map = {}
tex_to_export = set()

for ad in all_assets:
    cname = str(ad.asset_class_path.asset_name)
    aname = str(ad.asset_name)
    pkg = str(ad.package_name)
    if "Material" in cname and (aname in wanted_mat_names or any(aname in w or w in aname for w in wanted_mat_names)):
        obj = unreal.EditorAssetLibrary.load_asset(pkg)
        if obj is None:
            continue
        info = {"pkg": pkg, "class": cname, "textures": {}, "scalars": {}, "vectors": {}}
        if isinstance(obj, unreal.MaterialInstance):
            for tp in obj.texture_parameter_values:
                pname = str(tp.parameter_info.name)
                tval = tp.parameter_value
                if tval is not None:
                    tpkg = tval.get_outer().get_name()
                    info["textures"][pname] = tpkg
                    tex_to_export.add(tpkg)
            for sp in obj.scalar_parameter_values:
                info["scalars"][str(sp.parameter_info.name)] = float(sp.parameter_value)
            for vp in obj.vector_parameter_values:
                c = vp.parameter_value
                info["vectors"][str(vp.parameter_info.name)] = [float(c.r), float(c.g), float(c.b), float(c.a)]
        mat_map[aname] = info

print(f"Matched {len(mat_map)} materials, exporting {len(tex_to_export)} unique Texture2D assets...", flush=True)
if tex_to_export:
    at = unreal.AssetToolsHelpers.get_asset_tools()
    at.export_assets(sorted(tex_to_export), tex_raw_dir)

with open(os.path.join(out_root, "ue_materials.json"), "w") as f:
    json.dump(mat_map, f, indent=2)
print("Saved ue_materials.json!", flush=True)
