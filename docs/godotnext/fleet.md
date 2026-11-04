# Fleet

Hardware is named by what it is. This file does not record host names.

## Background runs on the Apple M5 Max

The M5 Max is also someone's desktop. A rendered (non-headless) Godot run activates itself and takes the keyboard focus, so every unattended run there sets `GODOTNEXT_BACKGROUND=1`: agents, the harness, smoke scenes and scene checks. With it, Godot runs as an accessory app (no Dock icon or menu bar), never activates itself, and shows its windows fully transparent and click-through. The windows stay on screen, so Metal keeps drawing every frame. Captures read the viewport texture, so they are unchanged.

```
GODOTNEXT_BACKGROUND=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --quit-after 60 <scene>
```

`misc/scripts/godotnext_background_check.sh` checks it: a lit scene renders on Metal while the frontmost app stays the same. Don't switch apps while it runs. The mode lives in `platform/macos/godotnext_background.h` and the `background_mode` hooks.

A transparent window is still a window: it shows as a blank tile in Mission Control, and the editor's main window spans the whole screen during `--export-pack` (the editor ignores `--position` and `--resolution`). Export runs never present a frame, so they also set `GODOTNEXT_BACKGROUND_HIDDEN=1`, which keeps the window ordered out. The shader baker still compiles on the RenderingDevice.

```
GODOTNEXT_BACKGROUND=1 GODOTNEXT_BACKGROUND_HIDDEN=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --export-pack "macOS (baker)" <out.pck>
```

## Metal shader validation

PR basics on the Apple M5 Max count Metal API and shader validation, not only Godot `ERROR:` lines. Shader-validation reports stay off stderr unless `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` is set. With the variable unset, a scene can render black while stdout looks clean, because validation zeros the stores it rejects.

```
GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 ./bin/godot.macos.editor.arm64 --path godotnext/testbed --rendering-driver metal --quit-after 60 <scene>
```

A pass is zero lines containing `Invalid Metal usage`, zero lines containing `Error detected in`, and zero lines starting with `ERROR:`. `useHeaps` marks placement-heap resources read-only. Argument-buffer writes need `useResource` with the shader's usage, or the log reports an invalid store (`resident:Read Only`) or a texture that is not resident.

## Apple M5 Max environment (recorded 2026-11-04, #11)

| Item | Value |
|---|---|
| Host | Apple M5 Max |
| GPU | Apple M5 Max, 40 cores, Metal 4 |
| macOS | 27.0.1 (26A434) |
| Compiler | Apple clang 21.0.0 (clang-2100.4.1.1) |
| Build | `SCONS_CACHE=$HOME/.cache/godotnext-scons ~/.cache/godotnext-venv/bin/python -m SCons platform=macos target=editor dev_mode=yes arch=arm64 use_volk=yes -j8` |
| Editor binary | `bin/godot.macos.editor.arm64` |
| Vulkan | Built with `use_volk=yes` (no MoltenVK SDK); Vulkan RT stays compiled out, and Metal is the P0/P1 backend here |
| Metal ray query | Active: the HWRT tier reports `NextGI: tier=HWRT reason=NONE` and the ray-query self-test passes |
| Doctests | `--headless --test`: 1632 passed, 0 failed, 3 platform skips that predate this record |
| SPIR-V tools | SPIRV-Tools v2026.4 (2026-09-10); SPIRV-Cross built 2026-09-16, both from `/opt/homebrew/bin` |

The dev tools are a local dependency, never vendored (§13.1). Wall time is not recorded yet: the scons cache is warm (2.5 GB), so the builds so far are 9–15 s incremental runs, not a cold full build. The RTX 4080 SUPER, RTX 5090 and Linux VM rows and the raw `vulkaninfo` JSON need those machines (§24.3).

## Ray-query toolchain (G0)

This section is the issue #20 record. Keep it separate from the #11 section.

The check proves two things. Godot's glslang compiles the `#version 460` `GL_EXT_ray_query` self-test to SPIR-V 1.3, `spirv-val --target-env vulkan1.1` accepts that module, and SPIRV-Cross turns `rayQuery` into `intersection_query`. On a device without ray query, `RendererRD::NextGI::run_toolchain_selftest` returns `{"pass":false,"reason":"NO_RAY_QUERY"}` and does not construct `NextgiRqSelftestShaderRD`. Metal ray query lands with #48 and #47, so the Apple M5 Max takes that path today. NVIDIA acceptance of the SPIR-V 1.3 module is #22.

### Re-run

CI runs `--test` on Linux and Windows runners that do not have `spirv-val` or `spirv-cross`. The doctest is that in-engine proof. It expands `nextgi_rq_selftest.glsl` through `ShaderRD` (`NextGI::build_toolchain_selftest_source`, which includes `nextgi_rt_inc.glsl`), compiles that source with `compile_glslang_shader` at Vulkan 1.1 / SPIR-V 1.3 (the Vulkan compute target in `rendering_shader_container_vulkan.cpp`), and checks the SPIR-V magic `0x07230203`, the version word `0x00010300`, one `rayQueryProceedEXT`, no candidate loop, and the absence of `rayQueryGetIntersectionTriangleVertexPositionsEXT`, `ShaderBindingTableRecordOffset`, and `ray_tracing_position_fetch`. A second case checks the `NO_RAY_QUERY` JSON when the device has no ray query. The doctest does not call either external tool. It writes the compiled module only when `GODOTNEXT_RQ_SPIRV_OUT` is set, and writes nothing otherwise.

The PR check on the Apple M5 Max is `misc/scripts/godotnext_rq_toolchain_check.sh`. It runs the compile doctest with `GODOTNEXT_RQ_SPIRV_OUT` set, then:

- `spirv-val --target-env vulkan1.1 <module.spv>`
- `spirv-cross <module.spv> --msl --msl-version 30000 --stage comp`

The script fails if either tool fails or the MSL does not contain `intersection_query`. The binary path is the first argument and defaults to `bin/godot.macos.editor.arm64` from the repository root. `GODOTNEXT_SPIRV_VAL` and `GODOTNEXT_SPIRV_CROSS` override the tool paths. Otherwise the script uses `spirv-val` and `spirv-cross` from `PATH`.

```
misc/scripts/godotnext_rq_toolchain_check.sh
```

Runtime hook, after `gi.init` in `RendererSceneRenderRD::init`:

```
GODOTNEXT_RQ_SELFTEST=1 GODOTNEXT_SPIRV_DUMP_DIR=<dir> <bin> --path <project> --rendering-driver metal --rendering-method forward_plus --gpu-index 0 --quit-after 3
```

Set `rendering/shader_compiler/shader_cache/enabled` to false in that project. A shader-cache hit loads bytecode in `ShaderRD::_load_from_cache` and never calls `_compile_variant`, so the dump is skipped. The dump writes every `ShaderRD` variant that compiles, as `<dir>/<name>.v<variant>.<stage>.spv`. The self-test signal is the presence or absence of `NextgiRqSelftest*.spv`.

Since #15 the self-test is a ray-query ShaderRD like any other, so it initializes only under the NextGI HWRT tier (plan §3.1). On a device with ray query, the project also needs `rendering/global_illumination/nextgi/enabled = true` and `hardware_ray_tracing` other than Off. Otherwise the report names the tier reason (for example `{"pass":false,"reason":"SETTING_OFF"}`) or `DISABLED`. A device without ray query still reports `NO_RAY_QUERY`. For a one-off run, put the setting in the project's `override.cfg`, as `godotnext/testbed/tests/tier/run_tier_check.sh` does.

### D5 source lint

`rg` searches comments, so the forbidden tokens must not appear in the `.glsl` text at all. The pattern is:

```
rg -n "rayQueryGetIntersectionTriangleVertexPositionsEXT|accelerationStructureEXT\s*\(|ShaderBindingTableRecordOffset|ray_tracing_position_fetch" servers/rendering/renderer_rd/shaders godotnext/testbed --glob '*.glsl'
```

`accelerationStructureEXT` followed by `(` is the 64-bit address constructor (`OpConvertUToAccelerationStructureKHR`). An `accelerationStructureEXT` uniform is allowed. The header of `nextgi_rt_inc.glsl` states the same rule: opaque-only (`gl_RayFlagsOpaqueEXT`), one `rayQueryProceedEXT`, no candidate loop, shared memory at or under 8 KiB, and none of the three constructs SPIRV-Cross MSL cannot translate.

`godotnext/testbed` is not in the tree yet (the testbed is a later issue). On 2026-11-03 the command above exits 2 because that directory is missing. The same pattern limited to `servers/rendering/renderer_rd/shaders` exits 1, which is ripgrep's "no matches" status. When the testbed lands, the full command covers its kernels too.

### Recorded on 2026-11-03

| Device | Driver | SPIR-V | spirv-val | Pipeline | Validation-layer errors |
| --- | --- | --- | --- | --- | --- |
| Apple M5 Max (40-core GPU, macOS 27.0.1) | Metal 4.0, device 0: Apple M5 Max (Apple9). `--gpu-index 0`, Forward+. | Offline module is 1.3, version word `0x00010300` (magic `0x07230203`). The runtime path does not compile the kernel. | SPIRV-Tools v2026.4, 2026-09-10. PR check `misc/scripts/godotnext_rq_toolchain_check.sh`: `spirv-val --target-env vulkan1.1` from `PATH`, exit 0, empty output. No `NextgiRqSelftest*.spv` was dumped (280 other engine shaders were), so there is no runtime dump to validate. | Not created. `GODOTNEXT_RESULT {"pass":false,"reason":"NO_RAY_QUERY"}`. Process exit 0. | Not run. The kernel is not submitted. Metal ray query is #48 and #47. |
| RTX 4080 SUPER machine | pending (#22) | pending (#22) | pending (#22) | pending (#22) | pending (#22) |
| Linux VM | pending (#22) | pending (#22) | pending (#22) | pending (#22) | pending (#22) |

Apple M5 Max MSL line from the PR check script, SPIRV-Cross 1.4.363.0 (timestamp 2026-09-16), `spirv-cross --msl --msl-version 30000 --stage comp`, exit 0. On this machine `PATH` resolves `spirv-val` and `spirv-cross` to `/opt/homebrew/bin`.

```
NextGIHit nextgi_trace_opaque(thread const float3& origin, thread const float& tmin, thread const float3& dir, thread const float& tmax, thread const uint& mask, thread raytracing::intersection_query<raytracing::instancing, raytracing::triangle_data>& rq, const raytracing::acceleration_structure<raytracing::instancing> nextgi_tlas)
```

The headless doctest saw the same `NO_RAY_QUERY` JSON, because `--headless --test` has no `RenderingDevice`. The windowed Metal run is the device-0 result in the table. Both paths return before `NextgiRqSelftestShaderRD` is constructed.

G0-1 (0 `spirv-val` errors on every dumped ray-query module, on this machine and on the RTX 4080 SUPER machine) is a PQ measurement under #24. The offline module on this machine was clean. The RTX 4080 SUPER dump, driver 621.01, `--gpu-validation`, and the Linux VM row move to #22.
