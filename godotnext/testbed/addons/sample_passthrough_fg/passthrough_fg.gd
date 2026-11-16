extends FrameGenerationExtension
## Test-only sample frame-generation provider (#203, plan §21.5 U2, GU+-1). Testbed only: it never
## ships in the engine, it is not a vendor provider, and it generates no frames. It passes every
## frame through and counts the calls, so the frame_generation scene can check the seam on a real
## renderer: one _on_frame_inputs per rendered frame, the resets, the inputs, and on D3D12 exactly
## one _on_present pair per presented frame. It can also report a chosen reason, for the fallback
## checks, and it reads the HUD-less colour and the UI layer back for comparison with other runs.

const PROVIDER_NAME := "SamplePassthrough"

## The name this instance registers under (a second instance can use another one).
var provider_name := PROVIDER_NAME
## Returned by _is_available while it is not REASON_OK.
var forced_reason: int = REASON_OK

var available_count := 0
var mode_count := 0
var last_mode := {}
var inputs_count := 0
var reset_count := 0
## The inputs_count at each _reset call.
var reset_at: Array[int] = []
## Values from the last _on_frame_inputs call; the inputs object itself is reused by the engine.
var last := {}
var hudless_texture := RID()
var ui_texture := RID()

var present_before_count := 0
var present_after_count := 0
## A before without an after, or an after without a before.
var present_order_errors := 0
## Pairs whose frame index is not above the previous pair's (two pairs for one frame).
var present_frame_errors := 0
var present_last_frame := -1
## Pairs whose frame index is the one the last _on_frame_inputs call carried (the same frame).
var present_matched := 0
## The other pairs: {"frame": inputs_count, "present_index": ..., "inputs_index": ...} for each.
var present_unmatched: Array[Dictionary] = []
## Pairs with no _on_frame_inputs call since the previous pair.
var presents_without_inputs := 0
var _inputs_frame := -1
var _inputs_since_present := false
var present_swap_chain := 0
var _in_present := false
## inputs_count of each frame whose output size differs from the frame before, and whether it reset.
var size_changed_at: Array[int] = []
var size_changed_reset: Array[bool] = []
var _last_output_size := Vector2i()


func _get_name() -> String:
	return provider_name


func _is_available() -> int:
	available_count += 1
	return forced_reason


func _set_mode(mode: Dictionary) -> void:
	mode_count += 1
	last_mode = mode.duplicate()


func _reset() -> void:
	reset_count += 1
	reset_at.append(inputs_count + 1)


func _on_frame_inputs(inputs: FrameGenerationInputs) -> void:
	inputs_count += 1
	_inputs_frame = inputs.frame_index
	_inputs_since_present = true
	if inputs_count > 1 and inputs.output_size != _last_output_size:
		size_changed_at.append(inputs_count)
		size_changed_reset.append(inputs.reset)
	_last_output_size = inputs.output_size
	hudless_texture = inputs.hudless_color
	ui_texture = inputs.ui_color
	last = {
		"has_hudless": inputs.hudless_color.is_valid(),
		"has_ui": inputs.ui_color.is_valid(),
		"has_depth": inputs.depth.is_valid(),
		"has_velocity": inputs.velocity.is_valid(),
		"render_size": inputs.render_size,
		"output_size": inputs.output_size,
		"motion_vector_scale": inputs.motion_vector_scale,
		"z_near": inputs.z_near,
		"z_far": inputs.z_far,
		"fovy": inputs.fovy,
		"aspect": inputs.aspect,
		"jitter": inputs.jitter,
		"reset": inputs.reset,
		"frame_index": inputs.frame_index,
		"camera_origin": inputs.camera_transform.origin,
		"clip_to_prev_clip_identity": inputs.clip_to_prev_clip == Projection.IDENTITY,
	}


func _on_present(inputs: Object, before: bool) -> void:
	var frame_inputs := inputs as FrameGenerationInputs
	if before:
		if _in_present:
			present_order_errors += 1
		_in_present = true
		present_before_count += 1
		var frame := frame_inputs.frame_index if frame_inputs != null else -1
		if present_last_frame >= 0 and frame <= present_last_frame:
			present_frame_errors += 1
		if frame == _inputs_frame:
			present_matched += 1
		else:
			present_unmatched.append({"frame": inputs_count, "present_index": frame, "inputs_index": _inputs_frame})
		if not _inputs_since_present:
			presents_without_inputs += 1
		_inputs_since_present = false
		present_last_frame = frame
		present_swap_chain = frame_inputs.swap_chain if frame_inputs != null else 0
	else:
		if not _in_present:
			present_order_errors += 1
		_in_present = false
		present_after_count += 1


## The HUD-less colour of the last rendered frame, read back (call it after the frame is drawn).
func read_hudless() -> Image:
	return _read(hudless_texture)


## The premultiplied UI layer of the last rendered frame, read back.
func read_ui() -> Image:
	return _read(ui_texture)


static func _read(texture: RID) -> Image:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not texture.is_valid():
		return null
	var format := rd.texture_get_format(texture)
	var image_format := Image.FORMAT_MAX
	match format.format:
		RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, RenderingDevice.DATA_FORMAT_R8G8B8A8_SRGB:
			image_format = Image.FORMAT_RGBA8
		RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT:
			image_format = Image.FORMAT_RGBAH
		_:
			push_error("SamplePassthrough: unexpected texture format %d" % format.format)
			return null
	var data := rd.texture_get_data(texture, 0)
	return Image.create_from_data(format.width, format.height, false, image_format, data)
