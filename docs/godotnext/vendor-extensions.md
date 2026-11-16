# Vendor extensions: upscalers and frame generation

UE vendors ship their upscalers and frame generation as plugins over public engine seams.
GodotNext exposes the same two seams to GDExtension (plan §21.5 U2):

- `TemporalUpscalerExtension`, the counterpart of UE's `ITemporalUpscaler`, for an upscaler
  selected by the `CUSTOM` 3D scaling mode;
- `FrameGenerationExtension`, for a frame-generation provider on the main window.

GodotNext contains no AMD, Intel or NVIDIA SDK file, and its CI builds none (plan §19;
`misc/scripts/check_proprietary_sdk_files.py` rejects them). A vendor or the community builds the
plugin against its own SDK and ships it as a GDExtension.

`tests/vendor_seam_test` is a test-only sample of both seams in plain C, without godot-cpp. Its
`SeamCopy` upscaler copies the colour input to the output with a compute shader recorded through
`RenderingDevice`, and its `SeamPassthrough` provider generates no frames. Read it beside this
page.

## Registering a provider

1. Register the classes at the `SCENE` initialization level. Both base classes exist from the
   `SERVERS` level on.
2. Create the instances and register them once the engine has started, in the main loop's startup
   callback (`register_main_loop_callbacks`). The `RenderingServer` singleton is not available yet
   at the `SCENE` level. Call `RenderingServer.register_temporal_upscaler_extension()` and
   `RenderingServer.register_frame_generation_extension()`. The server keeps its own references.
3. The project selects the providers by the names their `_get_name()` returns:

   | Setting | Value |
   |---|---|
   | `rendering/scaling_3d/mode` | `8` (`SCALING_3D_MODE_CUSTOM`), or `Viewport.scaling_3d_mode` |
   | `rendering/scaling_3d/custom_upscaler` | the upscaler's name |
   | `rendering/frame_generation/provider` | the provider's name |
   | `rendering/frame_generation/multiplier` | 2 to 6, passed to `_set_mode()` |
   | `rendering/frame_generation/dynamic` | passed to `_set_mode()` |

4. In the shutdown callback, call the two `unregister_*` methods and then
   `RenderingServer.force_sync()`. The engine calls the shutdown callback before it shuts down
   and before it unloads the library. With a separate render thread the unregister calls are
   queued, so the sync makes sure no instance outlives its class. Then free your
   `RenderingDevice` resources (the sample frees its shader, which frees the pipeline too) and
   drop your own references. An instance that is still alive when its class is unregistered
   crashes the engine later. `RenderingDevice` accepts calls only on the rendering thread. With the
   default thread model that is the main thread, where the shutdown callback runs; with a separate
   rendering thread, free the resources there instead.

The engine releases any provider that is still registered when it shuts down. It frees an
unregistered upscaler at the end of the frame being drawn, or at once when no frame is being
drawn.

## The upscaler seam

`TemporalUpscalerExtension` replaces the built-in temporal upscaler of every viewport in `CUSTOM`
mode. Custom upscalers need the Forward+ renderer.

| Virtual | Called | Default |
|---|---|---|
| `_get_name()` | When the upscaler is registered | Required |
| `_is_available()` | When the upscaler is selected, and before each `_process()` | `false` |
| `_get_scale_range()` | When the viewport is configured; it clamps `scaling_3d_scale` | `(0.25, 2.0)` |
| `_get_mip_bias_offset()` | When the viewport is configured; it is added to `log2(min(scale, 1.0))` | `-0.5` |
| `_get_jitter_phase_count(target_size, render_size)` | When the viewport is configured; `0` renders without jitter | The engine's count |
| `_process(inputs)` | Once per frame for each viewport, on the rendering thread | Required |
| `_reset_history()` | Before the `_process()` of a frame whose history is reset | Nothing |

`TemporalUpscalerInputs` carries the frame:

| Member | Content |
|---|---|
| `color` | The internal-resolution colour (RGBA16F). You can sample it and read it as a storage image. |
| `depth`, `velocity`, `reactive` | The internal-resolution depth, motion vectors and reactive mask |
| `output` | The output-resolution texture to write (RGBA16F). You can write it as a storage image. |
| `internal_size`, `max_internal_size`, `output_size` | Sizes in pixels. `max_internal_size` is the dynamic-resolution maximum. |
| `motion_vector_scale`, `jitter` | The velocity scale to internal pixels and the camera jitter |
| `world_to_view`, `view_to_clip`, `z_near`, `z_far`, `fovy` | The camera |
| `delta_time`, `reset`, `view_index` | Frame time, history reset, view |
| `render_scene_buffers` | The viewport's `RenderSceneBuffersRD`, for history kept for each viewport |

When the selected upscaler is not registered or is unavailable, the viewport falls back to TSR.
It reports `VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE` and prints one warning. A
scale outside the range is clamped with one warning. A viewport with more than one view (XR) falls
back to TSR with `VIEW_UNSUPPORTED`.

## The frame-generation seam

`FrameGenerationExtension` receives what a frame generator needs from the main window: the 3D
image without the HUD, the UI layer, depth, velocity and the camera.

| Virtual | Called |
|---|---|
| `_get_name()` | When the provider is registered |
| `_is_available()` | On the rendering thread, after `_set_mode()`, whenever the settings or the registrations change. Returns a `FrameGenerationReason`. |
| `_set_mode(mode)` | Before `_is_available()` when the multiplier or `dynamic` changes: `{"multiplier": int, "dynamic": bool}` |
| `_reset()` | Before the `_on_frame_inputs()` of a frame that resets: the first frame after activation, `RenderingServer.viewport_reset_temporal_history()`, a resize |
| `_on_frame_inputs(inputs)` | Once per rendered frame of the main window, on the rendering thread |
| `_on_present(inputs, before)` | D3D12 only: just before and just after the main window's `IDXGISwapChain::Present()` |

`FrameGenerationInputs` carries the frame:

| Member | Content |
|---|---|
| `hudless_color` | The 3D image before the canvas layers above it, at `output_size` |
| `ui_color` | The canvas layers alone, premultiplied: RGBA8, or RGBA16F with HDR 2D |
| `depth`, `velocity`, `motion_vector_scale` | At `render_size`. Velocity exists while a temporal upscaler runs. |
| `render_size`, `output_size` | The 3D render size and the viewport size |
| `view_to_clip`, `clip_to_prev_clip`, `camera_transform`, `z_near`, `z_far`, `fovy`, `aspect`, `jitter` | The camera |
| `reset`, `frame_index` | History reset and the `RenderingDevice` frame counter |
| `swap_chain` | During `_on_present()` on D3D12, the `IDXGISwapChain` pointer being presented, otherwise 0 |

The engine reuses one inputs object. Copy the values you keep, not the object.

Rules:

- **One provider per process.** The first provider that activates keeps the process. Switching
  providers needs a restart. Requesting another provider reports `PROVIDER_ALREADY_ACTIVE` and
  prints one error, and the first provider keeps running. Registering another object under the
  active provider's name fails the same way. This is the rule Intel (XeSS-FG) and AMD (FSR's
  D3D12 swap-chain override) state for their UE plugins, because each overrides the swap chain
  (plan §21.5).
- **Main window only.** Frame generation runs only in the main window of a running game. It never
  runs in the editor, in a secondary window or in XR (`NOT_MAIN_WINDOW`).
- **One log line per fallback.** `RenderingServer.get_frame_generation_status()` reports the state
  and the reason. Each change to a fallback reason prints one line that names it.
- **The D3D12 present callback** exists only when the project names a provider at startup and no
  engine module owns the D3D12 present callback. Vulkan and Metal have no present hook. A provider
  that needs one should return `REASON_API_UNSUPPORTED` there.

## Render-thread rules

- `_process()`, `_is_available()` of a frame-generation provider, `_on_frame_inputs()` and
  `_on_present()` run on the rendering thread. With the default thread model that is the main
  thread inside `RenderingServer.draw()`.
- Record GPU work through `RenderingServer.get_rendering_device()`, as a `CompositorEffect` does.
  Do not create a local `RenderingDevice` for frame work. Do not stall the frame with readbacks or
  syncs from these callbacks.
- Keep the other virtuals cheap. They may run on either thread.
- Keep per-viewport state in `render_scene_buffers` (for example `RenderSceneBuffersRD.create_texture()`
  under your own context), so it is freed with the viewport. Do not use the buffers object as a
  dictionary key: that keeps a freed viewport's buffers alive.

## Native handles

`RenderingDevice.get_driver_resource(resource, rid, index)` returns the native objects a vendor SDK
takes:

| Resource | Vulkan | D3D12 |
|---|---|---|
| `DRIVER_RESOURCE_LOGICAL_DEVICE` | `VkDevice` | `ID3D12Device *` |
| `DRIVER_RESOURCE_PHYSICAL_DEVICE` | `VkPhysicalDevice` | `IDXGIAdapter *` |
| `DRIVER_RESOURCE_TOPMOST_OBJECT` | `VkInstance` | 0 |
| `DRIVER_RESOURCE_COMMAND_QUEUE` | `VkQueue` | `ID3D12CommandQueue *` |
| `DRIVER_RESOURCE_TEXTURE` (with the texture's RID) | `VkImage` | `ID3D12Resource *` |
| `DRIVER_RESOURCE_TEXTURE_VIEW` | `VkImageView` | `ID3D12Resource *` |
| `DRIVER_RESOURCE_TEXTURE_DATA_FORMAT` | `VkFormat` | `DXGI_FORMAT` |

On Metal, the logical device is the `MTLDevice`, the command queue the `MTLCommandQueue`, and
both texture resources the `MTLTexture`. The other resources are 0.

The textures in both inputs objects belong to the engine:

- Never free them, and never free or keep their native objects.
- They are valid during the call that passes them. The engine recreates them when it reconfigures
  the viewport, for example after a resize or a scale change. Their RIDs and native objects then
  change, so read them again in every call and keep nothing from an earlier frame.
- `RenderingDevice` tracks their layout (Vulkan) or resource state (D3D12) for the commands
  recorded through it, and adds the barriers. The seams promise no layout or state for native
  work on these objects, so record the passes through `RenderingDevice` (see "Known gaps").

## Known gaps

- **No swap-chain hook.** FSR frame generation replaces the DXGI swap chain with AMD's D3D12
  proxy swap chain, and plan §21.5 states that XeSS-FG overrides the swap chain too. U2 has only
  the D3D12 present callback, so a provider that must own the swap chain cannot run on it. This
  is a deviation candidate for §21.5.5, for the lead to decide.
- **No frame command buffer.** A GDExtension cannot record native commands into the frame's
  command buffer. `RenderingDevice.driver_callback_add()` is C++ only, for engine modules; it is
  also where a module declares the textures it touches, so the engine moves them to a known
  state first. A plugin records its passes through `RenderingDevice`, or submits native work on
  its own command list and then owns the synchronization and the resource states itself.
- **The first import can crash at exit.** A headless `--import` of a project whose first scan
  finds a GDExtension that registers classes crashes when the editor quits right after it, with
  any classes, also plain `RefCounted` ones. A second import, which loads the extension at
  startup, exits cleanly. The cause is not known yet. `run_vendor_seam_test.py` imports again
  only after that crash (the extension loaded and no error was logged).

## The test sample

`tests/vendor_seam_test` builds `godot/vendor_seam_test` from `src/vendor_seam.c`. Build it
against the engine it runs on:

```
<godot> --headless --dump-gdextension-interface
mkdir -p tests/vendor_seam_test/src/deps
mv gdextension_interface.h tests/vendor_seam_test/src/deps/
scons --directory=./tests/vendor_seam_test
```

Warnings are errors (GCC and Clang `-Wall -Wextra -Werror`, MSVC `/W4 /WX`).
`run_vendor_seam_test.py` imports the project, runs it and checks the log:

- `GODOT4_BIN=<godot> ./tests/vendor_seam_test/run_vendor_seam_test.py` is the headless check
  (no GPU). It checks registration, availability and unloading. Nothing renders headless, so frame
  generation reports `API_UNSUPPORTED`.
- `--rendered --driver vulkan|d3d12` runs the rendered check with GPU validation instead of the
  headless check, then the reference comparison. `--gbv` adds D3D12 GPU-based validation. The
  rendered check covers one `_process()` and one `_on_frame_inputs()` per frame, and one
  `_on_present()` pair per frame on D3D12. It checks that the copy equals the frame drawn with
  bilinear scaling at 1.0, that a scale
  outside the upscaler's range is clamped, that a second provider is refused with
  `PROVIDER_ALREADY_ACTIVE`, and that unloading the library falls back to TSR and
  `PROVIDER_UNAVAILABLE`.
- In `godot/main.gd`, `--seam-reference` draws the same frame with the GDScript samples of #192
  and #203 for comparison.
- `--check-log <log> --mode headless|rendered|reference` checks the log of a run that another
  runner made, with Godot's exit code from `--exit-code` or the `done.json` next to the log. A
  clean log counts only when validation ran: the Khronos layer is in the Vulkan chain, or the
  D3D12 debug layer's messages go to the log.

A CI job that runs the headless check on Linux is still to be added.
