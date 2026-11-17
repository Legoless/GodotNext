## Summary
Contract S7 (local half). UE 5.8 renders all local-light VSMs in one Nanite pass and calls many small local lights 'a work in progress'. GodotNext matches that: Lead choice (follow UE): every shadowed light keeps coarse pages, handed out first, and the 25% pool run must overflow with zero sampled-but-unmapped pages.
- Spot: 1 perspective map. Omni: 6 cube-face maps. Area: 5 faces (front hemisphere), replacing the dual-paraboloid path at `render_forward_clustered.cpp:2859-2872`.
- All local maps of a frame are rasterized in one batched dispatch (S4).
- A new `Light3D.shadow_resolution_bias` property (float, −2 to +4 mips, default 0).
- VSM lights bypass the 88-slot positional atlas and the `MAX_UPDATE_SHADOWS = 512` cap (`renderer_scene_cull.h:60`).

## Plan reference
[§21.3 Shadows and lights](https://github.com/Legoless/GodotNext/blob/master/docs/godotnext/nextgi-nextgeo-plan.html#parity-lights) · contract S7 · GV-3, GV-8

## Dependencies
**Blocked by:**
- #220 — VSM core: physical page pool, virtual page tables and per-light virtual maps (directional clipmaps, spot, omni, area)
- #219 — VSM page marking and allocation: depth-driven requests, coarse pages, LRU reuse and clipmap scrolling
- #222 — VSM projection and SMRT soft shadows: replace atlas sampling in the forward light loop, contact-hardening penumbra from light size
- #217 — VSM × Nanite: per-page multi-view cluster cull and raster of NextGeo clusters into the pool, one pass for directional and one for local lights

**Unblocks:**
- #223 — Gate GV: Virtual Shadow Maps at UE 5.9 parity (quality, caching, cost, platforms, legacy safety), then flip the new-project default

## Implementation steps
1. **Map kinds.** In `VirtualShadowMapStorage::map_allocate`, build the projection per face from the light transform, range and spot angle. Store per-map view-projection matrices in an SSBO next to the page table.
2. **Bypass the atlas.** In `RendererSceneCull::_light_instance_update_shadow` (`renderer_scene_cull.cpp:2402`), skip atlas allocation for VSM-method lights. Keep frustum, layer and `shadow_caster_mask` culling (`light_storage.h:72`). Count VSM lights separately from `MAX_UPDATE_SHADOWS`.
3. **Single dispatch.** Concatenate the dirty-page lists of all local maps into one indirect list. The raster passes from S4 read map index and page from it.
4. **Resolution bias.** Add the property to `Light3D` (`scene/3d/light_3d.h`), bind it, document it in `Light3D.xml`, and pass it through `LIGHT_PARAM` storage into `vsm_flags`. Page marking adds it to the mip.
5. **Distant lights.** Lights whose projected radius is below a screen threshold request only coarse mips. The threshold is a quality-preset value.
6. **Transmittance.** `LIGHT_PARAM_TRANSMITTANCE_BIAS` (`rendering_server_enums.h:268`) reads VSM depth through the S6 lookup. Subsurface transmittance keeps working.

## Acceptance criteria
**Build (PR basics, §22):**
- [ ] Builds on the touched CI targets (Windows and Linux Vulkan, Windows D3D12, macOS Metal); CI, unit tests and doctests pass, with new tests added.
- [ ] 0 validation errors on the phase smoke scene (§22) on Vulkan, D3D12 and Metal; it renders without crashing on the RTX 4080 SUPER and on the M5 Max.
- [ ] Shadow Gallery with 256 shadowed local lights (spot, omni, area mixed): every light casts a shadow.
- [ ] Local-light page raster for 64 dirty lights is issued as ≤4 indirect dispatches.

**Measured in PQ:**
- [ ] GV-1: Shadow Gallery with 256 shadowed local lights: mean Q7 against the D16 direct-visibility reference ≤0.05.
- [ ] GV (phase task criterion): Area-light shadow FLIP against the dual-paraboloid baseline ≤0.03 at equal texel density, with no hemisphere seam (max seam-line error ≤2/255).
- [ ] GV (phase task criterion): Transmittance on the SSS test material matches the atlas path within FLIP 0.02.

## Validation
At PR time:
- Vulkan, D3D12 and Metal (M5 Max); `--test --test-case="[VSM]*"`.

In PQ, with GV:
- The GV-1 Q7, area-light and transmittance FLIP captures.

## Parallelization
Lane: shadows. Branch `feature/pv-local-lights`. Open a PR.

## Out of scope
- Ray-traced local shadows (PM).

