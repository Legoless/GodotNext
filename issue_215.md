## Summary
Contract S6 (receivers other than opaque surfaces). Lead choice (follow UE): every shadowed light keeps coarse pages, handed out first, and the 25% pool run must overflow with zero sampled-but-unmapped pages.
- **Fog:** `volumetric_fog_process.glsl` binds `shadow_atlas` and `directional_shadow_atlas` (:27-28) and samples them per froxel (:431 directional; :495-509 positional). VSM lights call `vsm_shadow_coarse()`: one tap, no SMRT, at the coarse mips marked by #219 step 3.
- **Transparents:** forward transparents use the same coarse lookup by default. `rendering/lights_and_shadows/virtual_shadows/translucent_quality = 0` (0 = coarse, 1 = full SMRT), matching UE's default of 0.

## Plan reference
[§21.3 Shadows and lights](https://github.com/Legoless/GodotNext/blob/master/docs/godotnext/nextgi-nextgeo-plan.html#parity-lights) · contract S6 · GV-7 · budget V5 · [§23](https://github.com/Legoless/GodotNext/blob/master/docs/godotnext/nextgi-nextgeo-plan.html#ue-bar) UE bar

## Dependencies
**Blocked by:**
- #222 — VSM projection and SMRT soft shadows: replace atlas sampling in the forward light loop, contact-hardening penumbra from light size

**Unblocks:**
- #137 — MegaLights fog and translucency: shared froxel lighting volume merged with the D17 translucency volume, high-quality front-layer pass, transmission
- #223 — Gate GV: Virtual Shadow Maps at UE 5.9 parity (quality, caching, cost, platforms, legacy safety), then flip the new-project default

## Implementation steps
1. **Bindings.** Add the page table and pool to the fog process uniform set in `fog.cpp`, next to bindings 1-2.
2. **Directional.** At :431, for VSM lights replace the PSSM lookup with `vsm_shadow_coarse(map_base, froxel_world_pos)`. Keep the exponential fade (`INV_FOG_FADE`).
3. **Positional.** At :495-509, same replacement for omni and spot.
4. **Transparents.** In `scene_forward_clustered.glsl`, when the material is in the alpha pass and `translucent_quality == 0`, call the coarse variant.
5. **Missing page.** If a coarse page is not resident, return unshadowed and increment a debug counter. GV-7 requires the counter to stay 0.

## Acceptance criteria
**Build (PR basics, §22):**
- [ ] Builds on the touched CI targets (Windows and Linux Vulkan, Windows D3D12, macOS Metal); CI, unit tests and doctests pass, with new tests added.
- [ ] 0 validation errors on the phase smoke scene (§22) on Vulkan, D3D12 and Metal; it renders without crashing on the RTX 4080 SUPER and on the M5 Max.
- [ ] Missing-coarse-page counter is 0 over 3,000 frames of VSM Flythrough (GV-7 re-checks it in PQ).

**Measured in PQ:**
- [ ] GV-7: Fog Hall scene (sun + 8 spots through windows): FLIP of the fog-only buffer against the atlas path at equal texel density ≤0.03.
- [ ] GV (phase task criterion): Budget V5: fog + transparents with VSM, RTX 4080 SUPER: report-only target: atlas cost + 0.05 ms (the gate's UE bar decides, §23).

## Validation
At PR time:
- Vulkan, D3D12, Metal (M5 Max): the Build criteria above.

In PQ, with GV:
- GV-7 on Fog Hall, and the V5 timing on the RTX 4080 SUPER and the RTX 5090 machine (#227). V5 is a per-pass line, so it is reported only; GV-3's VSM total on Dense City carries GV's UE bar (§23).

## Parallelization
Lane: shadows. Branch `feature/pv-fog-translucency`. Open a PR. Coordinate with #137, which replaces the per-froxel light loop for MegaLights lights.

## Out of scope
- MegaLights froxel lighting.

