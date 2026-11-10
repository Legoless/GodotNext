# UE comparison settings map

This is the settings record for every UE comparison (`pq-ue-settings-map`, #228). The comparison target stays UE 5.9 final (§25.4).

**Status: interim UE 5.8 record. It does not complete #228.** Public UE 5.9 documentation and a UE 5.9 install were not available on 2026-11-08. The values below are the public UE 5.8 basis that §23 and §25 say stands until the §25.3 re-check. Two parts are outstanding:

- **The UE 5.9 pass (#228, open).** Every setting below is re-checked against public UE 5.9 documentation or an observed UE 5.9 editor setting. On 2026-11-09 Epic's documentation site serves UE 5.9 pages: the console-variable reference is titled UE 5.9, and the UE 5.9 Preview release notes are published. That pass can start on the Preview (§25.4).
- **The stat columns (#229).** No packaged Test build has run. The "Stat or CSV" column names the UE stat group, and the `stat gpu` pass where a public page prints one. `pq-ue-bench-harness` (#229) names the exact GPU-stat or CSV-profiler column of each stat group from a packaged Test build and writes it into this record. #229 also records which Experimental and Beta features run in that Test build, and the DLSS plugin's mode and preset from its editor UI.

A stat group whose column is not named yet keeps the UE bar. If the Test build reports no column for a stat group, the affected lines follow §23.3. A line that §23.6 lists with the UE bar becomes report-only only through a plan revision that updates §23.3, §23.6 and the gate issues together (plan v3.6.1).

No `Engine/Source`, `Engine/Shaders`, plugin `Source`, or `Engine/Config/BaseScalability.ini` was read. No editor UI was observed. A setting below is either a value printed on a public Epic page or an explicit report-only reason.

Since 2026-11-09 an Epic documentation link without `?application_version=5.8` opens the UE 5.9 page. The values in this record were read on the 5.8 pages.

Scalability integers on the [5.8 console-variable reference](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8) use one scale for the quality groups below: 0 low, 1 medium, 2 high, 3 epic, 4 cinematic. That scale matches the [Lumen performance guide](https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine), which prints High as `=2` and Medium as `1`. Low disables Lumen. Cinematic targets Movie Render Queue. The guide's default per-preset tables live in `BaseScalability.ini`, which this record does not read.

## Platform and frame

| GodotNext | UE 5.8 setting | Stat or CSV | Gate lines |
|---|---|---|---|
| D3D12 or Vulkan on Windows | DX12 (SM6), UE's default Windows RHI. Already stated in §23.5. | No timing stat. | Every NVIDIA timing line |
| Metal on the M5 Max | Metal. Already stated in §23.5. | No timing stat. | Every M5 Max timing line |
| 1080p internal, the line's output resolution, `scaling_3d_scale` | The same internal and output resolution. `r.ScreenPercentage` is the percent of the output resolution (default 100, so 100 matches `scaling_3d_scale` 1.0). `sg.ResolutionQuality` is the resolution group (published range 10..100, default 100). The Lumen guide says features use an internal 1080p that TSR scales to the output. | No timing stat. | Every line |
| Whole frame, GPU | The whole frame at the matched settings. | `stat unit` GPU. CSV GPU stats are enabled with `r.GPUCsvStatsEnabled 1` or `-csvGpuStats`. The CSV column header is not named on the pages read. #229 records it. | G5-1 frame line |
| Render thread, CPU | The render thread. | `stat unit` Draw. Same CSV enable flags. The CSV column header is not named on the pages read. #229 records it. | GM-5; GN-13 (parent of its +0.30 ms line) |

`stat unit` names Frame, Game, Draw and GPU on the [5.3 stat-commands page](https://docs.unrealengine.com/5.3/en-US/stat-commands-in-unreal-engine/). The CSV enable flags are on the [4.27 CSV profiler page](https://docs.unrealengine.com/4.27/en-US/TestingAndOptimization/PerformanceAndProfiling/CSVProfiler). `r.ScreenPercentage` and `sg.ResolutionQuality` are on the 5.8 console-variable reference. The 5.4 [scalability reference](https://dev.epicgames.com/documentation/unreal-engine/scalability-reference-for-unreal-engine?application_version=5.4) also names `r.ScreenPercentage`.

## NextGI (Lumen)

Hardware ray tracing for a Lumen row means the project settings **Support Hardware Ray Tracing** and **Use Hardware Ray Tracing when available**, both enabled ([Lumen GI, 5.8](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine?application_version=5.8)). The console-variable reference publishes `r.Lumen.HardwareRayTracing` with default 0, described as using hardware ray tracing when available and falling back to software otherwise. The comparison enables the two project settings. It does not treat that cvar's default of 0 as "on".

The Lumen total, where a row names it, is the sum of the three `stat gpu` passes the performance guide prints: **Lumen Scene Lighting**, **Lumen Screen Probe Gather** and **Lumen Reflections**. No single CSV column for that sum is named. #229 records the header if the Test build prints one, and otherwise sums those passes.

| GodotNext | UE 5.8 setting | Stat or CSV | Gate lines |
|---|---|---|---|
| `quality = high`, HWRT | `sg.GlobalIlluminationQuality=2` and `sg.ReflectionQuality=2`. HWRT project settings enabled, as above. | Lumen total, as defined above. | G3-1 High; G5-1 NextGI sub-line; the parent totals of G3-4 and G3-5 |
| `quality = epic`, HWRT | `sg.GlobalIlluminationQuality=3` and `sg.ReflectionQuality=3` (the published epic integer). Same HWRT project settings. | Lumen total. | G3-1 Epic |
| Diffuse path at High (lines 1a + 2 + 3 + 4 + 5 + 7) | Same settings as G3-1 High. | Lumen GI alone, if UE reports it apart from Lumen reflections over the same work as the diffuse path. The public 5.8 pages name no such stat. **Lumen Screen Probe Gather** alone is not like-for-like: the guide says it evaluates "diffuse global illumination and rough reflections, and translucency global illumination". Surface-cache lighting, which is the diffuse path's line 2, is the separate **Lumen Scene Lighting** pass and also serves reflections. The TLAS rebuild in line 1a is a hardware ray tracing cost, not a Lumen pass. #229 checks the packaged Test build's columns for a stat that covers the same work. | G1-3, G2-4. If no such stat exists, both are report-only and G3-1 High carries the bar (plan §23.5). |
| `quality = medium`, `medium_gather = irradiance_field` (default), HWRT and SWRT | Medium GI: `sg.GlobalIlluminationQuality=1` with `r.Lumen.FinalGatherMethod=0` (Irradiance Field Gather). Medium reflections: `sg.ReflectionQuality=1`, which disables Lumen reflections and uses SSR on smooth surfaces plus rough specular from Lumen GI. HWRT project settings follow the tier: on for the HWRT Lite row, and `r.Lumen.HardwareRayTracing=0` for the SWRT row. Lumen Lite is Beta in the [5.8 release notes](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes). | Lumen total. | GS-7 Lite total on HWRT; GS-2 Medium (Lite) on SWRT |
| `medium_gather = screen_probes` | No equivalent. UE 5.8 Medium is Lumen Lite. | No timing stat. | G3-1 Medium: report-only |
| `TIER_SOFTWARE`, forced (`hardware_ray_tracing = off`) | Software ray tracing, **Global Tracing** (the Global Distance Field). `r.Lumen.HardwareRayTracing=0`. `r.Lumen.TraceMeshSDFs` stays at its published default 0. | Lumen total. | GS-2 at High, Epic and Medium (Lite); GS-4 (M5 Max); GS-9's parent total |
| `software_ray_tracing_mode = detail` | Project setting **Software Ray Tracing Mode** = **Detail Tracing**. `r.Lumen.TraceMeshSDFs=1` (published default 0 since the cvar reference; the plan already records 0 as the default since 5.7). | Lumen total. | GS-9 detail-mode cost: reported, with the UE ratio |
| Global SDF and voxel lighting update | Part of Lumen's scene update. The guide says the Global Distance Field update is cached and depends on instance count. | Report-only. No separate GPU or CSV column is named. The SWRT total carries the bar. | GS-6 |
| `meshes/generate_distance_fields`; `mesh_sdf/keep_when_hwrt` | Project setting **Generate Mesh Distance Fields**. `r.DistanceFields.SupportEvenIfHardwareRayTracingSupported` (published default 1). | No timing stat. | Settings match only. GS-5's import lines stay in seconds. |
| `ray_lighting_mode = hit_lighting_reflections`, Epic | Project setting **Ray Lighting Mode** = **Hit Lighting for Reflections**. The default name is **Surface Cache**. The same names exist on the Post Process Volume. Epic scalability as in the Epic row. The public API enum `ELumenRayLightingMode` prints SurfaceCache=0, HitLightingForReflections=2, HitLighting=1. `r.Lumen.HardwareRayTracing.LightingMode` uses a different 0–3 scale on the console-variable reference. The comparison sets the project setting by name and does not write that cvar. | Lumen total. The +2.0 ms increment stays report-only. | GL-2 |
| `hit_lighting_gi` | Hit lighting for GI. The plan's label stays UNVERIFIED: no project setting for it was found on the pages read. The API name HitLighting is the enum value that calculates lighting at the hit for GI and reflections. That is not a confirmed project-setting path. | Report-only. | GL-4 |
| Far field | `r.LumenScene.FarField=1` (published default 0). The plan already records that this needs World Partition HLOD1. | No timing stat. | GL-5: report-only (no port) |
| Front-layer translucency reflections | Project setting **High Quality Translucency Reflections**, plus the Post Process Volume setting of the same name. The volume setting requires the project setting first. | Lumen total. The increment stays report-only. | GL-6 |
| Translucency GI volume | Lumen GI for lit translucency. **Lumen Screen Probe Gather** includes translucency GI on the performance guide. | Lumen total. GL-7's line-3 fill stays report-only. | G3-5's parent; GL-7 |
| Fog at reflection hits | `r.Lumen.HeightFog 1`. The 5.8 release notes say this is the default. | No timing stat. | Settings match only |
| `nextgi/mode = reflections_only` over LightmapGI | Lumen reflections with lightmaps. Already stated in §23.5. No extra cvar was confirmed. | No timing stat. | GL-8 (quality only) |
| `hardware_ray_tracing.macos = auto` on the M5 Max | Report-only for a separate Mac checkbox. The 5.8 Lumen page names **Support Hardware Ray Tracing** and **Use Hardware Ray Tracing when available**, and does not name an Apple Silicon control. No editor session was run. Those two project settings are what a 5.9 Mac HWRT run uses. | Lumen total, when that Mac build runs HWRT. | G2-4; G3-1 M5 Max High and Epic |
| Mac default (`hardware_ray_tracing.macos = off`) | GodotNext's own default. The UE side of this row is software ray tracing, as in the `TIER_SOFTWARE` row. The public pages read do not state a Mac HWRT default. | Lumen total. | GS-4 |
| Skinned refit and TLAS rebuild | UE's ray-tracing scene update. The guide says hardware ray tracing rebuilds the TLAS every frame and tells you to read **Ray tracing active instances** under `Stat SceneRendering`. That item is a count. | Report-only. No GPU or CSV column for the refit time is named. The NextGI total on Character Walk carries the bar. | G3-4 |
| `nextgi/enabled = false` (SDFGI) | No comparison equivalent. | No timing stat. | G0-2, G1-4, GS-8 keep their form |

## NextGeo (Nanite)

Nanite is enabled per mesh with **Enable Nanite Support** ([Nanite overview](https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)). Public pages name **Raster Bins** and **Shading Bins** as visualization modes, not as GPU timings. No page read names a GPU stat or CSV column that is only Nanite raster and cull, only Nanite shading, or a Nanite total. These lines keep the UE bar (plan §23.3, §23.6): the stat groups below stand, and #229 names their columns from the packaged Test build.

| GodotNext | UE 5.8 setting | Stat or CSV | Gate lines |
|---|---|---|---|
| `nextgeo/raster_mode = hybrid` (cull, Hi-Z, visibility) | Nanite on for every Nanite mesh. | Nanite raster and cull. No column is named on the pages read. #229 names it. | G5-1 cull sub-line; GN-2 Dense City, M5 Max included |
| Stage A+B resolve with compute shading bins | Nanite material shading bins, the default shading path, as §23.5 already states. | Nanite shading. No column is named on the pages read. #229 names it. | G5-1 resolve sub-line; GN-11 |
| Draw identity (G4-5) | No equivalent. Nanite has no separate step. | No timing stat. | G4-5: report-only. The NextGeo cull total carries the bar. |
| Mesh-shader HW raster (GN-10) | Nanite hardware raster with mesh shaders where the RHI supports it. The [5.8 console-variable reference](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8) publishes `r.Nanite.MeshShaderRasterization` with default 1: "If available, use mesh shaders for hardware rasterization." The comparison leaves it at that default. | Nanite raster and cull, as above. | Settings match. GN-10's relative lines keep their form. |
| Programmable raster, WPO, pixel programmable distance | As §23.5 already states: programmable raster is automatic; Evaluate WPO on; Max WPO Displacement; WPO disable distance 0; Pixel Programmable Distance default unverified. This pass did not add a cvar. | Nanite total. No column is named on the pages read. #229 names it. | GN-4 overhead: report-only. Foliage Field's total carries the bar. |
| Foliage, assemblies, voxels, wind, Preserve Area | As §23.5 already states: ISM/HISM per-instance culling; Nanite Assemblies, Nanite Voxels and foliage wind are Experimental since 5.7, on where the scene uses them; Preserve Area. | Nanite total, as above. | GN-7 Foliage Field total |
| Skinned NextGeo, opted in per asset | Skinned Nanite, opted in per asset. The 5.9 label stays for the §25.3 re-check. | Nanite total, as above. | GN-5 Character Crowd total. N4 stays report-only. |
| Tessellation and displacement (Experimental) | `r.Nanite.AllowTessellation=1` (read-only, set in config) and `r.Nanite.Tessellation=1` (runtime), plus the material checkbox **Enable Tessellation** and a Displacement input ([working with Nanite](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content)). The console-variable reference publishes both cvars at default 0. Experimental. | Nanite total, as above. | GN-6 Displacement Cliff total. N5 stays report-only. |
| `SplineMeshInstance3D` | Spline meshes on Nanite. The product label stays UNVERIFIED, as §23.5 states. The console-variable reference publishes `r.Nanite.AllowSplineMeshes` default 1. That default is not a label. | Nanite total, as above. | N8: report-only |
| `nextgeo/ray_tracing_mode = fallback` / `native` | Nanite ray tracing mode 0 (fallback mesh) / mode 1 (native, Experimental), as §23.5 already states. | No timing stat. | Fallback in every NextGI run. GN-9's native rebuild stays report-only. |
| Nanite tier on the M5 Max | Nanite on Mac, Beta on M2+, as §23.5 already states. | Nanite raster and cull, as above. | GN-2's M5 Max line |
| Dense City sweep (10M / 100M / 1B) | The same sweep in the port. | Whole-frame GPU time, same row as the frame table. | G5-9: report-only, with the UE ratio |

Whether an Experimental or Beta Nanite feature runs in a packaged Test build was not observed. #229 records that (plan §23.3).

## Shadows, lights and fog

| GodotNext | UE 5.8 setting | Stat or CSV | Gate lines |
|---|---|---|---|
| `shadow_method = virtual` | Virtual Shadow Maps, the default shadow method ([VSM, 5.8](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine)). | `stat gpu` buckets **Shadow Depths** (`RenderVirtualShadowMaps(Nanite)` and `RenderVirtualShadowMaps(Non-Nanite)`) and **Shadow Projection** under Lights. CSV category: `CsvCategory VSM`. #229 copies the column headers inside that category. | GV-3 steady state; G5-1 VSM sub-line; GN-12's GV-3 rerun |
| VSM caching; invalidation budget 0 or 512 pages | Caching is on. `r.Shadow.Virtual.Cache 0` disables it. Per-primitive **Shadow Cache Invalidation Behavior** defaults to **Auto**. | Same VSM stats. The 512-page budget has no published equivalent. That match is report-only. | GV-3 time-of-day storm lines |
| SMRT ray and step counts per preset | Set `sg.ShadowQuality` to the same named level (0–4, as above) and do not override the SMRT cvars. At Epic the VSM page says local and directional lights use 8 rays: `r.Shadow.Virtual.SMRT.RayCountLocal` and `RayCountDirectional`. The console-variable reference publishes those defaults as 8, and `SamplesPerRayLocal` and `SamplesPerRayDirectional` as 4. Counts for the other named levels are not on the public page. | Virtual Shadow Maps, as above. | GV-3 |
| VSM page pool per preset | `r.Shadow.Virtual.MaxPhysicalPages` published default 4096. The VSM page says the Shadow scalability setting changes the pool. Per-preset counts are not on the public page. The comparison sets `sg.ShadowQuality` and leaves the cvar at the scalability default. | No timing stat. | Settings match. The V6 memory line keeps its form. |
| `shadow_method = atlas` | No equivalent. The comparison uses VSM. | No timing stat. | G5-1's atlas-path run: report-only |
| Distance-field far shadows | Distance-field shadows, as §23.5 already states. | No timing stat. | GV-10 (quality only) |
| MegaLights: project opt-in, per light, directional lights off | Project Settings, Rendering, Direct Lighting ([MegaLights, 5.8](https://dev.epicgames.com/documentation/unreal-engine/megalights-in-unreal-engine?application_version=5.8)). Off until enabled. The page recommends Support Hardware Ray Tracing. Once on, local lights use MegaLights; **Allow MegaLights** is per light. `r.MegaLights.Allow 0` disables it per scalability level or device profile. Directional lights stay off unless `r.MegaLights.DirectionalLights 1`. Shadow method is Ray Tracing (default) or Virtual Shadow Maps. Production on Windows in the 5.8 release notes. | The stat group name stays MegaLights. No CSV column is named on the page read. #229 records the header. | GM-2 total at 1,024 lights |
| MegaLights on the M5 Max | MegaLights on Apple M2+, Experimental, as §23.5 already states. Recorded only. | MegaLights, as above. #229 names the column. | Recorded only, with the UE ratio |
| `Light3D.shadow_method = RAY_TRACED`, off at project level | Per-light ray-traced shadows ("Use Project Setting"). The [5.8 console-variable reference](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8) publishes `r.RayTracing.Shadows` with default 0: "0: use traditional rasterized shadow map (default) 1: use ray tracing shadows". GodotNext's off-at-project-level default matches it. Which project setting writes that cvar, and "off in templates", are not stated on the pages read. | Ray-traced shadows. No column is named on the pages read. #229 names it. The per-light slope stays report-only. | GM-3 at sun + 8 local lights and at 64 lights |
| Light functions, IES, barn doors, contact shadows | The same production features, as §23.5 already states. No extra cvar was confirmed. | No timing stat. | Settings match only. M-9 stays report-only. |
| Volumetric fog fed by the NextGI ambient volume | Exponential Height Fog, with its Volumetric Fog section ([volumetric fog](https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine)). The Lumen performance guide includes volumetric fog in Lumen's budget. No further cvar was confirmed. | No timing stat. | Settings match only. No fog-only line has a port. |

## Path tracer and upscaling

| GodotNext | UE 5.8 setting | Stat or CSV | Gate lines |
|---|---|---|---|
| PP path tracer, sample count and bounces | Post Process Volume **Samples Per Pixel** and **Max. Bounces**. `r.PathTracing.SamplesPerPixel` and `r.PathTracing.MaxBounces` both default to -1, which follows that volume. `r.PathTracing` guards path-tracer shader permutations (published default 1). Match the port's samples and bounces. | The path-tracer page says to use `stat gpu` for the live timing. It does not print a pass name. The column is report-only until #229 records the header. | GP throughput: reported, not gated. GP's scenes have no port. |
| TSR presets Low through Cinematic | `sg.AntiAliasingQuality` 0–4. History screen percentage is 200 at Epic and Cinematic and 100 below, on the TSR console-variable description. | The stat group name stays TSR. No CSV column is named on the pages read. #229 records the header. | GT-4 T1 and T2 (both cards), T4 (M5 Max) |
| TSR async compute on | `r.TSR.AsyncCompute 2` where the RHI has async compute. The TSR page describes 2 as the default: passes that are independent, or that depend only on depth and velocity, run on async compute. | Same TSR column reason. | Settings match. GT-7 keeps its form. |
| Dynamic resolution (opt-in) | Dynamic resolution on PC, new in 5.8, as §23.5 already states. No extra cvar was confirmed in this pass. | No timing stat. | GT-6 keeps its form. T7 stays report-only. |
| DLSS SR (presets K, M and L) and DLAA | NVIDIA's UE DLSS plugin at the same mode and preset. The Epic pages read do not name the plugin's settings. NVIDIA's UE plugin source is a forbidden input and was not opened (§19, §23.7). #229 records the mode and preset from the installed plugin's editor UI on the RTX machine. | DLSS pass. No column is named on the pages read. #229 names it. | GU-5 U1, U1M, U1A and U1Q |
| DLSS Ray Reconstruction at Epic | The plugin's Ray Reconstruction with Lumen at Epic, as §23.5 already states. #229 records the plugin's setting from its editor UI, as for DLSS SR. | DLSS-RR pass. No column is named on the pages read. #229 names it. U3 stays report-only. | GU+-4 U2 |
| DLSS FG 2x, MFG 4x, Dynamic MFG | The plugin's Frame Generation, MFG and Dynamic MFG, as §23.5 already states (DX12, Streamline, off). The plugin API was not confirmed from an Epic page, and the plugin source was not read. | Not a GPU-stat column. F1 uses the Nsight Systems method. Displayed frame time uses PresentMon. | F1; GU+-5; GU+-9 (RTX 5090 only) |
| Reflex Off, On, On + Boost | The plugin's Reflex, as §23.5 already states. Same report-only reason for the plugin API. | PC latency, by the PresentMon method. | GU+-5 |

## Not confirmed, on purpose

These sentences elsewhere in the plan pointed at this record. The public pages read do not settle them.

- Mac HWRT default. Not stated. GodotNext's `hardware_ray_tracing.macos = off` is unchanged.
- "Hit lighting and MegaLights use RT pipelines." Hit lighting evaluates lighting at the hit, and MegaLights recommends Support Hardware Ray Tracing. Neither page says both use RT pipelines.
- Streamline and DLSS RHI list, and the FG plugin API. Not confirmed from an Epic page. The plugin source was not read. §23.5 still says FG is DX12 and Streamline, which is the plan's existing text.
- Which ray-traced-shadow project setting writes `r.RayTracing.Shadows`, and "off in templates." Not stated.
- Per-preset SMRT counts other than Epic's 8 rays, and per-preset VSM page-pool sizes. Not on the public page. `BaseScalability.ini` was not read.
- A 512-page VSM invalidation budget. Not named.
- CSV column headers for the frame, the render thread, the Lumen passes, Nanite, MegaLights, TSR, the path tracer, ray-traced shadows and DLSS. #229 names them from a packaged Test build. Each of these stat groups keeps the UE bar until then.
- A Lumen GI stat apart from Lumen reflections that covers the diffuse path's work (G1-3, G2-4). Not named on the pages read. #229 checks the Test build.

Answered on 2026-11-09 from the console-variable reference this record already cites (the 5.8 page; on 2026-11-09 the page titled UE 5.9 prints the same two rows):

- Nanite mesh-shader hardware raster: `r.Nanite.MeshShaderRasterization` default 1 (the GN-10 row).
- Ray-traced shadows: `r.RayTracing.Shadows` default 0, rasterized shadow maps (the GM-3 row).

## Experimental and Beta in a packaged Test build

This pass did not package a Test build, so it does not say which of these run there. The labels below are the public 5.8 labels. #229 records whether the Test build actually runs each one.

- Lumen Lite (Medium GI): Beta, 5.8 release notes. The settings are the Medium row above.
- Nanite tessellation: Experimental. The settings are the tessellation row above.
- Nanite Assemblies, Nanite Voxels and foliage wind: Experimental since 5.7, as §23.5 already states.
- Nanite and Virtual Shadow Maps on Apple Silicon M2+: Beta, as §23.5 already states.
- MegaLights on Apple M2+: Experimental, as §23.5 already states. MegaLights on Windows is Production in the 5.8 release notes.
- Native Nanite ray tracing (mode 1): Experimental, as §23.5 already states.
- Hit lighting for GI: label UNVERIFIED. GL-4 stays report-only.

## Corrections

- 2026-11-09 (plan v3.6.1). The first version of this record (55b95936) made the Nanite raster-and-cull, shading and total rows, the ray-traced-shadow total and the DLSS SR, DLAA and RR rows report-only because no public page names their column. §23.3, §23.6 and the gate issues keep those lines on the UE bar, and no lead decision changed that, so the rows name their stat groups again. The diffuse-path row had mapped G1-3 and G2-4 to Lumen Screen Probe Gather alone; it now carries the plan's condition and fallback again. The mesh-shader raster and ray-traced-shadow cvar defaults come from the console-variable reference this record already cited. The status note at the top replaces the earlier wording, which read as a finished record.
