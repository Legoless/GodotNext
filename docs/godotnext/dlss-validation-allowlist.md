# DLSS Vulkan validation allowlist

Object-attributed validation allowlist for NVIDIA NGX internal Vulkan objects (plan §14 Never cut, GU-2, #100).

## Rules

1. **Zero errors on GodotNext objects.** Any validation message whose `pObjects` list references a handle in `VulkanCreatedHandles` (`DEBUG_ENABLED`) or any `VkSurfaceKHR` is never allowlisted and still aborts under `--gpu-abort`. Shared `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, and frame `VkCommandBuffer` handles do not trigger rejection on their own.
2. **Exact triple match.** A non-GodotNext message skips the `--gpu-abort` crash (it is still logged as a warning tagged `[DLSS]`) only when its `(VUID, DLL SHA-256, Driver)` tuple matches a row in the table below.
3. **Requirements for adding a row:**
   - Lead review and sign-off.
   - Proof that the message is absent with `dlss_backend=mock`.
   - Proof that the message persists with `GODOT_RG_FULL_BARRIERS=1 GODOT_RG_REORDER=0`.

## Allowlist

`modules/dlss/SCsub` compiles every data row in this Markdown table into `dlss_validation_allowlist.gen.h`.

| VUID | DLL SHA-256 | Driver | Notes |
|---|---|---|---|
| `SYNC-HAZARD-WRITE-AFTER-WRITE` | `a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96` | `595.99.02` | NGX internal `vkCmdClearColorImage` on `nv.ngx.dlssd.resource` inside `nv.ngx.dlssd.Evaluate` (0 GodotNext objects; absent in mock; persists under `GODOT_RG_FULL_BARRIERS=1 GODOT_RG_REORDER=0`). |
| `VUID-vkCmdDraw-None-09600` | `a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96` | `595.99.02` | NGX internal `nv.ngx.dlssd.resource` first-frame layout inside `nv.ngx.dlssd.Evaluate` (0 GodotNext objects; absent in mock; persists under `GODOT_RG_FULL_BARRIERS=1 GODOT_RG_REORDER=0`). |
