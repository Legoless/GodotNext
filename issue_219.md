## Summary
Contract S3. Page requests come from the screen, as in UE: only pages that a visible pixel samples get memory.
- One request per pixel per light that reaches it, at the mip that gives ≤1 shadow texel per screen-pixel footprint, plus a configurable LOD bias.
- Coarse pages: always-resident coarse mips for volumetric-fog froxels and forward transparents (UE's coarse-page marking). Every shadowed light keeps coarse pages, handed out first, and the 25% pool run must overflow with zero sampled-but-unmapped pages.
- Allocation keeps cached pages, then pulls from an LRU free list.
- **Overflow (deviation, stated):** UE corrupts shadows when the pool overflows. GodotNext raises the LOD bias by one mip for the largest requesters and retries, and sets the OVERFLOW flag for stats.

## Plan reference
[§21.3 Shadows and lights](https://github.com/Legoless/GodotNext/blob/master/docs/godotnext/nextgi-nextgeo-plan.html#parity-lights) · contract S3 · GV-1, GV-2 · budget V1

## Dependencies
**Blocked by:**
- #37 — Velocity from the forced NextGI prepass, with per-upscaler pre-clear and motion-pass rules (TAA, FSR2, MetalFX)
- #220 — VSM core: physical page pool, virtual page tables and per-light virtual maps (directional clipmaps, spot, omni, area)

**Unblocks:**
- #223 — Gate GV: Virtual Shadow Maps at UE 5.9 parity (quality, caching, cost, platforms, legacy safety), then flip the new-project default
- #216 — VSM local lights: spot, omni and area maps, many-light single dispatch, per-light resolution bias and distant-light policy
- #217 — VSM × Nanite: per-page multi-view cluster cull and raster of NextGeo clusters into the pool, one pass for directional and one for local lights
- #218 — VSM non-Nanite path: per-page GPU instance culling and indirect raster for skinned, MultiMesh, particles, alpha-tested and custom-vertex meshes
- #221 — VSM product surface: shadow-method settings and tier rule, pool scalability, overflow stats, visualize modes and editor UX
- #222 — VSM projection and SMRT soft shadows: replace atlas sampling in the forward light loop, contact-hardening penumbra from light size

## Implementation steps
1. **Forced depth prepass.** While VSM is active, force the depth prepass in `_render_scene` (`render_forward_clustered.cpp:2207-2230`), as the NextGI prepass does.
2. **Mark pass.** `vsm_mark_pages.glsl`, one thread per pixel, dispatched in `_pre_opaque_render` (`render_forward_clustered.cpp:1581`) before the shadow loop at :1635. It iterates the pixel's clustered lights (the same cluster bits that `light_process_omni` uses at `scene_forward_lights_inc.glsl:466`) plus every directional light. It projects to each map, picks the mip from the screen-space derivative of the shadow UV, and sets REQUESTED with `atomicOr` on the page-table entry.
3. **Coarse marking.** The volumetric-fog froxel grid and transparent bounding rects request pages at the map's coarsest 2 mips only.
4. **Allocate pass.** `vsm_allocate_pages.glsl`: entries that are REQUESTED and already ALLOCATED stay put. New requests take pages from an LRU ring stored in the pool header. Pages not requested for N frames (N set by the quality preset) return to the ring. Clipmap SCROLLED pages are re-rendered, not reallocated.
5. **Overflow.** If free pages run out, write an overflow count. Next frame, raise the LOD bias of the top requesters by 1 mip (at most 3), and decay it back by 1 mip per 30 frames when pressure ends.
6. **Indirect args.** Emit per-map lists of dirty pages and the indirect dispatch arguments for raster, through #177.

## Acceptance criteria
**Build (PR basics, §22):**
- [ ] Builds on the touched CI targets (Windows and Linux Vulkan, Windows D3D12, macOS Metal); CI, unit tests and doctests pass, with new tests added.
- [ ] 0 validation errors on the phase smoke scene (§22); it renders without crashing on the RTX 4080 SUPER and on the M5 Max.
- [ ] With the pool forced to 25% of default, the OVERFLOW counter is non-zero.

**Measured in PQ:**
- [ ] GV-2: VSM Flythrough, 1080p, 10,000 frames at default pool: 0 sampled-but-unmapped pages (debug counter) and 0 validation errors.
- [ ] GV-1: ≥95% of shadowed screen pixels sample a texel footprint ≤1.0 screen pixel.
- [ ] GV-2: with the pool forced to 25% of default: 0 corrupted pages; Q7 degrades by ≤0.05.
- [ ] GV (phase task criterion): Budget V1 (mark + allocate, 1080p, sun + 16 local lights), reported on the RTX 4080 SUPER and the RTX 5090: report-only target: 0.10 ms on the RTX 4080 SUPER (the gate's UE bar decides, §23). V1 is a per-pass line; GV-3's Virtual Shadow Maps total on Dense City carries the UE bar.

## Validation
At PR time:
- `vsm/page_marking.gd` compares requested pages with a CPU reference for 8 fixed camera poses: 100% match.

In PQ, with GV:
- The 10,000-frame and 25%-pool runs (GV-2), the GV-1 footprint metric, and the V1 timing on the RTX 4080 SUPER and the RTX 5090 (report-only; GV-3 carries the UE bar, §23).

## Parallelization
Lane: shadows. Branch `feature/pv-page-marking`. Open a PR. Parallel with #216.

## Out of scope
- Raster into the pages.

