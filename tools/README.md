# Tools

Scripts for the UE 5.9 versus GodotNext comparisons and the GPU soak.

The heavy files stay in the gitignored `local/godotnext-tools` directory: photogrammetry scenes, the Godot project, Unreal projects, renders, Blender, Mesa, SCons, Python virtualenvs, and the Vulkan validation layers. These scripts read and write that directory.

- `ue59-real-scenes/` builds the comparison scenes and runs the 4K captures.
- `ue59-compare/` drives the small Unreal comparison project stored with the scene data.
- `soak_1h.py` runs the one-hour GPU soak.

## Paths

The scripts work out their paths when they run. These environment variables override them:

- `GODOTNEXT_ROOT`: the repository root. Default: the checkout the script is in.
- `GODOTNEXT_TOOLS_DIR`: the heavy-data directory. Default: `<repo>/local/godotnext-tools`. The paths in `scene_specs.json` and `scene_specs_4k.json` are relative to it; an absolute path is used as it is.
- `UE_ROOT`: the Unreal Engine 5.9 install, the folder that holds `Engine/`. Every step that runs Unreal needs it, and there is no default. The runners also look in its `Engine/Saved` folders for screenshots and CSV profiles.
- `UE_EDITOR` and `UE_EDITOR_CMD`: the `UnrealEditor` and `UnrealEditor-Cmd` binaries. Default: `$UE_ROOT/Engine/Binaries/Linux/`.
- `GODOTNEXT_DROP_DIR`: the folder the comparison viewers copy their pages into, `godotnext-ue59-comparison` (4K) and `godotnext-ue59-comparison-v3` (1080p). Each build replaces its own page folder. Default: `<repo>/local/comparison-page`.

The Blender and Unreal scripts read the same variables. The capture runners start the Unreal scripts from this folder and pass them `GODOTNEXT_TOOLS_DIR`.
