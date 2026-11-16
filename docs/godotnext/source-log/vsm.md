# Virtual Shadow Maps source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file records every
outside input used to write the Virtual Shadow Map (VSM) code, and the values and contract
decisions that the plan and the issues leave open.

## Approved inputs

None so far. The VSM core (#220) was written from the plan (§21.3, contract S2, budget V6 and
the risk table in §21.3.5), the issue and Godot's own light and shadow code. No outside
document, paper or code was consulted. Issue #220 mentions the virtual size and page size that
UE documents only to say they are not to be copied; the provisional values below differ from
both.

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
