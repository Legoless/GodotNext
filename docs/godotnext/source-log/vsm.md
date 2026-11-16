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
  read 2026-11-16 for the #219 review). Used only to state UE's behavior in the open questions
  below, not to change code: "Appropriate mip levels are picked by projecting the size of the
  screen pixels into shadow map space"; for directional lights "the resolution allocated to a given
  pixel is a function of the distance away from the clipmap origin — the camera"; coarse pages give
  "at least low-resolution shadow data ... across the whole domain" (local lights mark the root mip
  page, directional lights mark coarse clipmap levels); and the page pool's allocated pages "should
  always be less than Max pages, otherwise corruption can occur". The page says nothing about a
  fallback for missing pages or about upscaling. No UE source or shader was opened.

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

No outside document, paper or code was consulted. The code follows the plan (§21.3 contract S3, the
risk table's overflow deviation, budget V1), issue #219, #220's data model and Godot's own
renderer: the cluster bits of `light_process_omni()`, the froxel placement of
`volumetric_fog_process.glsl` and NextGI's depth reconstruction (`NextGIView`).

### Provisional values (#219)

They live only in `servers/rendering/storage/virtual_shadow_map_storage.h`, and PQ (§22) replaces
them with measured ones.

| Value | Provisional | Derivation |
|---|---|---|
| Frames a page stays cached without a request, Low / Medium / High / Epic | 15 / 30 / 60 / 120 | A quarter, half, one and two seconds at 60 frames per second: a camera that turns back within that time finds its pages. |
| Coarse mips | The 2 coarsest (mip 5: 2 x 2 pages, mip 6: 1 page) | Issue #219 step 3. |
| Overflow bias | Up to 3 mips; one back per 30 frames without overflow | Issue #219 step 5. |
| "Top requesters" under overflow | Every map with at least half the largest map's request count | Each overflowing view takes about a quarter of the pages away from the maps that asked for the most. |
| Request list | 4 entries per pool page, at least 65,536 | Requests past the pool cannot all be served; the list holds four times the pool before it drops any (counted). |
| Transparent boxes per view | 1,024; more merge into the last | A bound on the box pass; merging keeps it conservative. |
| Face near plane | 0.001 x the light's range, 1 mm to 5 cm | Points closer to the light than this request nothing. |
| Spot map field of view | 2 x the spot angle, 1 to 179 degrees | The square frustum holds the cone; wider cones are clamped. |

### Contract decisions (#219), 2026-11-16 — for review

| Topic | Decision | Why |
|---|---|---|
| Where marking runs | At the end of `RenderForwardClustered::_pre_opaque_render()`, after `update_light_buffers()` and `bake_cluster()`, not before the atlas shadow loop as the issue says. | The pass reads the cluster bits and `LightData::vsm_map_base`, which are only built there. The atlas shadows are not affected; raster (#217, #218) runs after allocation. |
| Which views mark | Every mono Forward+ view with render buffers (not reflection probes) while VSM is on; each forces the depth prepass (and the MSAA depth resolve). | Contract S3: requests come from the screen. Multiview is left to a later task. |
| Per-map data | One `VSMMapData` record per map (96 bytes): directional levels as a light-plane transform plus the window origin, faces as a perspective clip transform. Built on the CPU in `update_light()`, uploaded when it changes. | Marking must project into each map. This is the per-map matrix buffer of #216 step 1, built here because #219 needs it first; #216 and #222 read the same records. |
| Faces | Spot: 1 face along the light's -Z. Omni: 6 faces of 90 degrees along the light basis' +X, -X, +Y, -Y, +Z, -Z. Area: the same without +Z (the back). | Contract S2's map counts. A point on a face edge requests both faces. |
| Pixel footprint (open) | The world size of one internal-resolution pixel at the point's view depth, on the finer axis, compared with the map's texel; how the surface faces the light is not used. | A deviation from issue step 2, which asks for "the screen-space derivative of the shadow UV": open for the lead. It matches UE's public description (pixel size projected into shadow space; directional resolution by distance from the camera). On surfaces at a grazing angle to the light the texel stretches along the light's direction (about 5.8 pixels with the sun 10 degrees above the ground), and with upscaling a texel covers about 1 / scale output pixels; both count against GV-1 (texel footprint at most 1.0 screen pixel for 95% of shadowed pixels), measured in PQ. |
| Mip choice | Faces: the coarsest mip whose texel (texel scale x w x 2^mip) is not larger than the footprint, by exact comparisons. Clipmaps: level + mip is the footprint's float exponent plus 10; the finest level that holds the point takes the rest as its mip; the map's overflow bias is added after; a page outside that mip's window moves to the next level. | Exact comparisons and exponent bits (no `log2`) keep the GPU and the CPU model in step. |
| Which lights | The pixel's cluster bits (omni, spot, area) with the light's range, cone and front-hemisphere tests, plus every directional light within its shadow max distance. The instance layer mask is not applied (the pass sees no instances). | A superset of what the forward loop shades. |
| Coarse pages | Fog: every vertex of the froxel grid requests the coarse pages of the lights the fog samples (fog energy above 0). A local light counts when it is within its range plus one cell diagonal of the vertex, with no cone or hemisphere test, since the fog shades points inside the cells (a small light inside a cell reaches none of its vertices); a vertex within one cell diagonal of a light requests every face. Transparents: each transparent instance's world box requests the coarse pages its corners span in every level or face it reaches (all of a face's coarse pages when the near plane cuts the box). Directional: each coarse mip in the finest level whose window at that mip holds the point. | Grid vertices and box corners bound every point the fog and the transparents can sample. A level's mip 6 window covers an aligned half of the level, so a point may have no mip 6 page in any level; #215's coarse lookup must use the same rule. |
| LRU free list | The pool header holds, per physical page, its owning entry, the frame it was last requested and its bucket. Each view rebuilds the free list: free pages first (never used, orphaned, dropped), then cached pages least recently used first. A page not requested for the cache frames, or scrolled to new ground without a request, is dropped. New requests take free pages first and cached pages only when those run out; a page requested earlier in the same frame is never taken. | Issue steps 4 and 5. Taking an idle cached page beats overflowing; pages dropped after N frames are the ring's free part. The GPU check compares the pages handed out per bucket with the CPU model, and a doctest pins the order (free pages, then the oldest cached page). |
| Request order | Coarse requests get pages first, then the rest. | Coarse pages are the always-resident ones the fog and the transparents fall back to. |
| Scrolled pages | A requested entry with `SCROLLED` keeps its page and goes to the dirty list (re-rendered, not reallocated). | Issue step 4. |
| Flags | The emit pass clears `REQUESTED`, `COARSE`, `DIRTY_STATIC`, `DIRTY_DYNAMIC` and `SCROLLED` of every requested entry, and hands the dirty ones to the dirty list. `OVERFLOW` stays on an entry left without a page until it gets one. | Each view starts clean; the dirty list carries what raster needs. |
| Dirty list | Per page: map, mip and page, physical page, flags. Each map's pages together, directional maps first, with per-map offsets and counts in the map state buffer, and dispatch arguments (one workgroup per page) for directional pages, local pages and all pages. | Issue step 6. #217 runs one chain for directional and one for local maps; #216 batches all local maps. Draw-count arguments belong to the raster tasks, which build them per (instance, page) on #177. |
| Page clear | One workgroup per dirty page, dispatched from those arguments, clears the layers the flags name to the far plane (0 in reversed Z). The GPU check fills the pool with a non-zero value before allocation, reads it back after the clear, then restores the pool and clears again. | Raster then only adds depth with `imageAtomicMax`; it also exercises the indirect arguments on every driver. |
| Overflow | Per map, not per light: the bias of a directional level applies within that level. Fine requests only. Counted per frame, not per view: a map rises at most once per frame however many views overflow, only a frame's first marked view counts the 30-frame decay, and an overflow in any view of the frame restarts it. Freeing a light's maps clears their bias. Overflowed requests get no page in that view; the bias reacts from the next view (issue step 5's "next frame"). `GODOT_DEV_VSM=25%` sizes the pool at a quarter of the default High pool for the overflow test. | Issue step 5, with the plan's stated deviation from UE (raise the bias instead of corrupting pages). |
| Pool change | The next marked view clears the page table and the pool header. | #220's hand-off: a new pool holds none of the old pages. |
| D3D12 GPU-based validation | Every atomic's result is used. | #277: with GPU-based validation, atomics with unused results do not land. |
| Check tolerance (open) | `page_marking.gd` fails on a request that 1e-5 relative changes of the inputs cannot explain; an exact match is reported per capture (`exact_captures`) but not required. | Issue #219's validation asks for a "100% match". GPU float arithmetic (fused multiply-adds, division precision) may legitimately round a page, mip or light edge differently from the CPU model on other drivers. All 23 captures matched exactly on the NVIDIA L4 (Vulkan and D3D12). Open for the lead. |
| Clipmaps per view (open) | Unchanged from #220: one set of clipmap levels per directional light, moved by every view that draws it. | Open for the lead (#216 or #222). With pages allocated, two views whose cameras differ (two editor viewports, a SubViewport) scroll each other's levels every frame, so their scrolled pages are dropped or re-rendered every frame and the directional cache does not hold. `tests/vsm/page_table.tscn`'s second view copies the main camera, so no check shows it. Per-view levels (maps per light and view) would fix it at the cost of page-table space. |
