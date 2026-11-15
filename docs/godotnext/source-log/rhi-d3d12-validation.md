# D3D12 validation source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used to make the D3D12 driver's root signatures work under
GPU-based validation (#272): what each input is, where it came from, its license, and
how it was used.

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
