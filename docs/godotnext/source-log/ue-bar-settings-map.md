# UE comparison settings map source log

The plan's clean-room protocol (§19, §23.7) requires a source log when outside
input is used. This file records the input for the UE settings map (#228).

## Outside input

Public Epic documentation only. No Unreal Engine source, shader, plugin source,
NvRTX sample, Donut sample or decompile was opened. `Engine/Config/BaseScalability.ini`
was not opened. No editor UI was observed, and no packaged Test build was run.
API pages below display a header path; those headers were not opened.

- https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine?application_version=5.8
- https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes
- https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference
- https://dev.epicgames.com/documentation/unreal-engine/megalights-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content
- https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/path-tracer-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/volumetric-fog-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/anti-aliasing-and-upscaling-in-unreal-engine
- https://dev.epicgames.com/documentation/unreal-engine/scalability-reference-for-unreal-engine?application_version=5.4
- https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/ELumenRayLightingMode
- https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FPostProcessSettings
- https://docs.unrealengine.com/4.27/en-US/TestingAndOptimization/PerformanceAndProfiling/CSVProfiler
- https://docs.unrealengine.com/5.3/en-US/stat-commands-in-unreal-engine/

## Copied files

None.

## Correction pass (2026-11-09, plan v3.6.1)

Public Epic documentation only, read again for the #228 correction. No Unreal
Engine install, source, shader, plugin source, `.ini` file or decompile was
opened.

| Field | Value |
|---|---|
| What | Console-variable reference rows `r.Nanite.MeshShaderRasterization` (default 1, "If available, use mesh shaders for hardware rasterization.") and `r.RayTracing.Shadows` (default 0, "0: use traditional rasterized shadow map (default) 1: use ray tracing shadows") |
| URL | https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference?application_version=5.8 |
| Version | The page titled "Unreal Engine 5.8 Documentation", read on 2026-11-09. The unversioned URL now serves the page titled "Unreal Engine 5.9 Documentation"; both print the same two rows |
| License | Epic Games public documentation. Consulted, not copied: only the cvar names, default values and their one-line descriptions are quoted |
| Consulted by | The #228 fixer |
| How used | Answers the two "not stated" defaults in `docs/godotnext/ue-bar/settings-map.md` (GN-10 and GM-3 rows) and in plan §21.1, §21.3, §21.4 and §23.5 |

| Field | Value |
|---|---|
| What | The Lumen performance guide's pass descriptions: "Lumen Scene Lighting for evaluating surface cache lighting", "Lumen Screen Probe Gather for evaluating diffuse global illumination and rough reflections, and translucency global illumination", and "Hardware Ray Tracing requires rebuilding the Top Level Acceleration Structure (TLAS) every frame" |
| URL | https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine?application_version=5.8 |
| Version | The page titled "Unreal Engine 5.8 Documentation", read on 2026-11-09 |
| License | Epic Games public documentation. Consulted, not copied beyond the quoted pass descriptions |
| Consulted by | The #228 fixer |
| How used | Shows that Lumen Screen Probe Gather alone does not cover the GodotNext diffuse path (lines 1a and 2), so the G1-3/G2-4 row keeps the plan's condition and fallback |

| Field | Value |
|---|---|
| What | The page title and heading of the UE 5.9 release notes ("Unreal Engine 5.9 Preview Release Notes") |
| URL | https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-9-release-notes |
| Version | Read on 2026-11-09 (page marked last updated 11/9/26) |
| License | Epic Games public documentation. Consulted, not copied |
| Consulted by | The #228 fixer |
| How used | Only to record that UE 5.9 Preview documentation is now public, so #228's UE 5.9 pass can start. No 5.9 setting was taken from it |
