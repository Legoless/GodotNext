# Tools

Scripts for the UE 5.9 versus GodotNext comparisons and the GPU soak.

The heavy files stay in the gitignored `local/godotnext-tools` directory: photogrammetry scenes, the Godot project, Unreal projects, renders, Blender, Mesa, SCons, Python virtualenvs, and the Vulkan validation layers. These scripts read and write that directory.

- `ue59-real-scenes/` builds the comparison scenes and runs the 4K captures.
- `ue59-compare/` drives the small Unreal comparison project stored with the scene data.
- `soak_1h.py` runs the one-hour GPU soak.
