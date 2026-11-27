import json, os, unreal
level_sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
level_sub.load_level("/Game/ClassicSponza/Maps/SponzaPersistent")
out_root = "/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/exports/classic_sponza_tex"

mesh_mats = {}
comp_mats = {}
for a in actor_sub.get_all_level_actors():
    for smc in a.get_components_by_class(unreal.StaticMeshComponent):
        sm = smc.static_mesh
        if sm is None:
            continue
        sm_name = sm.get_name()
        mats = []
        n = smc.get_num_materials()
        for i in range(n):
            m = smc.get_material(i)
            mats.append(m.get_name() if m else "")
        mesh_mats[sm_name] = mats
        comp_mats[f"{a.get_actor_label()}::{smc.get_name()}"] = {"mesh": sm_name, "mats": mats}

with open(os.path.join(out_root, "mesh_mats.json"), "w") as f:
    json.dump({"mesh_mats": mesh_mats, "comp_mats": comp_mats}, f, indent=2)
print(f"Dumped mesh_mats ({len(mesh_mats)} meshes, {len(comp_mats)} comps)", flush=True)
