/**************************************************************************/
/*  vsm.md                                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

# Virtual Shadow Maps source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records every
outside input used to write the Virtual Shadow Map (VSM) code, and the values and contract
decisions that the plan and the issues leave open.

## Approved inputs

The VSM core (#220) was written from the plan (§21.3, contract S2, budget V6 and the risk table in
§21.3.5), the issue and Godot's own light and shadow code. No outside document, paper or code was
consulted. Issue #220 mentions the virtual size and page size that UE documents only to say they
are not to be copied; the provisional values below differ from both. Page marking and allocation
(#219) were written with no outside input; one public page was read afterwards, for the review:

- Epic, Virtual Shadow Maps in Unreal Engine
  (https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine,
  read 2026-11-16 for the #219 review and its always-resident coarse-page follow-up). Used to state
  UE's documented behavior and align #219's mip-footprint wording and coarse-page residency with it
  (lead decision 2026-11-16, follow UE): "Appropriate mip levels are picked by projecting the size
  of the screen pixels into shadow map space"; for directional lights "the resolution allocated to a
  given pixel is a function of the distance away from the clipmap origin — the camera"; coarse pages
  give "at least low-resolution shadow data ... across the whole domain" ("Local lights simply mark
  the root mip page while Directional Lights can mark a series of pages at various low detail levels
  to form some coarse clipmaps"); and the page pool's allocated pages "should always be less than
  Max pages, otherwise corruption can occur". No UE source or shader was opened.

## Provisional sizes (#220), 2026-11-16

The derivation in #220 step 1 runs in PQ (§22) and replaces these values; until then they are
derived from Godot's own defaults and from budget V6. They live only in
`servers/rendering/storage/virtual_shadow_map_storage.h`; shaders get them as generated defines.

| Value | Provisional | Derivation |
|---|---|---|
| Virtual size per map | 4096 texels | Godot's default directional shadow atlas size (`rendering/lights_and_shadows/directional_shadow/size`), so a map at mip 0 is never coarser than the largest legacy shadow it replaces. |
| Page size | 64 texels (64 x 64 pages per map, 7 mips) | The largest power-of-two page count per side for which the page table of the V6 case fits 16 MiB: 64 omni lights (6 maps each) and a directional light with all 16 clipmap levels are 400 maps; a full mip chain of 128 x 128 pages would need 33.3 MiB for them, 64 x 64 pages need 8.3 MiB (10.7 MiB with the table's 128-map growth steps). |
| Page-table entry | 32 bits: page x and y (12 bits each), 8 flags | Contract S2. |
| Pool pages, Low / Medium / High / Epic | 2048 / 4096 / 8192 / 16384 | V6's 64 / 128 / 256 / 512 MiB divided by a page's 32 KiB (64 x 64 texels, 4 bytes, 2 layers). The texture grids are 64 x 32, 64 x 64, 128 x 64 and 128 x 128 pages, at most 8192 texels per side. |
| Page table growth | 128 maps (2.7 MiB) per step, at most 4096 maps | 128 maps hold a directional light with 7 levels and 20 omni lights; the cap keeps the table at 85.3 MiB. |
| Clipmap level 0 | 4 m square, doubling per level, at most 16 levels | At Godot's default 75° vertical field of view and 1080 rows, a pixel 1 m away covers 1.4 mm; level 0's texel is 4 m / 4096 = 0.98 mm. The default 100 m directional max distance takes 7 levels. |

## Contract decisions (#220), 2026-11-16 — for review

| Topic | Decision | Why |
|---|---|---|
| Clipmap addressing | Toroidal: a world position's virtual texel is its absolute texel modulo the virtual size, at every mip. A level's window origin snaps to whole mip 0 pages and is centered on the camera's page; at mip m the window starts at the floor of the origin divided by 2^m. A move marks `SCROLLED` on the allocated entries of the columns and rows that wrap, per mip; a turned light marks every allocated entry. | Contract S2 and #220 step 4: a camera move re-renders only the pages that wrap. |
| Clipmap lookup | `vsm_lookup_clipmap(table_maps, map, level_origin, texel, mip)` takes integers: the level's mip 0 page origin and the absolute mip 0 texel. It misses outside the level, and falls back to a coarser mip only while the texel's page at that mip lies in the mip's window, which holds while (origin mod 2^m) + the page's offset in the level is below the pages per side; once outside, every coarser mip is outside too. `vsm_lookup()` is for spot, omni and area maps only. | When the origin is not a multiple of 2^m, the level's far edge is outside the mip m window and its slot holds the near edge, 4 m away at level 0. Falling back to the next clipmap level instead, and getting the level origins to the shader, belong to #222 (projection) with #216's per-map data. |
| Map guards | The lookups take the page table's map count from the caller and miss on a map that is `VSM_MAP_NONE` or not below it, as the CPU model does. `vsm_light_map(vsm_map_base, vsm_flags, index)` gives a light's level or face, or `VSM_MAP_NONE` for a light without maps or an index past its count. | `VSM_MAP_NONE` + 1 wraps to map 0. The count stays outside the buffer so that map offsets and the V6 byte sizes stay exact; consumers (#222) bind the count with the buffer. |
| Map storage | Each map owns a fixed mip chain (5461 entries). A light's maps are one contiguous range, so `vsm_map_base` plus the level or face is the map. First fit; the table grows at its end. | One base index per light in `LightData`, and no per-map header. |
| Freed maps | `map_free` clears the light's entries in the same frame (a buffer clear recorded before any later pass). | #220 step 6. The allocation pass (#219) must also return the physical pages those entries held to its free ring; #220 does not track physical pages. |
| Lights out of view | Every view stamps the lights it draws with the renderer's frame number. A light drawn in neither this frame nor the one before is stale; when a light needs maps that do not fit, stale lights are evicted, least recently used first, before the table grows. | A light that leaves the view keeps its maps (and cache) while there is room, and the table stays at the size of the lights in view: 200 shadowed omni lights with 64 in view keep the 512-map table of V6's 64 lights. |
| Directional level count | From the light's shadow max distance alone (all 16 levels when it is 0), never clamped by a view's far plane. | Views with different far planes would otherwise ask for different counts and reallocate the levels every frame. Levels past the far plane get no requests and hold no pages. |
| Pool changes | `pool_create` keeps the previous pool when the new one cannot be created. A new pool holds none of the old pages: #219 must drop every entry's page when the pool's reallocation count changes. | A failed switch (no memory for Epic) leaves a working pool, not a stale page count with no texture. #220 rasterizes no pages, so nothing is lost yet. |
| Turning VSM on | No setting yet: #221 adds `shadow_method` and the tier rule. `GODOT_DEV_VSM` (`1`, `low`, `medium`, `high`, `epic`) in builds with `DEBUG_ENABLED` turns the core on for every shadow-casting light, as `GODOT_DEV_RR_GUIDES` does for the RR guides. The lights keep their atlas shadows. | The core can be run and checked before the product surface exists, and VSM stays off by default. |
| Clipmaps per view | One set of clipmap levels per directional light instance, moved by every non-probe view that draws it; reflection probes reuse them. | Views with different cameras (several editor viewports) re-scroll the levels each frame. Per-view clipmaps are a decision for #219 and #216. |

## Page marking and allocation (#219), 2026-11-16

The initial pass followed the plan (§21.3 contract S3, the risk table's overflow deviation, budget
V1), issue #219, #220's data model and Godot's own renderer: the cluster bits of
`light_process_omni()`, the froxel placement of `volumetric_fog_process.glsl` and NextGI's depth
reconstruction (`NextGIView`). The follow-up added always-resident coarse pages for every shadowed
light in view, following Epic's public VSM documentation page listed above (lead decision
2026-11-16).

### Provisional values (#219)

They live only in `servers/rendering/storage/virtual_shadow_map_storage.h`, and PQ (§22) replaces
them with measured ones.

| Value | Provisional | Derivation |
|---|---|---|
| Frames a page stays cached without a request, Low / Medium / High / Epic | 15 / 30 / 60 / 120 | A quarter, half, one and two seconds at 60 frames per second: a camera that turns back within that time finds its pages. |
| Always-resident pages | Local faces: root page (mip 6, 1 page per face); directional levels: mips 4 to 6 (21 pages per level) | A level's window lies within pages 16 to 48 of the next level's, and a level's mip m window holds pages [0, 64 - (origin mod 2^m)): mip 4 is the finest mip whose window always holds the whole finer level (64 - 15 = 49 > 48). |
| Coarse mips (fog and transparents) | Local faces: the 2 coarsest (mip 5: 2 x 2 pages, mip 6: 1 page); directional levels: mips 4 to 6 (`CLIPMAP_RESIDENT_MIP = 4`), plus `< 4` in the last level's far strip | Issue #219 step 3, extended to mip 4 (when `origin mod 32 == 31`, mips 5 and 6 end 1 page past the camera in every level) and the last level's far strip. |
| Overflow bias | Up to 3 mips; one back per 30 frames without overflow | Issue #219 step 5. |
| "Top requesters" under overflow | Every map with at least half the largest map's request count | Each overflowing view takes about a quarter of the pages away from the maps that asked for the most. |
| Request list | 4 entries per pool page, at least 65,536 | Requests past the pool cannot all be served; the list holds four times the pool before it drops any (counted). |
| Transparent boxes per view | 1,024; more merge into the last | A bound on the box pass; merging keeps it conservative. |
| Face near plane | 0.001 x the light's range, 1 mm to 5 cm | Points closer to the light than this request nothing. |
| Spot map field of view | 2 x the spot angle, 1 to 179 degrees | The square frustum holds the cone; wider cones are clamped. |

### Contract decisions (#219), 2026-11-16

| Topic | Decision | Why |
|---|---|---|
| Where marking runs | At the end of `RenderForwardClustered::_pre_opaque_render()`, after `update_light_buffers()` and `bake_cluster()`, not before the atlas shadow loop as the issue says. | The pass reads the cluster bits and `LightData::vsm_map_base`, which are only built there. The atlas shadows are not affected; raster (#217, #218) runs after allocation. |
| Which views mark | Every mono Forward+ view with render buffers (not reflection probes) while VSM is on; each forces the depth prepass (and the MSAA depth resolve). | Contract S3: requests come from the screen. Multiview is left to a later task. |
| Per-map data | One `VSMMapData` record per map (96 bytes): directional levels as a light-plane transform plus the window origin, faces as a perspective clip transform. Built on the CPU in `update_light()`, uploaded when it changes. | Marking must project into each map. This is the per-map matrix buffer of #216 step 1, built here because #219 needs it first; #216 and #222 read the same records. |
| Faces | Spot: 1 face along the light's -Z. Omni: 6 faces of 90 degrees along the light basis' +X, -X, +Y, -Y, +Z, -Z. Area: the same without +Z (the back). | Contract S2's map counts. A point on a face edge requests both faces. |
| Pixel footprint | The world size of one internal-resolution pixel at the point's view depth, on the finer axis, compared with the map's texel; how the surface faces the light is not used. | Matches UE's documented rule ("projecting the size of the screen pixels into shadow map space"; directional resolution by distance from the camera). Kept by lead decision 2026-11-16 (#219 step 2 reworded). On surfaces at a grazing angle to the light the texel stretches along the light's direction, and with upscaling a texel covers about 1 / scale output pixels; both count against GV-1, measured in PQ. |
| Mip choice | Faces: the coarsest mip whose texel (texel scale x w x 2^mip) is not larger than the footprint, by exact comparisons. Clipmaps: level + mip is the footprint's float exponent plus 10; the finest level that holds the point takes the rest as its mip; the map's overflow bias is added after; a page outside that mip's window moves to the next level (or clamps into the window in the last level). | Exact comparisons and exponent bits (no `log2`) keep the GPU and the CPU model in step, and clamping in the last level keeps its far strip mapped when its overflow bias rises. |
| Which lights | The pixel's cluster bits (omni, spot, area) with the light's range, cone and front-hemisphere tests, plus every directional light within its shadow max distance. The instance layer mask is not applied (the pass sees no instances). | A superset of what the forward loop shades. |
| Always-resident coarse pages | `MODE_RESIDENT` runs before the pixel, froxel and box passes with a barrier after it: every light in the view's light buffers with maps requests the root page (mip 6) of each spot, omni and area face, or every page of mips 4 to 6 (21 pages) of each directional clipmap level. In the last clipmap level outside mip 4's window, pixel, froxel and box marking also request as coarse the coarsest mip (< 4) whose window holds the point or box strip. | Lead decision 2026-11-16 (follow UE). Every pixel, fog froxel or transparent box request whose fine page overflows (or has sky behind it) still finds a mapped fallback page: its face's root page, the resident mips of its level or the next clipmap level, or the last level's far-strip coarse page. |
| Coarse pages (fog and transparents) | Fog: every vertex of the froxel grid requests the coarse pages of the lights the fog samples (fog energy above 0). A local light counts when it is within its range plus one cell diagonal of the vertex, with no cone or hemisphere test, since the fog shades points inside the cells (a small light inside a cell reaches none of its vertices); a vertex within one cell diagonal of a light requests every face. Transparents: each transparent instance's world box requests the coarse pages its corners span in every level or face it reaches (all of a face's coarse pages when the near plane cuts the box). Directional: mips 4 to 6 (`CLIPMAP_RESIDENT_MIP = 4`) each in the finest level whose window at that mip holds the point, plus in the last level's far strip (outside mip 4's window) the coarsest in-window mip (< 4). | Grid vertices and box corners bound every point the fog and the transparents can sample. When `origin mod 32 == 31`, mips 5 and 6 end 1 page past the camera in every level, so mip 4 is needed to cover the level; and in the last level's far strip behind sky (`MODE_PIXEL` inactive), `< 4` coarse marking keeps fog and transparents mapped. #215's coarse lookup uses the same rule (`clipmap_coarse_sample()`). |
| LRU free list | The pool header holds, per physical page, its owning entry, the frame it was last requested and its bucket. Each view rebuilds the free list: free pages first (never used, orphaned, dropped), then cached pages least recently used first. A page not requested for the cache frames, or scrolled to new ground without a request, is dropped. New requests take free pages first and cached pages only when those run out; a page requested earlier in the same frame is never taken. | Issue steps 4 and 5 (accepted by lead decision 2026-11-16). Taking an idle cached page beats overflowing; pages dropped after N frames are the ring's free part. Because active views request their always-resident coarse pages every frame, `MODE_UPDATE` keeps them and never puts them on the free list. |
| Request order | Three phases in `MODE_ASSIGN`: the always-resident requests (`[0, STATE_RESIDENT)`) first, then the other coarse requests (fog, transparents and the last clipmap level's far strip), then fine requests. | Always-resident coarse pages get physical pages before any other request, so fine-request overflow never leaves a view sampling an unmapped page (`SampleCheck` in `page_marking.gd`). |
| Scrolled pages | A requested entry with `SCROLLED` keeps its page and goes to the dirty list (re-rendered, not reallocated). | Issue step 4. |
| Flags | The emit pass clears `REQUESTED`, `COARSE`, `DIRTY_STATIC`, `DIRTY_DYNAMIC` and `SCROLLED` of every requested entry, and hands the dirty ones to the dirty list. `OVERFLOW` stays on an entry left without a page until it gets one. | Each view starts clean; the dirty list carries what raster needs. |
| Dirty list | Per page: map, mip and page, physical page, flags. Each map's pages together, directional maps first, with per-map offsets and counts in the map state buffer, and dispatch arguments (one workgroup per page) for directional pages, local pages and all pages. | Issue step 6 (scope split accepted by lead decision 2026-11-16). #217 runs one chain for directional and one for local maps; #216 batches all local maps. Draw-count arguments belong to the raster tasks, which build them per (instance, page) on #177. |
| Page clear | One workgroup per dirty page, dispatched from those arguments, clears the layers the flags name to the far plane (0 in reversed Z). The GPU check fills the pool with a non-zero value before allocation, reads it back after the clear, then restores the pool and clears again. | Raster then only adds depth with `imageAtomicMax`; it also exercises the indirect arguments on every driver. |
| Overflow | Per map, not per light: the bias of a directional level applies within that level. Fine requests only. Counted per frame, not per view: a map rises at most once per frame however many views overflow, only a frame's first marked view counts the 30-frame decay, and an overflow in any view of the frame restarts it. Freeing a light's maps clears their bias. Overflowed fine requests fall back to the always-resident coarse pages in that view, and the bias reacts from the next view (issue step 5's "next frame"). `GODOT_DEV_VSM=25%` sizes the pool at a quarter of the default High pool for the overflow test. | Issue step 5, with the plan's stated deviation from UE (raise the bias instead of corrupting pages) plus UE's always-resident coarse-page fallback (lead decision 2026-11-16). |
| Pool change | The next marked view clears the page table and the pool header. | #220's hand-off: a new pool holds none of the old pages. |
| D3D12 GPU-based validation | Every atomic's result is used. | #277: with GPU-based validation, atomics with unused results do not land without `25.3.1-gn3` (#279). |
| Check tolerance | `page_marking.gd` fails on a request that 1e-5 relative changes of the inputs cannot explain, and fails on any sampled-but-unmapped pixel request; exact matches are reported per capture (`exact_captures`). | Accepted by lead decision 2026-11-16. GPU float arithmetic may legitimately round a boundary pixel differently from the CPU model, while every confirmed pixel request must find a mapped page. |
| Clipmaps per view | Unchanged from #220 here: one set of clipmap levels per directional light, moved by every view that draws it. #216 owns per-view directional clipmaps. | Lead decision 2026-11-16 assigns per-view clipmap ownership to #216 alongside reusing `VSMMapData`. |

## Non-Nanite per-page culling and indirect raster (#218), 2026-11-17

The code follows plan §21.3 (contract S4, non-Nanite half; GV-6; budget V3), issue #218, #220's pool
and table layout, #219's dirty-page list and `VSMMapData`, #177's `draw_list_draw_indirect_count`
and Godot's own shadow and SDF passes in `SceneShaderForwardClustered`. No outside code was
consulted. One public page was read on 2026-11-17 to settle the open choices by following UE (lead
decision 2026-11-16): Epic, Virtual Shadow Maps in Unreal Engine (the page listed under Approved
inputs). Used: UE culls non-Nanite instances that overlap no required page ("Page Mask Culled:
Number of instances removed due to not overlapping any required pages"); "Non-Nanite objects still
respect the regular shadow CPU culling settings, such as `r.Shadow.RadiusThreshold`"; the static
and dynamic depth copies are chosen at runtime ("If a mesh has Movable mobility but is not updated,
it will be moved to the static cache"; the layer "does not primarily rely on object mobility"); and
skeletal animation and World Position Offset change shadows every frame. No UE source or shader
was opened.

### Provisional values (#218)

| Value | Provisional | Derivation |
|---|---|---|
| Small-caster threshold (`DEFAULT_MIN_CASTER_TEXELS`) | 1.0 virtual texel at the target page's mip (0 disables) | A caster whose world box is smaller on every axis than one texel of the page (`texel_scale * 2^mip` for clipmaps; `texel_scale * max(min_w, near_w) * 2^mip` for faces, the box's nearest `w`) covers less than one shadow texel there. UE's equivalent is the regular shadow radius threshold, which non-Nanite casters keep under VSM. PQ tunes it with V3. |
| Instance-page pair list (`raster_pair_capacity`) | `max(pool_pages * 64, 262144)` `uvec4` pairs, grown after an overflow up to 4,194,304 (`VSM_PAIR_CAPACITY_MAX`, 64 MiB) | 64 pairs per physical page (8 MiB at High), plus two counters per pair (4 MiB at High) that only a raster check writes. Pairs past the end are never written out of bounds: their pages are marked to be drawn again (see Pair list overflow below), and the list grows to the next power of two of 1.25 times the frame's pairs once the count is read back. Debug builds start the list at `GODOT_DEV_VSM_PAIR_CAPACITY` pairs instead, to test the overflow path. |

### Contract decisions (#218), 2026-11-17

| Topic | Decision | Why |
|---|---|---|
| Pool depth encoding (`depth_pack` / `depth_unpack`) | An order-preserving map of the float into `(0, 0xFFFFFFFF)`: nearer (smaller) linear depth along the light gives a larger `uint`, so `imageAtomicMax` on `R32_UINT` keeps the nearest caster. `0` is the cleared, empty texel (`+INF`); `0xFFFFFFFF` stays reserved for the #277 sinks. Clipmaps store the signed depth along the light (row 2), faces store `w`. | #219 clears dirty layers to `0`. One map serves every map kind at full float precision. #217 and #222 share `vsm_raster_inc.glsl`. |
| Raster | `SHADER_GROUP_VSM` (`PIPELINE_VERSION_VSM_DEPTH`, off until VSM runs) of the forward clustered scene shader draws into an attachmentless 64 x 64 framebuffer. One indirect-count draw per group (pipeline, vertex and index arrays, transforms and material sets, cull variant); each instance is one (element, sub-instance, dirty page) pair. The vertex shader places the vertex in its pair's page (`vsm_page_clip_position`); the fragment shader, after the alpha discard, writes layer 0 or 1 with `imageAtomicMax`, skipping helper invocations. A material's `DEPTH` write is dropped. Cull variants follow the atlas pass (no Y flip: reversed, the light's reverse-cull flag and the instance's mirror flip it; double-sided shadows stay double-sided). | The scene shader runs Godot's own skinning, blend shapes, MultiMesh and particle transforms, `VERTEX` code and alpha scissor and hash, so every class casts as it does into the atlas. |
| Instance culling | `vsm_instance_cull.glsl` culls single-instance boxes, and every MultiMesh (3D and 2D) and GPUParticles3D sub-instance read from its transform buffer (trails included, inactive particles rejected), against each dirty page of each of the light's maps, then builds the indirect-count arguments with a prefix sum. A last pass clamps each group's instance count to the pairs its write pass wrote, so a count pass and write pass that disagree cannot draw stale pairs (the check fails on any difference). `cull_box_dirty_pages()` and `sub_instance_box()` are its CPU models. | Issue steps 2, 3 and 6; UE's page-mask culling. |
| Cull boxes | A single-instance caster's box is the scene cull's world box, which already holds `extra_cull_margin` and `Mesh.custom_aabb` (the margin is not added twice). A sub-instance's box is its mesh's `Mesh.custom_aabb`, or else the surface's box, through the instance and node transforms, plus `extra_cull_margin`. When the shadow material writes `MODELVIEW_MATRIX` (Billboard Enabled, Y-Billboard, Particle Billboard), the box of a sub-instance or of a single instance (a MeshInstance3D with a billboard material keeps its mesh's flat box; Sprite3D and Label3D enlarge theirs) also holds the largest `|x| + |y| + |z|` of a local corner, times max(1, the instance's longest world axis), around the instance's world origin, plus the margin: a basis of unit axes, orthogonal or not (Y-Billboard under a rolled camera), keeps the mesh within it, and Keep Scale multiplies by the axis lengths. `MultiMesh.custom_aabb` bounds the whole MultiMesh, not one instance, so it does not grow the instance boxes. | Follows UE: an instance is culled with its mesh's bounds through its transform; World Position Offset beyond those bounds is the content's to declare (UE's material setting; in Godot `Mesh.custom_aabb` or `extra_cull_margin`). Documented limits, which the raster check reports as vertices outside their cull box: `VERTEX` displacement past those bounds, Fixed Size billboards (scaled by their distance to the camera) and any other `MODELVIEW_MATRIX` write that is more than a turn about the instance's origin. |
| Light camera in the VSM variant | The vertex and fragment stages see the pair's map as the camera: `VIEW_MATRIX`, `INV_VIEW_MATRIX`, `CAMERA_POSITION_WORLD` and `NODE_POSITION_VIEW` come from the map's rows (`vsm_map_view()`; CPU model `map_view()`). A face's camera is `face_camera()`; a clipmap level looks along the light from the main camera's position, the clipmaps' center. `VERTEX` then leaves the vertex stage in that view space, as in the atlas, and goes back to world space to be placed in its page. `MAIN_CAM_INV_VIEW_MATRIX` stays the main camera's. | The atlas pass sets the light's camera, and Particle Billboard builds its quad from `INV_VIEW_MATRIX`, so its quads face the light in both paths. With an identity camera they faced world +Z. |
| Instance records (issue step 1) | Per frame the renderer uploads its own cull records (a world box, or a MultiMesh or particle transform buffer with its node transform, each with its element, draw group, light maps and layer) instead of reusing #75's persistent instance records. | #75's records hold NextGeo virtual-mesh instances; non-virtual casters, their MultiMesh and particle sub-instances and their per-light lists have none. Persistent records for them belong with #212's invalidation and #217. |
| Pair list overflow | A pair the full list cannot hold is not drawn; the write pass marks its page `ENTRY_SCROLLED` (keep the allocation, draw again), so the next view that requests the page clears and draws it in full, and an unrequested one is evicted. The cull state is read back asynchronously: the list grows (above), `get_debug_info()` reports `overflow_frames`, `dropped_pairs_total` and `redraw_marks_total` under `non_nanite`, and the first overflow prints one warning. At the 4,194,304-pair limit the pages that do not fit are drawn over the next frames. | Pages are cached until #212, so a silently dropped pair would leave a page without its caster until eviction. Which pairs drop follows atomic order, so marking alone may not converge; growth makes it converge, and the marks keep the cache complete meanwhile. |
| Caster lists | While VSM is active, the scene cull gives the renderer every shadow caster per shadowed light of the view, with no atlas tile cap: for a directional light the cascade casters plus all geometry within the coarsest clipmap level's reach across the light; for a positional light its range box query. Layer and caster-mask culling as in the atlas path. | Issue step 1. The CPU walk is a PQ cost item (V3 and the CPU-submission criterion); #217 and #212 refine it. |
| Static and dynamic layers | Per frame and per caster: the dynamic layer (1) takes casters whose transform changed this frame, mesh instances (blend shapes, skinning), skeletons, particles, MultiMeshes whose buffer changed recently, and shadow materials that write `VERTEX`, animate with `TIME` or read the main camera (`MAIN_CAM_INV_VIEW_MATRIX`: camera-facing billboards, whose shadow turns with the camera); everything else renders into the static layer (0). The check reports each caster's reasons (`dynamic_reasons`). | Follows UE: the layer is chosen at runtime, an unchanged movable object is static, deformation and WPO change shadows every frame. #212 adds invalidation on top. |
| Point sizes | A surface whose shader sets `POINT_SIZE` on a device without point sizes (D3D12) is not drawn into VSM and is counted (`point_size_skipped`); the raster check fails on any, since such a caster still casts in the atlas. | The emulation needs `gl_InstanceIndex`, which the VSM variant uses for the pair. UE has no point-sprite shadow casters. |
| Indirect MultiMesh | A MultiMesh drawn with `use_indirect` is culled and drawn over all of its instances to draw; the instance count that the user's GPU command buffer holds is not read. | Limit until a follow-up: the cull would have to read the command buffer, and the check's CPU model a copy of it. The shadow atlas honors the buffer. |
| Atlas path while VSM is on | Shadow-casting lights keep their atlas pass under #218; issue step 5 (skipping `_render_shadow_append` for VSM lights) and the acceptance row on CPU shadow submission (a drop of 80% or more against the atlas path on Shadow Gallery, measured in PQ) moved to #222, which switches the light loop to VSM and skips the pass (see the #222 section). | Nothing sampled VSM under #218; without the atlas, VSM-on frames would have lost their shadows. |
| GPU check (`virtual_shadow_maps_get_raster_check_result`) | The GPU's pairs are compared with the CPU model: a pair that a 1e-5 change of the box or the threshold cannot explain fails (the #219 rule); exact differences are reported. Every pair's fragments are counted: a dirty page layer with texels but no drawing pair, with fragments but no texel, or with more texels than fragments fails, and so does a shadowed light with maps but no caster list, a caster that the atlas draws for a light but that light's VSM caster list lacks (compared light by light), a `point_size_skipped` surface, or a #277 sink. Every vertex is tested against the box that culled its pair: one outside it fails (`vertices_outside_box`), which is how a cull box that does not hold its geometry shows (the CPU model shares the box, so the pair comparison cannot). Each caster's fragments are reported per light (`light_texels`, keyed by the light's RID id). A pair list overflow fails unless every pair it dropped had its page marked, and a capture in the frame after another fails if a page the earlier one marked was neither drawn again nor evicted; `tests/vsm/non_nanite.gd` fails on any overflow in its normal runs and tests the recovery in its overflow run. Casters with pairs but no fragment are listed; `tests/vsm/non_nanite.gd` fails on any, since its casters are closed or double-sided (it makes ShadowGallery's single-sided wave, which faces away from both suns, cast double-sided, and adds a blend-shape plate, the one class of the issue's row that the gallery lacks). | A back-facing single-sided surface legitimately draws nothing, so only the test, which knows its scene, can require texels for every caster. |

## Product surface: settings, tier rule, scalability, stats and debug draws (#221), 2026-11-19

The code follows plan §21.3 (contract S1 as corrected in v3.9.9, budget V6, GV-2, GV-9), §13.2's
mocked-capability tier tests, issue #221 and Godot's own settings, `Light3D`, `Viewport` debug draws,
`Performance` monitors and editor menus. No outside code was consulted. One public page was read on
2026-11-19 to settle the open choices by following UE (lead decision 2026-11-16): Epic, Virtual Shadow
Maps in Unreal Engine (the page listed under Approved inputs). Used: the Shadow Map Method project setting
and its console variable ("will need to opt-in using the project setting, or console variable
`r.Shadow.Virtual.Enable`"; "New projects use Virtual Shadow Maps by default"); the visualization modes
("Shadow Mask: The final shadow mask that is used by shading", "Clipmap/Mip Level: The chosen clipmap (for
Directional Lights) or mip (for Local Lights) level", "Virtual Page: Visualization of the virtual page
address", "Cached Page: Cached pages are tinted green, uncached pages are red ... Pages where only the
static page is cached (dynamic uncached) are blue", "Nanite Overdraw: Nanite overdraw into mapped pages");
the page statistics (physical pages, allocated pages, which "should always be less than Max pages");
scalability ("The page pool size can be adjusted using `r.Shadow.Virtual.MaxPhysicalPages`", the
resolution LOD biases for directional and local lights "set by Shadow scalability", and their moving
variants: "Lights that aren't moving are largely cached and, as a result, can use a much higher resolution
than moving lights"; "When a light stops moving, it does a gradual transition to the original bias"). No
per-preset value was copied: the page gives none. No UE source or shader was opened.

### Provisional values (#221)

They live only in `servers/rendering/storage/virtual_shadow_map_storage.h` (`default_scalability()`), and PQ
(§22) replaces them with measured ones.

| Value | Provisional | Derivation |
|---|---|---|
| Default preset (`virtual_shadows/quality`) | High | The pool #220 calls the default, and the preset the GV and comparison sets run at (§21.3.3; UE's `sg.ShadowQuality` pinned at High). UE's own default level is Epic. |
| `max_physical_pages` Low / Medium / High / Epic | 2048 / 4096 / 8192 / 16384 | `pool_quality_pages()`: V6's 64 / 128 / 256 / 512 MiB at 32 KiB per page. |
| Resolution LOD bias, directional and local | Low +1, Medium / High / Epic 0 | One mip quarters the pages a view requests; Low's pool is a quarter of High's. Medium keeps 0 and leans on the overflow bias (S3), which raises a map's bias by up to 3 mips when the pool runs out. |
| Moving variants | The static bias + 1 at every preset | A moving light re-renders its pages every frame, so it takes a quarter of the pages. |
| Moving decay | One mip back per 30 frames once the light stops | The overflow bias's decay (#219); UE's "gradual transition". |
| "Moving" | The light's transform changed in the current frame (a directional light's basis only) | The camera's own motion is not light motion: directional clipmaps scroll with the camera. |
| Frame stats views | 16 views per frame | More views of one frame are not counted in `RENDERING_INFO_VSM_PAGES_*`. |
| Debug-draw lights | 256 per view, directional lights first | A bound on the per-pixel light loop of `vsm_debug.glsl`. |

### Contract decisions (#221), 2026-11-19

| Topic | Decision | Why |
|---|---|---|
| One user-facing switch | `rendering/lights_and_shadows/shadow_method` (Atlas, Virtual; default Atlas) is the only user-facing switch. `GODOT_DEV_VSM` stays for debug builds as a developer override that stands for Virtual and also picks the pool (a quality name or a percentage of the High pool), as a console override of UE's `r.Shadow.Virtual.Enable` stands for its project setting. It goes through the same tier rule. The #218, #219 and #220 checks run unchanged with it. | Follows UE: one project setting backed by a console variable. |
| When the setting is read | At startup (`GLOBAL_DEF_RST`), as `nextgi/enabled`. The quality preset and its values are read live: a new page count creates a new pool. | The tier is evaluated once at startup and the pool exists from then on; presets can be compared while running. |
| Per-light methods | `Light3D.shadow_method` and `RenderingServer.light_set_shadow_method()` store PROJECT, VIRTUAL, ATLAS, RAY_TRACED or DISTANCE_FIELD. RAY_TRACED (#141) and DISTANCE_FIELD (#214) resolve to PROJECT and print one warning per method per process. A VIRTUAL light in an atlas project creates the pool when it is set; VSM then stays active. Light storage, the scene cull's caster lists and #218's raster check use the same resolution (`light_uses_vsm()`). `RenderingServer` has the setter and no getter, as for the other light parameters. | Issue step 2; the stored value survives, only its resolution falls back. |
| Tier rule | Forward+, `SUPPORTS_IMAGE_ATOMIC_32_BIT`, `SUPPORTS_DRAW_INDIRECT_COUNT`, first failure wins (`UNSUPPORTED_RENDERER`, `NO_IMAGE_ATOMIC_32`, `NO_INDIRECT_COUNT`). Mobile, Compatibility and a Forward+ device without the VSM storage report `UNSUPPORTED_RENDERER`. One line per process: at startup for a virtual project, or when a light first asks for VIRTUAL. `Light3D` shows the reason as a configuration warning. The dummy renderer (headless runs, imports, exports) draws no shadows and prints neither the line nor the unimplemented-method warnings (`RendererLightStorage::draws_shadows()`). | S1 as corrected in v3.9.9; §13.2's mocked-capability tests. A headless command on a virtual project has no renderer that fell back. |
| UE's floor | Not computed at runtime. VSM without 64-bit atomics (M1 included) is documented as an extension, not validated against UE, in the class reference. | D3D12 reports no 64-bit atomics until #175's shader toolchain emits them, so a runtime label would mark every D3D12 device. |
| New projects | `EditorNode::get_initial_settings()` writes `shadow_method` only when `NEW_PROJECT_SHADOW_METHOD` differs from the setting's default; both are Atlas until #223 flips the constant. | Existing projects keep the default; the flip is one constant. |
| Stats | `RENDERING_INFO_VSM_PAGES_ALLOCATED` (pages holding an entry after the frame's last view: kept, new and cached), `_REQUESTED`, `_RERENDERED` (the dirty list) and `_OVERFLOW`, summed over the frame's views. Each marked view copies its counters to a frame buffer; the next frame reads it back asynchronously. The counters are sampled, as NextGI's (#186): one read-back is in flight at a time, so the frames in between are not counted, and frames whose views run no page marking (a stereo view, a reflection probe) leave the values of the last marked frame. `Performance` shows them under `virtual_shadows/`. 0 without VSM. | Issue step 5, with no GPU stall. The issue's "one frame of latency" is the frame the read starts in; it arrives when the GPU has finished. |
| Debug draws | Overlays drawn after the frame from the view's depth, the page table, the per-map data, the pool and, for VSM_CACHE and VSM_OVERDRAW, a per-page buffer that a compute pass fills from the view's dirty list and non-Nanite pairs. Lookups choose level and mip as the mark pass does and fall back as `lookup()` and `clipmap_sample()` do. VSM_MASK is the raw one-tap depth test of every light with maps, not the filtered SMRT shadow, which #222 owns. VSM_OVERDRAW counts caster draws (instance-page pairs) per page; NextGeo's raster (#217) adds its own when it lands. VSM_CLIPMAP shows the directional level, VSM_MIP the mip. The per-page pass reads the dirty list only when the view marked pages and the pairs only when its non-Nanite cull ran (a view whose lights have no non-Nanite casters skips the cull's clear), and its pair loop is a grid-stride loop capped at `LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X`, since the pair list grows to 1 << 22 pairs, 65,536 groups of 64 (D3D12 takes 65,535). | UE's modes; nothing is added to the raster path. The shared buffers otherwise hold an earlier view's data. |
| Shading while Virtual | Lights whose method resolves to Virtual still shade from the shadow atlas: the atlas pass stays (#218's row above) and the light loop samples VSM only with #222. Their virtual maps feed the debug draws and the stats, at the cost of both paths. The class reference says so on `shadow_method` (project and `Light3D`); #222 removes the note. Superseded by #222 (below), merged on 2026-11-19: the light loop samples the maps and the notes describe that. | The user-facing docs would otherwise describe Virtual as the shading path before it is one. |
| Moving bias in marking | `MapData.lod_bias` (was `pad`) holds a light's extra bias; the mark pass and its CPU model add it to the view's bias (level 0's for directional lights). | Per light, while the configured bias stays per view; GPU and CPU stay in step. |

## Projection and SMRT soft shadows (#222), 2026-11-19

The code follows plan §21.3 (contract S6, GV-1, GV-5, budget V4), issue #222 with step 7 from #218,
#220's page table, #219's marking rules and `VSMMapData`, #218's pool depth encoding, #54's blue-noise
tile and Godot's own light loop, PCSS and shadow-atlas code. The design and every provisional value
below were fixed before any outside input was read. One public page was read afterwards, on
2026-11-19, to check the open choices against UE (lead decision 2026-11-16): Epic, Virtual Shadow Maps
in Unreal Engine (the page listed under Approved inputs). Used: SMRT "works by shooting a number of
rays towards the light source", spread by "Source Radius on local lights or Source Angle on
Directional Lights"; setting the ray count to 0 "disables SMRT and reverts to single-sample hard
shadows"; fewer samples per ray "limit the amount of penumbra softness"; "the per-light Shadow Bias
and Shadow Slope Bias settings apply only to conventional shadow maps". The page says nothing about
Prefiltered Distant, and a web search found no public description of it, so its behavior below is
GodotNext's own (the plan's facts about it are its label and default: Experimental, off). No UE
source or shader was opened.

### Provisional values (#222)

| Value | Provisional | Derivation |
|---|---|---|
| Rays per pixel and light (`smrt_preset_rays()`), Low / Medium / High / Epic | 2 / 4 / 4 / 8, local and directional alike | Rays x steps equal the texel reads of Godot's own PCSS at the soft shadow quality the preset matches (blocker search plus filter: 2 x the penumbra samples): Low = Soft Very Low (4 + 4), Medium = Soft Low (8 + 8), High = Soft Medium (12 + 12), Epic = Soft High (24 + 24). Epic's 8 rays agree with the plan's UE fact (8 rays at Epic); the other counts are unpublished. GV-5 sets the final values in PQ. |
| Steps per ray (`smrt_preset_steps()`) | 4 / 4 / 6 / 6 | The same read budget divided by the rays. |
| Settings | `rendering/lights_and_shadows/virtual_shadows/smrt_rays_local`, `smrt_rays_directional` (-1 to 32) and `smrt_steps` (-1 to 16); -1 (default) takes the active preset's value | One setting each, as the issue names them. Godot's dot suffixes (`.mobile`) are feature-tag overrides, so preset values cannot be stored that way; the preset table stays in code until GV-5. |
| March length cap (`SMRT_MAX_TEXELS`) | 32 texels of the sampled mip across the map, half a page | A ray then reads at most the neighboring page, which bounds its cost (issue step 2). It clamps the penumbra: a directional ray ends at height 32 texels / its slope; a local ray where the angle it spans seen from the light reaches 32 texels. |
| First step (`SMRT_FIRST_STEP_TEXELS`) and spacing | 2 texels above the receiver toward the light, then geometric to the cap, jittered per ray | Geometric spacing keeps contact detail near the receiver and reach for tall casters with few steps. |
| Normal and depth bias (`SMRT_NORMAL_BIAS_TEXELS`, `SMRT_DEPTH_BIAS_TEXELS`) | 1 texel of the sampled mip each; the normal offset scales with the sine of the angle to the light | Godot's own shadow normal bias is in texels of the shadow map times the same sine; the light's Shadow Bias settings do not apply (as in UE). |
| Local reach (`SMRT_LOCAL_MAX_REACH`) | No step past 95% of the way to the light | Keeps steps off the light's own near plane. |
| Prefiltered Distant | Off; the last 2 levels of a directional light with more than 2 levels; a 4 x 4 texel block; minimum variance (0.5 texel)^2; light bleeding cut below 0.2 | The two coarsest levels are where SMRT's rays span the most ground per texel. The block covers the 2 x 2 hard tap's support plus one texel on each side. |

### Contract decisions (#222)

| Topic | Decision | Why |
|---|---|---|
| Lookup signature | `vsm_shadow_directional(map_base, flags, world, normal, tan_half_angle, view_depth, noise)` and `vsm_shadow_local(map_base, flags, world, normal, light_position, radius, area_a, area_b, view_depth, noise)` instead of the issue's single `vsm_shadow(map_base, world, n, light_size, noise_seed)`. | A directional light's level choice and a local light's face choice and ray frame differ; the view depth gives the pixel footprint the marking pass used, and an area light's rectangle needs its two edges. |
| Level, face and mip | The receiver reads the level, face and mip the marking pass requested for it (`clipmap_projection_select()`, the CPU model of the fine request in `mark_directional_point()`; faces as in `mark_local_point()`), then falls back to coarser mips and, for clipmaps, the next levels; a sample with no page anywhere is lit. | Issue step 1. The resident coarse pages (#219) make an unmapped sample rare. |
| SMRT occlusion rule | A ray is occluded at the first step whose point lies behind the stored depth by more than the depth bias; the shadow factor is the share of unoccluded rays. R = 0, or a light without size, takes one 2 x 2 filtered tap. | The issue's rule. It sees a caster only where a step lands below it inside its footprint, so a receiver under a caster is shadowed by every ray that leaves under it: the penumbra lies mostly outside the hard shadow's edge. GV-5 measures the width error in PQ. |
| Noise | #54's 64 x 64 RG8 blue-noise tile (`nextgi_blue_noise_tile`) per pixel, offset by the R2 sequence of `taa_frame_count`; the disk samples are stratified in radius and turned by the golden angle. | Issue step 2. Without TAA `taa_frame_count` is 0 and the noise is static, as Godot's PCSS. |
| Call sites | `light_process_omni`, `light_process_spot`, `light_process_area` (shadow and transmittance) and the directional loop branch on `VSM_LIGHT_FLAGS_SAMPLE` (bit 7 of `vsm_flags`) and the `use_vsm` specialization bit; the atlas code is unchanged. Only the Forward+ color variants define `VSM_PROJECTION`. | Issue step 3: with VSM off the flag is never set and the shaders take the atlas code. |
| Which views sample | The views that mark pages (#219) while no NextGeo virtual-mesh instance is live. | NextGeo clusters reach the shadow atlas only until #217 rasterizes them into the pool; while any is live, views keep the atlas path (their pages are still marked and drawn). |
| PCSS | Off for every light the light loop samples through VSM (`soft_shadow_size = 0`); SMRT takes the penumbra from `LIGHT_PARAM_SIZE` (spot and omni radius), the area light's rectangle, and the directional angular size (`size = 1 - cos(diameter)`, so `tan(diameter / 2) = sqrt(size / (2 - size))`). | Issue step 5. |
| Atlas pass (issue step 7, from #218 step 5) | A shadowed light that already holds virtual maps skips its shadow-atlas pass in a view that samples VSM: the scene cull gives a positional light no atlas tile and no caster cull, and the renderer drops a directional light's cascades from its pass lists (the cascades' matrices stay for contact shadows and fades). A light in its first frame with maps keeps the pass that frame. Volumetric fog still samples the atlas (#215 moves it to coarse pages), so a light the fog shows (fog energy above 0 with volumetric fog on) keeps its pass. In a raster check's frame the skipped positional lights still get their caster cull, for #218's per-light diff. | #218 review finding 7. The CPU-submission criterion (Shadow Gallery drops by 80% or more against the atlas path) is measured in PQ. |
| NextGI shadow reuse | NextGI's surface-cache relight reuses a light's atlas shadow only when the atlas pass ran this frame; for a light whose pass is skipped it traces its shadow rays instead. | The relight shader already traces any light without a reused map (`nextgi_reuse_state()` returns -1). |
| Transparent receivers | They sample VSM like opaque ones (the alpha pass uses the same specialization), from the pages #219 marks for their boxes. | Fog and transparents get one coarse tap in #215 (`translucent_quality = 0`); until then they read the full SMRT path. |
| Debug info | `virtual_shadow_maps_get_debug_info()["projection"]`: the last sampling view's lights sampled, atlas passes kept and skipped, rays, steps and Prefiltered Distant. | `tests/vsm/projection.gd` proves from it that no atlas pass ran while the shadows appeared. |
| Prefiltered Distant | `rendering/lights_and_shadows/virtual_shadows/prefiltered_distant` (Experimental, off): receivers in a directional light's last 2 levels take a moments filter (mean and mean square of the stored depths of the 4 x 4 texels around them, Chebyshev's bound with a light-bleeding cut; empty texels lit) instead of SMRT. The moments are taken per lookup from the pool, not stored as a separate moments mip. | The issue's stored mip needs a pass after the raster for every dirty coarse page and a second pool; per-lookup moments give the same filtered result behind the setting without new resources. A stored mip is a V4 cost follow-up if PQ asks for it. |
| Double-precision builds | The light loop takes world positions from `inv_view_matrix`'s rows as Godot's own fragment code does, which in `REAL_T_IS_DOUBLE` builds holds the negated, split origin. | Same limit as the existing fragment code; double-precision VSM is not in v1's test fleet. |
| Static casters that change | Until #212, nothing invalidates a cached static page: a static caster that moves and stops, or whose shadow casting is turned on or off, keeps its old depth in the pages of lights that already hold maps, and with the atlas pass skipped the light loop shows that stale shadow (a lane check on 2026-11-19: the shadow stayed where it was after the caster moved 0.25 m or stopped casting). A new light, or a page that is newly mapped, draws the current casters. `tests/vsm/projection.gd` gives every capture a new light for that reason. | #212 adds the per-page invalidation from swept bounds and per-instance behavior; today virtual shadow maps are only a debug-build switch (`GODOT_DEV_VSM`), and #223 (the default flip) waits for #212. |
| Merged with #221 (2026-11-19) | The light loop samples, and step 7 skips the atlas pass of, only lights whose method resolves to Virtual with VSM available (`light_uses_vsm()`, #221's `resolve_light_shadow_method()` with the tier rule): only those get maps from `update_light()`, so only those get `LIGHT_FLAGS_SAMPLE`, and `vsm_light_skips_atlas()` checks the resolved method too, because a light's maps outlive a change to Atlas until `update_light()` frees them later in that frame. Atlas lights keep their atlas pass and atlas sampling. The projection's level and mip choice adds the light's moving bias (`MapData.lod_bias`, #221), as the marking pass does. The light loop's VSM bindings are set 1, 51 to 56, and `use_vsm` is bit 11 of the specialization's `packed_1`. | AVBOIT (#295) took set 1 bindings 49 and 50 and `packed_1` bits 8 to 10 first; the receiver reads the mip the marking pass requested (the Level, face and mip row). |

## VSM × Nanite per-page cluster cull and hybrid raster (#217), 2026-11-20

The code follows plan §21.3 (contract S4, Nanite half; GV-3, GV-6; budget V2), issue #217, #220's pool
and page table, #219's dirty-page lists (`VSM_STATE_DIRTY_DIRECTIONAL`, `VSM_STATE_DIRTY`, `VSM_INDIRECT_ALL`)
and `VSMMapData`, #218's pool depth encoding (`vsm_raster_inc.glsl`), #76/#82's virtual mesh BVH and
shadow cut (`cluster_cull.glsl`), #162's shared software raster setup (`sw_raster_setup_inc.glsl`),
#177's `draw_list_draw_indirect_count`, and #155's shared raster-feature table (`RASTER_BIN_TABLE`,
`raster_features_inc.glsl`, `raster_material_{inc,hw,sw}.glsl`). No outside source, shader or SDK was
consulted beyond Epic's public Virtual Shadow Maps documentation page listed under Approved inputs.

### Provisional values (#217)

They live in `servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h` (and the provisional
project setting below), and PQ (§22) measures and tunes them against V2 and GV-3.

| Value | Provisional | Derivation |
|---|---|---|
| SW/HW triangle-size switch (`NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS`, `rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels`) | 8.0 shadow texels of the target page's mip (clamped to `[0, 64]`; `0` forces HW) | Matches #162's 8-pixel software-raster switch in the main visibility pass, expressed in shadow texels of the target page's mip; clusters straddling a local face near plane always fall back to HW. Documented as provisional for PQ tuning. |
| Cut error threshold (`NEXTGEO_VSM_DEFAULT_MAX_TEXEL_ERROR`) | 1.0 shadow texel of the target page's mip | Matches P5's `<= 1` shadow-texel rule (#82) at mip 0, scaled by `2^mip` for coarser VSM pages (issue step 2). |
| Per-page 1-level Hi-Z (`NEXTGEO_VSM_HIZ_PAGE_SIZE`) | 8 × 8 cells per 64 × 64 physical page (`R32_UINT`, 256 B/page = 2 MiB at High / 8,192 pages) | One 64-thread workgroup per dirty page reduces each 8 × 8 static-layer texel block in a single pass with zero shared-memory sync; each cell stores the minimum packed reversed-Z `uint32` (farthest linear depth; `0` = empty/never occludes). |
| Per-chain compaction capacities (`NEXTGEO_VSM_{NODE,GROUP,CLUSTER}_CAPACITY`) | 262,144 node pairs, 262,144 group pairs, 524,288 cluster-page pairs per chain | Fits well inside #155's 21-bit raster-bin entry index (`2^21`) and stays within budget while leaving headroom for multi-light dirty frames; overflows mark the affected page `VSM_ENTRY_SCROLLED` so the next view re-draws it cleanly without corrupting the cache. |

### Contract decisions (#217), 2026-11-20

| Topic | Decision | Why |
|---|---|---|
| Dispatch chains | Two chains per frame: one indirect dispatch chain over `[0, dirty_directional)` for all directional clipmap pages and one over `[dirty_directional, dirty_total)` for all local (spot, omni, area) face pages (each 1 chain, <= 2 per frame). | Issue step 1 and #219's directional-first dirty-page layout keep CPU submission constant regardless of light or page count. |
| Hardware raster (`vsm_raster_nextgeo.glsl`) | Vertex-pulling draw into an attachmentless 64 × 64 framebuffer with `vsm_page_clip_position()`, 32-bit `imageAtomicMax(vsm_pool, physical_texel, depth_uint)`, `draw_list_draw_indirect_count` (#177), and `firstInstance = 0`. | Matches #218's depth format and keeps `gl_InstanceIndex` valid on D3D12 below SM 6.8 without `BaseInstance`. |
| Software raster (`vsm_raster_sw.glsl`) | One 64-thread workgroup per `(cluster, dirty page)` pair sharing `sw_raster_setup_inc.glsl`. Directional maps interpolate linear depth directly; perspective local faces interpolate `1/w` (affine in page space) and recover `w = 1 / (1/w)` clamped to `[near_w, max_distance]`. | Exact perspective-correct linear depth for local faces with zero hardware rasterizer overhead on dense micro-clusters. |
| Per-page 1-level Hi-Z | Built post-raster by `vsm_raster_sw.glsl` (`MODE_HIZ_BUILD`, dispatched indirectly from `VSM_INDIRECT_ALL`) from static layer 0 into a dedicated `vsm_hiz` texture and `vsm_hiz_owner` buffer (`entry_idx + 1`). Uncached (`VSM_ENTRY_UNCACHED`), scrolled, static-dirty, unowned, or near-straddling bounds skip Hi-Z occlusion; `vsm_allocate_pages.glsl` and `pages.data` stay untouched so #219's check keeps matching its CPU model bit-for-bit. | Issue step 3 without altering #219's allocator format; static-unchanged cached pages skip redundant Hi-Z rebuilds. |
| Static / dynamic layer hook | Static instances route to layer 0 and dynamic instances (`transform != prev_transform` this frame, `RECORD_RT_SKINNED`, or WPO material, matching #218's runtime rule) to layer 1 when `NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK` is active. Defaults to all-static (layer 0) until #212 lands per-primitive invalidation; an opt-in debug setting (`rendering/lights_and_shadows/virtual_shadows/debug_nanite_dynamic_layer`, off by default) exercises layer 1 and next-frame Hi-Z culling in tests. | Issue step 6: clean hook ready for #212 while avoiding stale dynamic-layer shadows before #212 owns per-page invalidation. |
| Programmable raster bins (#155) | Shares `RASTER_BIN_TABLE` (`RASTER_PASS_VSM`), `raster_features_inc.glsl`, and `raster_material_{inc,hw,sw}.glsl` via Group 2 (`NEXTGEO_RASTER_DEPTH_ONLY` + VSM defines, 32-bit `imageAtomicMax` only — no 64-bit atomics required). Surfaces in `RASTER_BIN_FIXED` always run on the VSM Nanite path; programmable-bin virtual surfaces hand off from `_vsm_raster_non_nanite()` to `_vsm_raster_nanite()` whenever programmable raster bins are active in the view (0 gaps, 0 double-draws). Clusters with `RASTER_FEATURE_VERTEX_OFFSET` skip tight undisplaced sub-bounds and Hi-Z culling so WPO displacement inside `extra_cull_margin` never clips at page seams. | Issue step 7: every PN feature added to `RASTER_BIN_TABLE` automatically gets its VSM depth-only variant in the same table, and VSM programmable raster works on 32-bit-atomic-only GPUs including Metal/D3D12. |
| Projection sampling with live NextGeo | `vsm_sample_view` and `vsm_view_samples()` now enable VSM light-loop sampling and atlas-pass skipping when NextGeo virtual meshes are live whenever `nextgeo_cut.is_vsm_available()` is true (superseding #222's temporary `live_record_count == 0` guard). | NextGeo casters now populate the VSM physical page pool directly; VSM-off projects keep the legacy shadow-atlas path unchanged. |
| Debug draw (`VSM_OVERDRAW`) | `vsm_debug_pages.glsl` adds `MODE_NANITE_PAIRS` so `DEBUG_DRAW_VSM_OVERDRAW` counts NextGeo `(cluster, page)` pairs alongside non-Nanite pairs. | Completes #221's VSM_OVERDRAW contract decision for Nanite overdraw. |
| D3D12 GPU-based validation | Every `atomicAdd`, `atomicMax`, `atomicOr`, and `imageAtomicMax` in the new/updated VSM shaders checks its return value against `0xFFFFFFFFu` and writes `vsm_counters.sink` (word 21). | #277 compliance across Vulkan VVL and D3D12 GBV. |

## VSM caching, swept-bounds invalidation, per-instance behavior and deferred invalidation budget (#212), 2026-11-20

The code follows plan §21.3 (contract S5, GV-3, GV-4, budget V2), issue #212, #220's page table, #219's
page marking and allocation passes, #218's non-Nanite cull/raster, #217's VSM × Nanite cluster cull/raster
(`NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK`), and #222's projection sampling. No outside source, shader or SDK was
consulted beyond Epic's public Virtual Shadow Maps documentation page listed under Approved inputs.

### Provisional values (#212)

| Value | Provisional | Derivation |
|---|---|---|
| Motion cooldown (`CACHE_MOTION_COOLDOWN_FRAMES`) | 8 frames | Fixed by contract S5 / issue #212 step 2: an `AUTO` instance moves to dynamic layer 1 when its transform changes and settles back to static layer 0 once stationary for 8 frames (`frame - last_moved_frame >= 8`). |
| Deferred invalidation budget (`rendering/lights_and_shadows/virtual_shadows/deferred_invalidation_budget`) | `0` (unlimited), range `0..16384` pages/frame (`or_greater`) | Issue #212 summary and GV-3 (which tests budget `0` and `512` in PQ). |
| Screen-coverage priority histogram (`DEFER_BUCKET_COUNT`) | 64 monotonic log-linear pixel-count buckets (`coverage_bucket()`: exact `0..15`, then 2 sub-buckets per power of two up to `2^27` pixels) | Orders deferred `DIRTY_STATIC` candidates highest screen pixel coverage first in O(1) GPU passes (`MODE_DEFER_SCAN` + `MODE_DEFER_APPLY`) with zero sorting buffers or extra allocation memory. |

### Contract decisions (#212), 2026-11-20

| Topic | Decision | Why |
|---|---|---|
| Per-instance behavior API | `RSE::ShadowCacheInvalidation` (`AUTO`, `ALWAYS`, `RIGID`, `STATIC`), `RenderingServer.instance_geometry_set_shadow_cache_invalidation()`, and `GeometryInstance3D.shadow_cache_invalidation` (default `AUTO`). Encoded in `NextGeoInstanceRecord::flags` bits 12–13 alongside resolved `RECORD_VSM_DYNAMIC` (bit 14). | Issue steps 5–6; keeps `NextGeoInstanceRecord` at 224 B and routes both non-Nanite and Nanite VSM paths through the exact same rule. |
| `AUTO` dynamic classification | Pure `RendererVirtualShadowMapStorage::classify_shadow_cache_dynamic()`. Inspects the material's own `ShaderData` (`material_get_vsm_dynamic_flags()`) as well as the shadow shader (`VERTEX`/custom vertex, vertex `TIME`, `DEPTH`, animated, `MAIN_CAMERA`; `DEPTH_OFFSET` joins via `RASTER_FEATURE_DEPTH_OFFSET` when #149 lands), plus 8-frame motion cooldown, skeleton, blend shapes (`mesh_instance`), particles, and dynamic MultiMesh. | `uses_shared_shadow_material()` substitutes the default shadow shader for materials that write `DEPTH` or read `TIME` in `vertex()` without writing `VERTEX`, so checking only `surf->shader_shadow` would miss them. |
| `RIGID` / `ALWAYS` / `STATIC` rules | `RIGID` stays on static layer 0 and emits swept-bounds (`previous ∪ current`) `DIRTY_STATIC` invalidations on transform/AABB/lifecycle changes. `ALWAYS` stays on dynamic layer 1, never writes layer 0, and emits only `DIRTY_DYNAMIC`. `STATIC` stays on layer 0 and never emits invalidations even when moved. Superseded for `STATIC` by the 2026-11-22 proof pass (R5, below): only transform changes are suppressed. | Issue step 2 and acceptance criteria (STATIC never invalidates; ALWAYS never touches layer 0). |
| Swept-bounds & light invalidation (`vsm_invalidate.glsl`) | Runs before `vsm_mark_pages.glsl` in `mark_and_allocate()`. `MODE_MAPS` invalidates all allocated pages of moved/rotated/property-changed lights (`ENTRY_DIRTY_STATIC`, plus `ENTRY_DIRTY_DYNAMIC` only on pages whose `PageRecord::pad` records prior dynamic content); `MODE_BOXES` projects swept AABBs into every active map whose `MAP_STATE_CULL_MASK` overlaps the box's `layer_mask`. | Running before marking lets `MODE_PIXEL` measure exact screen pixel coverage for `DIRTY_STATIC` pages and keeps #219's `get_check_result()` CPU allocation model bit-exact. Tracking per-page dynamic usage in `PageRecord::pad` prevents sun rotation on static scenes from invalidating empty layer-1 pages, keeping GV-3's 512-page budget exact. |
| Unrequested dirty pages | `page_update()` and `vsm_allocate_pages.glsl` (`MODE_UPDATE`) evict unrequested pages carrying `ENTRY_DIRTY_STATIC` (immediately reclaiming pool slots), while preserving unrequested cached pages that only carry `ENTRY_DIRTY_DYNAMIC` (keeping static layer 0 intact until requested again). | Prevents stale unrequested static pages from wasting pool memory while preserving static cache across dynamic caster movement. |
| Deferred invalidation budget | When `deferred_invalidation_budget > 0`, `vsm_mark_pages.glsl` (`MODE_PIXEL`) counts screen pixels on already-allocated `DIRTY_STATIC` pages into `page_coverage_buffer`; `MODE_ASSIGN`, `MODE_DEFER_SCAN` and `MODE_DEFER_APPLY` accept up to `budget` kept non-scrolled `DIRTY_STATIC` pages highest-coverage-first, and `MODE_EMIT` leaves leftover pages flagged `ENTRY_DIRTY_STATIC` without clearing their stale layer-0 depth. Newly allocated and scrolled pages are never deferred. | Issue step 4 and GV-3: zero overhead when `budget == 0`, deterministic highest-coverage-first convergence when `budget > 0`. |
| Pairing & atlas dirtiness | Superseded by the 2026-11-22 proof pass (R1, below): lights keep upstream's `make_shadow_dirty()` calls. Was: VSM lights skip `light->make_shadow_dirty()` on geometry pairing/unpairing and caster changes in `RendererSceneCull`. | Issue step 1. |
| Production Nanite dynamic hook | `_vsm_raster_nanite()` now enables `NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK` in all production frames (completing #217's follow-up note), while preserving the `debug_nanite_dynamic_layer` test toggle used by `tests/vsm/nanite.gd`. | #217 follow-up comment on #212. |

### Proof pass (#212), 2026-11-22

Outside input: Epic's public Virtual Shadow Maps page (UE 5.9 documentation,
https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine, read 2026-11-22;
no revision shown), its "Shadow Cache Invalidation Behavior" list. Used: Static is "In addition to the Rigid behavior,
also suppress invalidations due to transform changes. Add/Remove still triggers invalidations. If the primitive is
moved or animated, the visual result is undefined"; Rigid suppresses the invalidations World Position Offset would
cause; and "Geometry that casts shadows moving, or being added or removed from the scene will invalidate any pages",
including property changes that recreate the primitive's render state. No source, shader or SDK was read.

| Topic | Decision | Why |
|---|---|---|
| `STATIC` and removal (R5) | `STATIC` suppresses only transform changes (each one is counted in `static_mode_invalidations`). Hiding, showing, freeing, leaving the scenario, turning shadow casting off or on and changing the layer mask still invalidate. An instance's cached footprint is replaced by its current bounds only when an invalidation covered it, so a moved `STATIC` instance keeps every bound it was drawn at through later syncs that invalidate nothing (a material change), and its removal clears them all. Changing an instance's `shadow_cache_invalidation` invalidates its footprint and its current bounds, as removing and re-adding it would. | UE's Static, above, and its property changes that recreate the render state. Before, a hidden or freed `STATIC` caster left its shadow in the cache until LRU eviction. |
| Atlas dirtiness (R1) | Lights keep upstream's `make_shadow_dirty()` calls. | A light a view samples through VSM has no atlas pass (#222), so the flag is not read; a light that keeps its atlas pass (volumetric fog, a view that does not sample VSM, a light before its maps exist) must still redraw it. Skipping the call for every VIRTUAL light left those atlas shadows stale. |
| Sun turn with a budget (R3) | A turn still scrolls the pages that leave each level's window (`ENTRY_SCROLLED`, never deferred) and invalidates the rest (`ENTRY_DIRTY_STATIC`, deferrable). | A turn moves each level's window; without the scroll a deferred page's slot could hold a page from elsewhere in light space, not just older depth of the same page. |
| Layer routing (R9) | Non-Nanite casters draw into the layer RendererSceneCull's `classify_shadow_cache_dynamic()` gave the instance (every material of the instance) at its last `_vsm_sync_instance()`, the classification its invalidation boxes were emitted for and the one NextGeo's record holds. The per-frame tick re-syncs dynamic instances when they settle. | Per-surface routing, or a classification computed again at caster collection, could put a caster in a layer whose pages its boxes never invalidate, so it reached only new pages. |
| MultiMesh buffers | A MultiMesh whose buffer changed in the last 8 frames is dynamic under `AUTO`, as an instance that moved in the last 8 frames. `tests/vsm/non_nanite.gd` takes its first capture after that window (frame 12). A buffer change reaches `_vsm_sync_instance()` only through an AABB notification, which a custom AABB suppresses, so `RendererSceneCull::update()` polls the MultiMesh instances in the static cache (not `STATIC`) each frame, before any view collects casters: an `AUTO` one whose buffer changed is synced into the dynamic layer, and a `RIGID` one invalidates its bounds as a move would, in every frame of the 8-frame window. | One cooldown for every kind of change. The poll is O(MultiMesh instances in the static cache) and does nothing with VSM off. |
| Pending invalidations (R8, R10) | A scenario's box list is capped at 16,384 boxes (later boxes merge into the last one, `append_invalidation()`); freed maps drop their pending map invalidations (`prune_map_invalidations()`). A scenario's published boxes are replaced at the next publish only once a page-marking view applied them (`last_applied_invalidation_epoch`, read after each render); otherwise the new ones are appended (`publish_invalidations()`), so a frame whose views do not mark pages drops none. | Bounded memory and pass size for scenarios that are not rendered; no work on cleared entries; no stale page after a frame drawn only by views that do not mark pages. |
| Visibility ranges and HLOD | The directional caster collection records whether an instance's visibility range (or its HLOD parent) hid it; when that changes, the instance's cached bounds are invalidated in its layer, as hiding or showing it would, from the next frame on. Local lights do not apply visibility ranges to casters, as upstream's shadow atlas does not. | UE: adding or removing a caster invalidates. Before, a range swap left the hidden mesh's shadow in cached pages and the shown mesh's shadow only on new pages. |
| Deferred budget per view | `deferred_invalidation_budget` applies in each view that marks pages (`MODE_DEFER_SCAN` takes it whole per view); the setting's description says so. | The pages a view accepts are counted on the GPU, so a per-frame budget across views would need a counter carried between views' passes; GV-3 and the PR-time checks use one view. |
| GPU check | `virtual_shadow_maps_get_check_result()` also compares the invalidation pass with `invalidate_map_model()` and `invalidate_box_model()` (every entry, every page record's layer-1 flags, both counters), returns the CPU model's page counts (`invalidation.box_static_pages_model`: the model's count on the emitted boxes) and the boxes themselves (`invalidation.box_list`). `tests/vsm/caching.gd` checks that each actor frame's one box equals the actor's scripted swept bounds, which makes `box_static_pages_model` the analytic swept-bounds overlap for GV-4's actor line. | R7: the dynamic-if-used path now has CPU/GPU parity; the bound API is unchanged. |

## Distance-field shadows (#214), 2026-11-23

The code follows plan §21.3 (contract S7, distance-field half; GV-10), issue #214, #182's global SDF
clipmaps (`NextGIGlobalSDF`, `NextGIGlobalSDFClipmaps`, `NextGIGlobalSDFGPU`), #185/#186's mesh SDF
atlas and 2 m detail froxel grid (`NextGISWRTTrace`, `nextgi_sdf_trace_inc.glsl`), #218's non-Nanite
VSM cull (`vsm_instance_cull.glsl`), #221's `Light3D.shadow_method` and tier-info surface, and #222's
Forward+ light loop. No outside source, shader, paper, or SDK was consulted.

### Provisional values (#214)

They live in `servers/rendering/storage/virtual_shadow_map_storage.h`, and PQ (§22) measures and
tunes them against GV-10.

| Value | Provisional | Derivation |
|---|---|---|
| Mask resolution | Half internal resolution (`ceil(w/2) x ceil(h/2)`), `R8_UNORM` 2D array | Contract S7 / issue #214 step 1: one cone trace per half-res pixel per DF light, with a matching half-res `RGBA16F` view-normal + linear-depth guide for the 2 × 2 bilateral upsample. |
| Max DF lights per view (`DF_MAX_LIGHTS_PER_VIEW`, `rendering/lights_and_shadows/virtual_shadows/df_max_lights`) | Preset Low / Medium / High / Epic = 2 / 4 / 8 / 8 (hard cap 8; `-1` follows preset) | Contract S7 / issue #214 step 1: max 8 layers per view, scalable by quality preset. |
| Global SDF trace steps (`df_preset_max_steps`) | Low / Medium / High / Epic = 32 / 48 / 64 / 64 | Matches `NextGISWRTTrace::MAX_STEPS` (64) at High/Epic and scales down at Low/Medium. |
| Directional handoff blend band (`DF_BLEND_BAND_FRACTION`) | 10% (`[0.9 * start, start]`) using cubic smoothstep | Contract S7 / issue #214 step 3: 10% blend band across the handoff so there is no visible seam between VSM/CSM and DF far shadows. |
| Near-receiver mesh SDF refinement (`DF_DETAIL_METERS`) | 2.0 m | Issue #214 step 1 and `SWRT_DETAIL_METERS` (#186): refines the first 2 m from the receiver against per-mesh SDFs in the view's detail froxel grid before continuing in the global SDF. |
| Minimum cone half-angle tangent (`DF_MIN_CONE_TAN`) | 0.004 (~0.23°) | Keeps point/zero-radius lights crisp without dividing by zero while letting `light_size` / `light_angular_distance` widen penumbras smoothly. |

### Contract decisions (#214), 2026-11-23

| Topic | Decision | Why |
|---|---|---|
| Half-res guide texture | `df_shadow.glsl` writes both `df_shadow_mask` (`R8_UNORM` 2D array) and `df_shadow_guide` (`RGBA16F` 2D: view-space unit normal + linear depth, layer 0 only) in `_pre_opaque_render()`. The forward color pass reads bindings 57–58 only. | Avoids any depth-buffer read/write hazard during the opaque color pass (where `depth_buffer` is `RB_TEX_BACK_DEPTH`) and works identically for opaque, transparent, and NextGeo compute shading bins. |
| Global SDF coverage up to 2+ km | `NextGIGlobalSDFClipmaps::update_range()` populates levels 6–9 (up to `MAX_LEVELS = 10`, half-extent 3.28 km) on demand when `df_max_distance` exceeds level 5's ~200 m reach, reusing NextGI's `TIER_SOFTWARE` tracer when active or a standalone `NextGISWRTTrace` otherwise. | Reuses #182's existing 10-level brick pool and coarse bound buffer with zero extra work when DF shadows are off or short-range. |
| Directional far-caster VSM drop | `LightStorage::update_light_buffers()` clamps the directional VSM `max_distance` to `distance_field_shadow_start` (keeping `fade_from`/`fade_to` extended to `distance_field_shadow_distance`), and `vsm_instance_cull.glsl` + `cull_box_dirty_pages()` drop non-Nanite casters whose world AABB nearest point to the camera exceeds `distance_field_shadow_start`, counting them in `VSM_CULL_STATE_DF_FAR_CULLED`. | Issue #214 step 3: drops far non-Nanite casters from directional VSM maps while keeping CPU/GPU cull parity in `get_vsm_raster_check_result()`. |
| Tier rule & fallback | `evaluate_df_tier()` requires Forward+ with `rendering/global_illumination/mesh_sdf/generate` enabled and not dropped by HWRT (`keep_when_hwrt == false`). Reasons: `UNSUPPORTED_RENDERER`, `MESH_SDF_DISABLED`, `DROPPED_FOR_HWRT`. Exposed in `virtual_shadow_maps_get_tier_info()` (`df_available`, `df_reason`, `df_fallback_lines`, `df_max_lights`) and `Light3D::get_configuration_warnings()`. When unavailable, directional handoff is ignored so VSM/atlas continues to full distance, and `SHADOW_METHOD_DISTANCE_FIELD` falls back to `SHADOW_METHOD_PROJECT`. | Issue #214 step 5: clear inspector/debug reason and zero impact when DF shadows are off or unavailable. |
| Self-shadowing guard | Soft cone visibility accumulates once the sample clears `2 * band` above the receiver's tangent plane and clamps the effective cone tangent to `max(0.5 * NdotL, DF_MIN_CONE_TAN)`, while hard interior hits (`surface_d <= 0`) still occlude immediately. | Guarantees an unobstructed flat receiver never self-shadows at grazing light angles or wide light radii while preserving crisp contact shadows. |

## VSM local lights, resolution bias, distant-light policy and transmittance (#216), 2026-11-24

The code follows plan §21.3 (contract S7, local-lights half; GV-3, GV-8), issue #216, #220's page-table
map allocator (`MAP_KIND_SPOT`, `MAP_KIND_OMNI`, `MAP_KIND_AREA`), #219's dirty-page lists
(`VSM_INDIRECT_LOCAL`) and three-phase allocation (`MODE_RESIDENT` root pages first, then `ENTRY_COARSE`,
then fine requests), #218's non-Nanite indirect-count raster, #217's NextGeo local indirect chain, #221's
scalability presets, and #222's SMRT/transmittance lookups. No outside source, shader, paper, or SDK was
consulted beyond Epic's public Virtual Shadow Maps documentation page listed under Approved inputs.

### Provisional values (#216)

They live in `servers/rendering/storage/virtual_shadow_map_storage.h`, and PQ (§22) measures and tunes them
against GV-3 and GV-8.

| Value | Provisional | Derivation |
|---|---|---|
| Distant-light screen-radius threshold (`rendering/lights_and_shadows/virtual_shadows/distant_light_screen_radius.{low,medium,high,epic}`) | Low `0.06`, Medium `0.045`, High `0.03`, Epic `0.015` (fraction of viewport height; `0.0` disables) | At 1080p High, `0.03` corresponds to a ~65 px projected light diameter (~32 px radius): below that size a 128 × 128 coarse mip (`COARSE_FIRST_MIP = 5`) or 64 × 64 root page already provides >= 1 shadow texel per screen pixel across the light's footprint while saving fine pages under many-light load. Scales monotonically with the preset pool budgets. |
| Per-light `shadow_resolution_bias` (`Light3D.shadow_resolution_bias`, `LIGHT_PARAM_SHADOW_RESOLUTION_BIAS`) | Range `[-2.0, +4.0]` mips, default `0.0`, quantized as signed 8-bit fixed-point Q3.4 (1/16 mip step) in `vsm_flags` bits `16..23` | Range fixed by issue #216. Packing in `vsm_flags` avoids enlarging `LightData` (240 B) or `DirectionalLightData`; `0x00` encodes `0.0` so lights without a bias keep unchanged `vsm_flags`, and splitting into `floor(q / 16)` + `exp2(rem / 16)` footprint scale matches CPU and GPU bit-for-bit with no `exp2` when `rem == 0`. |

### Contract decisions (#216), 2026-11-24

| Topic | Decision | Why |
|---|---|---|
| Frame-1 atlas bypass & `MAX_UPDATE_SHADOWS` | `RenderForwardClustered::vsm_light_skips_atlas()` returns `true` for VSM-resolved shadowed lights as soon as `VirtualShadowMapStorage::can_allocate_maps(needed_maps)` succeeds (even on Frame 1 before `update_light_buffers()` runs). `RendererSceneCull` skips `_light_instance_update_shadow()` and the 88-slot positional shadow atlas for those lights while keeping frustum, `light_instance_is_shadow_visible_at_position()`, visibility-layer and `shadow_caster_mask` culling in the VSM caster loop, counting `vsm_local_light_count` independently of `MAX_UPDATE_SHADOWS = 512`. | Eliminates Frame-1 atlas fallback and removes the 88-slot positional atlas and 512-shadow cap so 256+ shadowed local lights all cast VSM shadows from Frame 1 onward with 0 atlas passes. |
| Area-light 5-face reach | `VirtualShadowMapStorage::update_light()` passes `range + area_size.length() * 0.5f` as the far plane of `map_data_face()` for `MAP_KIND_AREA` (matching `LightStorage::update_light_buffers()` and `vsm_light_reaches()`). | Prevents area-light VSM faces from clipping shadows short near the rectangle's corners. |
| Many-light single dispatch | All local dirty pages stay concatenated in `dirty_buffer[dirty_directional..dirty_total)` (`VSM_INDIRECT_LOCAL`). NextGeo runs 1 local chain (`local_raster_indirect_dispatches = 2`: 1 SW indirect compute + 1 HW indirect-count draw), and non-Nanite groups across all lights by mesh/material (`indirect_draw_count = groups.size()`), both independent of dirty light count (`<= 4` indirect dispatches for 64+ dirty local lights). | Satisfies S7's many-light single-dispatch requirement on both Nanite and non-Nanite paths without per-light CPU loops. |
| Distant-light coarse policy & overflow safety | Local lights with `local_light_projected_radius(view_pos, reach, cam_projection) < distant_light_screen_radius` receive `LIGHT_FLAGS_DISTANT_COARSE` (bit 10 of `vsm_flags`). Page marking (`vsm_mark_pages.glsl`, `mark_local_point()`) clamps their requested mip to `>= COARSE_FIRST_MIP` and flags `ENTRY_COARSE` so they allocate in Phase 1 (after Phase-0 always-resident root pages and before Phase-2 fine pages). Projection, transmittance and debug draws apply the same clamp. | Keeps distant lights on coarse mips and guarantees 0 sampled-but-unmapped pages even under 25% pool overflow (`GODOT_DEV_VSM=25%`). |
| Subsurface transmittance (`LIGHT_PARAM_TRANSMITTANCE_BIAS`) | `vsm_transmittance_directional()` and `vsm_transmittance_local()` (and their CPU models) return `abs(thickness) >= 0` whenever a mapped, occupied VSM texel exists (`-1.0` only when unmapped/empty), apply `shadow_resolution_bias` and `LIGHT_FLAGS_DISTANT_COARSE`, and for `MAP_KIND_OMNI` and `MAP_KIND_AREA` cube faces convert axial face depth difference `|w - stored|` to Euclidean radial thickness by multiplying by `sqrt(1 + x_ndc^2 + y_ndc^2)`. | Prevents small negative front-face deltas from colliding with the `-1.0` sentinel and matches Godot's dual-paraboloid omni/area SSS radial thickness without cube-corner seams. |
