extends CompositorEffect

## Reads the render buffers after the opaque pass for frame_block_check.gd (#26): whether the
## depth + normal-roughness prepass texture exists, and the size of RB_TEX_AMBIENT ("rbgi" /
## "ambient"). It sets no needs_* flag, so it does not change the prepass mode itself.

var calls := 0
var internal_size := Vector2i.ZERO
var normal_roughness := false
var ambient_size := Vector2i.ZERO


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_OPAQUE


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null:
		return
	calls += 1
	internal_size = buffers.get_internal_size()
	normal_roughness = buffers.has_texture(&"forward_clustered", &"normal_roughness")
	ambient_size = Vector2i.ZERO
	if buffers.has_texture(&"rbgi", &"ambient"):
		var format := RenderingServer.get_rendering_device().texture_get_format(buffers.get_texture(&"rbgi", &"ambient"))
		ambient_size = Vector2i(format.width, format.height)
