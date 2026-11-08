# MegaLights source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used to write MegaLights code, and the contract decisions
that the plan and the issues leave open.

## Approved inputs

None so far. The direct-light inputs (#136) were written from the plan (§21.3, contract
M1), the issue and Godot's own Forward+ shaders. No paper, article or outside code was
consulted.

## Contract decisions

### M1 direct-light input targets (#136), 2026-11-08 — for later review

**Status: approved by the lead on 2026-11-08, on the reviewer's proposal. Flagged for
later review**, when #143 (MegaLights in the frame) consumes the targets and when the
PQ spike #144 compares the input paths.

The plan's M1 lists only "`ml_albedo` and `ml_material` (F0, shading model, layer-group
index)". Issue #136 contradicted itself: its summary called `ml_albedo` RGB the
*diffuse* albedo, while its acceptance line compared it with the color pass's `ALBEDO`,
the base color. The prepass also stored nothing a consumer could recover metallic
from. These decisions settle the contract so that #143 can reproduce the clustered
loop exactly:

| Field | Decision | Why |
|---|---|---|
| `ml_albedo.rgb` | Diffuse albedo, `ALBEDO * (1 - METALLIC)`. The PR check compares it with `ALBEDO * (1 - METALLIC)` within 1/255, on materials with metallic 0, 0.5 and 1. | This is the diffuse reflectance the clustered loop applies (`diffuse_light *= albedo * (1 - metallic)`). F0 already carries the metal tint, and metallic cannot be recovered from the packed F0. |
| `ml_albedo.a` | Direct-light occlusion, `mix(1.0, ao, ao_light_affect)` (the bent-normal variant where the color pass uses one for direct light). The field is no longer "specular occlusion". | This is the factor Forward+ applies to direct diffuse and specular light. The material's raw AO, or Godot's ambient-based specular occlusion, would make MegaLights frames darker or lighter than MegaLights-off frames. |
| Fallback (shading model 3, keeps the clustered loop) | Custom `light()`, `unshaded`, `diffuse_toon`, `diffuse_lambert_wrap`, `specular_toon` and `specular_disabled` (the issue's list), plus rim, backlight, anisotropy and `diffuse_lambert`. Transmission (model 1) requires SSS together with transmittance, as the color pass does. | `ml_material` cannot express rim, backlight, anisotropy or Lambert diffuse (Godot's default is Burley), so those materials stay on the clustered loop. Their pixels stay byte-identical to MegaLights-off frames. |

Issue #136's acceptance line is reworded to compare against `ALBEDO * (1 - METALLIC)`
when the change lands. The decision does not change the plan's M1 text; if #143 or the
spike #144 needs a different packing, revise it with the lead and record the change here.
