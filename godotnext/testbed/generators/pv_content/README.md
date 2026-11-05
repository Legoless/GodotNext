# PV content scenes

The shadow and light scenes for gates GV and GM (#213, plan §21.3.3).
Original procedural content only; every row's license is MIT in
`../../PROVENANCE.md`. Unlike the PN scenes these are committed under
`scenes/<name>/` because the geometry is light.

## Generate

From the repository root:

```
python3 godotnext/testbed/tools/ies_gen.py --check
godot --headless --path godotnext/testbed --script res://generators/build_pv.gd
godot --headless --path godotnext/testbed --script res://generators/validate_pv_content.gd
```

The scenes are also built by `generators/build_all.gd`. Two runs are
byte-identical.

## Scenes

| Scene | Content |
|---|---|
| `scenes/shadow_gallery/shadow_gallery.tscn` | 0.5° and 2° suns (alternated every 2 s by `gallery.gd`), 10/50 cm penumbra spheres, 10 cm and 1 cm planks at 50 m, the caster-class row (skinned walker, 10k MultiMesh, 5k GPUParticles3D, alpha foliage, VERTEX-animated material) and 256 local lights (64 shadowed) |
| `scenes/vsm_flythrough/vsm_flythrough.tscn` | The PV phase smoke scene: a 600 m street, sun, 16 shadowed local lights, a 2 m actor moving at 5 m/s, a 60 s path; `--tod` sweeps the sun 0.5°/frame |
| `scenes/fog_hall/fog_hall.tscn` | Volumetric fog, sun and 8 shadowed spots through transparent windows |
| `scenes/rt_shadow_yard/rt_shadow_yard.tscn` | Sun and 64 local lights, alpha foliage, colored glass, and a 20-layer caster-mask matrix (occluder *i* on layer *i*, matrix light *i* casting only layer *i*) |
| `scenes/mega_lights_hall/mega_lights_hall.tscn` | 64 base lights: point, spot, rect, textured rect, IES projectors from `assets/ies/`, and two lighting-channel lights; volumetric fog, front-layer glass and SSS foliage. `--lights=1024` or `--lights=4096` adds the larger variants from `hall.gd` |

## Harness

The harness takes a scene path at run time; there is no registry to edit.
Capture mode example:

```
godotnext/testbed/harness/runners/macos_local.sh -- \
  ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal \
  --write-movie "$OUT/frame.png" --fixed-fps 60 --quit-after 1062 -- \
  --mode=capture --scene=res://scenes/vsm_flythrough/vsm_flythrough.tscn \
  --camera=cam_main --out="$OUT" --seed=1 --frames=8 --warmup=4 --capture-frames=4,7
```

Timing mode uses the same `--scene`/`--camera`. The GV/GM metric sets
(Q7, Q8, Q10, Q11, Q15) are registered in the harness scene list as part
of the gate work; VSM Flythrough is the phase smoke scene (§22), so render
it with Metal validation before landing a PV change.
