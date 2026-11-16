# D3D12 validation source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used to make the D3D12 driver's root signatures work under
GPU-based validation (#272), and to find why storage-buffer atomics do not land under it
(#277): what each input is, where it came from, its license, and how it was used.

The cause of the failure, that `CreateRootSignature` with GPU-based validation on commits
memory in proportion to the highest root-constant or root-descriptor register, was not
taken from any document. The #272 implementer measured it with a standalone probe
program (not committed) on an NVIDIA L4 with the Agility SDK 1.618 debug layers. The
first draft of the switch, written while finishing #174, and the RTX 4080 SUPER logs
that showed the failure are this project's own work, not outside sources.

## GPU-based validation and register spaces (#272)

### Mesa: SPIR-V to DXIL and NIR to DXIL

| Field | Value |
|---|---|
| What | How Mesa's SPIR-V to DXIL path assigns resource registers and spaces, and how NIR to DXIL finds the resource behind a buffer access. `vtn_variables.c` sets the `vulkan_resource_index` intrinsic's descriptor set and binding from the variable. `nir_to_dxil.c` declares UBO and SSBO resources from the variable's `descriptor_set` (the space) and `binding` (the register), and resolves `load_vulkan_descriptor` through `nir_chase_binding` and `nir_get_binding_variable`. `spirv_to_dxil.h` and `dxil_spirv_nir.c` place the push-constant and runtime-data CBVs at `dxil_spirv_runtime_conf`'s register and space. `nir.h` holds the 5-bit `descriptor_set` field. `dxil_container.c` and `dxil_container.h` give the PSV0 part's layout: runtime info size and runtime info, resource count, record size, then `dxil_resource_v0` records (type, space, lower bound, upper bound). |
| Upstream | Mesa, https://gitlab.freedesktop.org/mesa/mesa: `src/compiler/spirv/vtn_variables.c`, `src/compiler/nir/nir.c`, `src/compiler/nir/nir.h`, `src/microsoft/spirv_to_dxil/spirv_to_dxil.c`, `spirv_to_dxil.h`, `dxil_spirv_nir.c`, `dxil_spirv_nir.h`, `src/microsoft/compiler/nir_to_dxil.c`, `nir_to_dxil.h`, `dxil_container.c` and `dxil_container.h` |
| Version | Mesa 25.3.1, the version that godot-nir-static release `25.3.1-3` builds. That release is the one `misc/scripts/install_d3d12_sdk_windows.py` installs |
| License | MIT. Each file read carries the MIT permission notice, under a Microsoft Corporation copyright for the `src/microsoft` files and an Intel Corporation or Connor Abbott copyright for the `src/compiler` files. Consulted, not copied |
| Read on | 2026-11-15, for issue #272 |
| Consulted by | The #272 implementer |
| Used in | `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_remap_dynamic_buffers`, `_convert_spirv_to_nir`, `_nir_report_resource`), `drivers/d3d12/rendering_shader_container_d3d12.h` (the register-space constants and their `static_assert`), `tests/servers/rendering/test_d3d12_shader_container.cpp` (`read_psv0_resources`) |
| Copied files | None |

How it was used: Mesa confirms that a buffer's register and space live in the NIR variable and in each `vulkan_resource_index` that names it, so `_remap_dynamic_buffers` rewrites both together, after `dxil_spirv_nir_passes` and before NIR to DXIL runs. The runtime configuration's `register_space` fields move the push-constant and runtime-data CBVs. The space must fit Mesa's 5-bit field. The doctest walks the DXIL container's parts and reads PSV0's resource records in the layout above, to check that every resource the DXIL declares is bound by the root signature.

### godot-nir-static patches

| Field | Value |
|---|---|
| What | The godot-nir-static patch `01_godot_nir_goodies.patch`, which gives Godot's prebuilt Mesa its resource-register scheme (set and binding times the multipliers in `drivers/d3d12/d3d12_godot_nir_bridge.h`, in space 0) and the callbacks that report resources and bitcode offsets to the engine |
| Upstream | https://github.com/godotengine/godot-nir-static |
| Version | Release `25.3.1-3`, as installed by `misc/scripts/install_d3d12_sdk_windows.py` |
| License | MIT (Mesa's license for the patched files). Consulted, not copied |
| Read on | 2026-11-15, for issue #272 |
| Consulted by | The #272 implementer |
| Used in | `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_remap_dynamic_buffers`, `_nir_report_resource`) |
| Copied files | None |

How it was used: it shows that the multipliers are compiled into the prebuilt library, so the fix leaves Mesa's descriptor-table registers alone. It moves only the root constants and the dynamic buffers, which become root descriptors, after Mesa has assigned them.

### DirectX-Headers (`d3d12.h`, `d3d12sdklayers.h`)

| Field | Value |
|---|---|
| What | `ID3D12Debug1::SetEnableGPUBasedValidation`, the `D3D12_GPU_BASED_VALIDATION_FLAGS` enumeration, root signature version 1.1 (`D3D12_ROOT_PARAMETER1`, `D3D12_DESCRIPTOR_RANGE1`, root constants and root CBV, SRV and UAV descriptors with a register and a register space), and `D3D12CreateVersionedRootSignatureDeserializer` |
| Upstream | https://github.com/microsoft/DirectX-Headers, vendored in `thirdparty/directx_headers` |
| Version | The vendored copy, `main` at `25411c74bb9cc7c416b2ff01b3ad8a306811dfdd` (2025), as recorded in `thirdparty/README.md` |
| License | MIT, as recorded in `thirdparty/README.md`. Read in the tree, not copied |
| Read on | 2026-11-15, for issue #272 |
| Consulted by | The #272 implementer |
| Used in | `drivers/d3d12/rendering_context_driver_d3d12.cpp` (`_initialize_debug_layers`), `drivers/d3d12/rendering_shader_container_d3d12.cpp` (`_generate_root_signature`), `tests/servers/rendering/test_d3d12_shader_container.cpp` |
| Copied files | None |

How it was used: the switch queries `ID3D12Debug1` from the debug controller and calls `SetEnableGPUBasedValidation` after `EnableDebugLayer`. The root signature gives root constants and root descriptors their new register spaces. The doctest deserializes the container's root signature to check each parameter's register and space.

## Atomics under GPU-based validation (#277)

What was measured, on an NVIDIA L4 and on WARP (the Microsoft Basic Render Driver software
adapter) with the Agility SDK 1.618 debug layers, by the #277 implementer. None of it was taken
from a document:

- With GPU-based validation on, storage-buffer and image atomics whose result the shader does
  not use are lost, with no debug-layer message: `atomicAdd` on a storage buffer in compute and
  fragment shaders, and, tried in compute shaders only, `atomicOr`, `atomicMax`,
  `atomicExchange`, `atomicCompSwap` and `imageAtomicAdd` (all 32-bit; the D3D12 driver does
  not offer 64-bit atomics yet, so those were not tried). Atomics whose result is used,
  shared-memory atomics and plain stores land. Without GPU-based validation everything lands.
  The fixture is `godotnext/testbed/tests/rd_parity/storage_atomics.tscn`.
- On the L4, it happens whenever GPU-based validation patches shaders: the debug device's
  default shader patch mode set to state tracking only, unguarded or guarded validation loses
  the atomics, and "none" keeps them. Forcing the patch mode is not a fix, since "none" turns
  the shader checks off.
- It is the DXIL the engine's Mesa library writes, not the GPU driver and not GPU-based
  validation in general. A standalone probe program (not committed) ran the same shader compiled
  by the DirectX Shader Compiler, and the engine's own DXIL with its root signature. The DXC
  shader landed every atomic under GPU-based validation on both adapters, and the engine's DXIL
  lost the unused-result ones on both. The disassembly shows the difference: Mesa declares
  `dx.op.atomicBinOp` (and, in its source, `dx.op.atomicCompareExchange`) with the attributes
  `nounwind readnone`, and DXC declares `dx.op.atomicBinOp` `nounwind`. The probe then changed
  only the attribute list of the engine DXIL's `dx.op.atomicBinOp.i32` declaration, to
  `nounwind` or to none, and rehashed the container: every atomic landed under GPU-based
  validation on both adapters.

The fix belongs in Mesa (declaring the two atomic operations without `readnone`,
as DXC does), which is not this repository's code. Until then the limit is documented in
`rendering/rendering_device/d3d12/gpu_based_validation`'s class reference, and the testbed
reports the affected results as not checked under GPU-based validation
(`godotnext/testbed/harness/d3d12_gbv.gd`, the fixture, `tests/tier/tier_check.gd` for the
NextGI WRC benchmark, whose ray and hit counters are such atomics, and the `GODOTNEXT_RUNNER`
line of `harness/runners/win_session1.ps1`). The engine's shaders use statement-form
atomics on storage buffers or images in 20 files: NextGI (the WRC update, the
gathers, voxel lighting, the global SDF and source relighting), NextGeo culling, volumetric fog,
the Forward+ and Mobile scene shaders, particles, SDFGI preprocessing and the SSAO and SSIL
importance maps. Their results in a GPU-based validation run are not valid; its validation
messages are. Apart from `tier_check.gd`, the testbed checks that read counters NextGI or
NextGeo fill with such atomics are not routed through `d3d12_gbv.gd`, so under GPU-based
validation only the runner line marks them. They were not all audited. Many checks read NextGI's
counters through `nextgi_get_tier_info`, `nextgi_get_counters` or `nextgi_get_status`; for
example, `tests/swrt_gi/swrt_gi_check.gd` fails a run whose gather traced no rays or whose
software WRC found no clean SDF hit, and both counts are such atomics.
`tests/nextgeo_visibility/nextgeo_visibility_check.gd` is another: its probe counts virtual
pixels and non-finite and sentinel velocities with such atomics, and NextGeo's culling uses them
too, so the whole result is not valid. So are the Q21 counts that `harness/velocity_dump.gd`
writes for a PQ metric.

### Mesa: DXIL function declarations

| Field | Value |
|---|---|
| What | The table of DXIL operations Mesa declares, with each declaration's attribute (`predefined_funcs` in `dxil_function.c`: `dx.op.atomicBinOp` and `dx.op.atomicCompareExchange` are `DXIL_ATTR_KIND_READ_NONE`), and how `dxil_module.c` turns that attribute into the declaration's attribute set (`nounwind` plus the given attribute) |
| Upstream | Mesa, https://gitlab.freedesktop.org/mesa/mesa: `src/microsoft/compiler/dxil_function.c` and `src/microsoft/compiler/dxil_module.c` |
| Version | Mesa 25.3.1, the version both the upstream library (godot-nir-static `25.3.1-3`) and the GodotNext Mesa NIR library build, and Mesa `main` at `6c09d07c567eb8c9624b4026e49d09fd83166ae0` (2026-11-16), which declares the two operations the same way |
| License | MIT, under a Microsoft Corporation copyright. Consulted, not copied |
| Read on | 2026-11-16, for issue #277 |
| Consulted by | The #277 implementer |
| Used in | Nothing committed. The class reference of `rendering/rendering_device/d3d12/gpu_based_validation` and `godotnext/testbed/harness/d3d12_gbv.gd` state the declaration |
| Copied files | None |

How it was used: it explains the disassembly, and shows that the declaration comes from a fixed table in the library rather than from the shader, so the engine cannot change it through Mesa's options.

### DirectX-Headers (`d3d12sdklayers.h`): GPU-based validation settings

| Field | Value |
|---|---|
| What | `ID3D12Debug2::SetGPUBasedValidationFlags` (`D3D12_GPU_BASED_VALIDATION_FLAGS_DISABLE_STATE_TRACKING`), `ID3D12DebugDevice2::SetDebugParameter` and `GetDebugParameter` with `D3D12_DEBUG_DEVICE_PARAMETER_GPU_BASED_VALIDATION_SETTINGS` (`D3D12_DEBUG_DEVICE_GPU_BASED_VALIDATION_SETTINGS`, the `D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE` values) |
| Upstream | https://github.com/microsoft/DirectX-Headers, vendored in `thirdparty/directx_headers` |
| Version | The vendored copy, as recorded in `thirdparty/README.md` |
| License | MIT, as recorded in `thirdparty/README.md`. Read in the tree, not copied |
| Read on | 2026-11-16, for issue #277 |
| Consulted by | The #277 implementer |
| Used in | Nothing committed: a diagnostic build and the probe program used them to try each shader patch mode |
| Copied files | None |

### DirectX Shader Compiler (`dxc.exe` in the Windows SDK)

| Field | Value |
|---|---|
| What | The compiler and disassembler: a compute shader with `RWByteAddressBuffer::InterlockedAdd`, compiled for `cs_6_2` as the probe's control, and `-dumpbin` disassembly of that shader and of the engine's DXIL |
| Upstream | https://github.com/microsoft/DirectXShaderCompiler, as shipped in the Windows SDK |
| Version | Windows SDK 10.0.26100.0, `bin\10.0.26100.0\x64\dxc.exe` |
| License | University of Illinois/NCSA Open Source License for the project. Run as a tool; nothing copied or shipped |
| Read on | 2026-11-16, for issue #277 |
| Consulted by | The #277 implementer |
| Used in | Nothing committed |
| Copied files | None |

### LLVM bitcode format

| Field | Value |
|---|---|
| What | The bitstream format that DXIL's LLVM 3.7 bitcode uses (blocks, abbreviations, VBR operands), the module's function records (the attribute-list operand) and the attribute blocks, as LLVM documents them |
| Upstream | LLVM, "LLVM Bitcode File Format", https://llvm.org/docs/BitCodeFormat.html |
| Version | The format of LLVM 3.7, which DXIL is based on |
| License | LLVM 3.7 is under the University of Illinois/NCSA Open Source License. Consulted, not copied |
| Read on | 2026-11-16, for issue #277 |
| Consulted by | The #277 implementer |
| Used in | Nothing committed: a script that changed one declaration's attribute-list operand in a copy of the engine's DXIL, for the probe |
| Copied files | None |
