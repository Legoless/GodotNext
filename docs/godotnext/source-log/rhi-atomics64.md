# 64-bit atomics source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records the outside inputs used for 64-bit atomics through RenderingDevice (#175) on
Vulkan and Direct3D 12: what each is, where it came from, its license, and how it was
used. The Metal part, including the SPIRV-Cross change, is in `rhi-metal.md`.

## 64-bit atomics (#175), Vulkan and D3D12

RenderingDevice gains `SUPPORTS_BUFFER_INT64_ATOMICS`, `SUPPORTS_IMAGE_INT64_ATOMICS` and
`SUPPORTS_INT64_ATOMIC_ARITH`. A SPIR-V scan finds the 64-bit atomics a shader uses, and
shader creation fails with one named error when the device lacks a matching feature.
The Vulkan driver enables `VK_KHR_shader_atomic_int64` and
`VK_EXT_shader_image_atomic_int64` and chains their individual feature structures. The
D3D12 driver reads its 64-bit atomic options and maps `R64_UINT` to an `R32G32_UINT`
storage UAV, but reports the flags as false until its DXIL toolchain can emit 64-bit
atomics (#123).

### Vulkan specification, 64-bit integer atomics

| Field | Value |
|---|---|
| What | `VK_KHR_shader_atomic_int64` (the SPIR-V `Int64Atomics` capability, promoted to Vulkan 1.2) and `VkPhysicalDeviceShaderAtomicInt64Features` (`shaderBufferInt64Atomics` for buffers, `shaderSharedInt64Atomics` for shared and payload memory). `VK_EXT_shader_image_atomic_int64` and `VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT` (`shaderImageInt64Atomics`, `sparseImageInt64Atomics`), with 64-bit atomics guaranteed on optimally tiled `VK_FORMAT_R64_UINT` and `VK_FORMAT_R64_SINT` images when the feature is present. `VUID-VkDeviceCreateInfo-pNext-02830`: a device create chain with `VkPhysicalDeviceVulkan12Features` must not also hold `VkPhysicalDeviceShaderAtomicInt64Features`, `VkPhysicalDeviceShaderFloat16Int8Features`, `VkPhysicalDeviceBufferDeviceAddressFeatures` or `VkPhysicalDeviceVulkanMemoryModelFeatures`, among others. |
| Upstream | https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_atomic_int64.html, https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderAtomicInt64Features.html, https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_image_atomic_int64.html, https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT.html and https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html |
| Version | Vulkan reference pages as published on 2026-11-15. The driver compiles against the vendored `thirdparty/vulkan` headers (`VK_HEADER_VERSION` 335) |
| License | The reference pages show no license notice of their own and are generated from the specification sources (CC-BY-4.0, per https://github.com/KhronosGroup/Vulkan-Docs/blob/main/LICENSE.adoc). Consulted, not copied |
| Read on | 2026-11-15, for issue #175 |
| Consulted by | The #175 implementer |
| Used in | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (`_initialize_device_extensions`, `_check_device_capabilities`, `_initialize_device`, `has_feature`) |
| Copied files | None |

How it was used: both extensions are requested as optional. The feature structures are queried, and the supported features are enabled at device creation, through the individual structures and not `VkPhysicalDeviceVulkan12Features`, because the create chain already carries the 16-bit float, buffer device address and memory model structures that the VUID lists. `SUPPORTS_IMAGE_INT64_ATOMICS` also checks that `VK_FORMAT_R64_UINT` has `VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT` with optimal tiling. `SUPPORTS_INT64_ATOMIC_ARITH` needs `shaderSharedInt64Atomics` as well as `shaderBufferInt64Atomics`, because RenderingDevice puts 64-bit atomics on shared memory in that flag (Metal has none).

### SPIR-V extension SPV_EXT_shader_image_int64

| Field | Value |
|---|---|
| What | The `Int64ImageEXT` capability (atomics, loads and stores on 64-bit integer images) and the `R64ui` and `R64i` image formats. A module that uses atomics on these images must also declare `Int64Atomics`. |
| Upstream | https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_shader_image_int64.html |
| Version | Revision 2, as published on 2026-11-15. The capability and format values come from the vendored `thirdparty/spirv-headers` |
| License | Copyright (c) 2019 The Khronos Group Inc., under the Khronos specification copyright terms (https://www.khronos.org/registry/speccopyright.html). Consulted, not copied |
| Read on | 2026-11-15, for issue #175 |
| Consulted by | The #175 implementer |
| Used in | `servers/rendering/rendering_device_atomics64.cpp` (the SPIR-V scan) |
| Copied files | None |

How it was used: it confirms that an image-only 64-bit atomic shader declares `Int64Atomics` too, which is why the scan maps `Int64Atomics` to the buffer flag and `Int64ImageEXT` to the image flag, and why drivers report the image flag only together with the buffer flag.

### Direct3D 12 64-bit atomic feature options

| Field | Value |
|---|---|
| What | `D3D12_FEATURE_DATA_D3D12_OPTIONS1::Int64ShaderOps` (64-bit integer operations), `D3D12_FEATURE_DATA_D3D12_OPTIONS9::AtomicInt64OnTypedResourceSupported` and `AtomicInt64OnGroupSharedSupported`, and `D3D12_FEATURE_DATA_D3D12_OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported`. The shader model 6.6 64-bit atomics specification: raw and structured buffers bound as root descriptors must support 64-bit atomics on shader model 6.6 devices with 64-bit integers; typed resources, group shared memory and resources in descriptor heaps are optional, behind the three options above; 64-bit typed atomics use an `R32G32_UINT` UAV. |
| Upstream | https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options1, https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options9, https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options11 and https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html |
| Version | Microsoft Learn and DirectX-Specs as published on 2026-11-15. The driver compiles against the vendored `thirdparty/directx_headers` |
| License | Microsoft Learn documentation and Microsoft DirectX-Specs (CC-BY 4.0). Consulted, not copied |
| Read on | 2026-11-15, for issue #175 |
| Consulted by | The #175 implementer |
| Used in | `drivers/d3d12/rendering_device_driver_d3d12.cpp` (`_check_capabilities`, `RD_TO_D3D12_FORMAT`, `texture_get_usages_supported_by_format`, `has_feature`) |
| Copied files | None |

How it was used: the four options are read and logged, and `has_feature` reports false until #123. `DATA_FORMAT_R64_UINT` maps to `R32G32_TYPELESS` with an `R32G32_UINT` view, and is limited to storage and copy usages. Godot binds UAVs through descriptor heaps, not root descriptors, so once the toolchain emits 64-bit atomics the buffer flag also needs `AtomicInt64OnDescriptorHeapResourceSupported`, the image flag `AtomicInt64OnTypedResourceSupported`, and the arithmetic flag `AtomicInt64OnGroupSharedSupported`.
