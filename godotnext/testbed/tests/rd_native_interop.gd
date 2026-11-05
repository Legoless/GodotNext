extends SceneTree

## Native interop check for #102.
##
## Builds a layered texture, a slice view and a depth view on a local
## RenderingDevice and reads the vendor metadata back. On Vulkan the image, view,
## format and subresource range must match get_driver_resource() and the command
## buffer handle must be non-zero; on Metal and D3D12 both report no handle and
## ERR_UNAVAILABLE. An unknown RID must give ERR_INVALID_PARAMETER.
##
## Run (dev build required):
##   GODOTNEXT_BACKGROUND=1 godot --path godotnext/testbed --rendering-driver metal \
##     --script res://tests/rd_native_interop.gd
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const WIDTH := 256
const HEIGHT := 128
const MIPS := 3
const LAYERS := 2

var _failures := PackedStringArray()


func _initialize() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return _result(false, "NO_RENDERING_DEVICE")
	if not rd.has_method("_dev_texture_native_info"):
		return _result(false, "DEV_BUILD_REQUIRED")

	var usage := RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT

	var array_format := RDTextureFormat.new()
	array_format.width = WIDTH
	array_format.height = HEIGHT
	array_format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	array_format.texture_type = RenderingDevice.TEXTURE_TYPE_2D_ARRAY
	array_format.array_layers = LAYERS
	array_format.mipmaps = MIPS
	array_format.usage_bits = usage
	var array_texture := rd.texture_create(array_format, RDTextureView.new(), [])

	var slice_view := rd.texture_create_shared_from_slice(RDTextureView.new(), array_texture, 1, 1, 1, RenderingDevice.TEXTURE_SLICE_2D, 1)

	var depth_format := RDTextureFormat.new()
	depth_format.width = WIDTH
	depth_format.height = HEIGHT
	depth_format.format = RenderingDevice.DATA_FORMAT_D32_SFLOAT
	depth_format.usage_bits = usage | RenderingDevice.TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
	var depth_texture := rd.texture_create(depth_format, RDTextureView.new(), [])
	var depth_view := rd.texture_create_shared(RDTextureView.new(), depth_texture)

	if not array_texture.is_valid() or not slice_view.is_valid() or not depth_texture.is_valid() or not depth_view.is_valid():
		return _result(false, "TEXTURE_CREATE_FAILED")

	var array_info: Dictionary = rd.call("_dev_texture_native_info", array_texture)
	var vulkan := int(array_info["error"]) == OK
	var slice_info: Dictionary = rd.call("_dev_texture_native_info", slice_view)
	var depth_info: Dictionary = rd.call("_dev_texture_native_info", depth_view)
	var unknown: Dictionary = rd.call("_dev_texture_native_info", RID())

	# The command-buffer handle only exists inside a callback.
	rd.call("_dev_capture_command_buffer_handle")
	rd.submit()
	rd.sync()
	var command_buffer := int(rd.call("_dev_get_captured_command_buffer_handle"))

	if int(unknown.get("error", 0)) != ERR_INVALID_PARAMETER:
		_failures.append("an unknown RID did not return ERR_INVALID_PARAMETER")

	if vulkan:
		if int(array_info["error"]) != OK:
			_failures.append("array texture native info failed")
		if int(array_info["image"]) == 0 or int(array_info["image"]) != int(rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE, array_texture, 0)):
			_failures.append("array image handle mismatch")
		if int(array_info["view"]) != int(rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE_VIEW, array_texture, 0)):
			_failures.append("array view handle mismatch")
		if int(array_info["format"]) != int(rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, array_texture, 0)):
			_failures.append("array format mismatch")
		if int(slice_info["base_layer"]) != 1 or int(slice_info["layer_count"]) != 1:
			_failures.append("slice layer range mismatch")
		if int(slice_info["base_mipmap"]) != 1:
			_failures.append("slice mipmap base mismatch")
		if int(slice_info["width"]) != WIDTH / 2 or int(slice_info["height"]) != HEIGHT / 2:
			_failures.append("slice extent mismatch")
		if not bool(depth_info["aspect_depth"]) or bool(depth_info["aspect_color"]):
			_failures.append("depth aspect mismatch")
		if command_buffer == 0:
			_failures.append("the command buffer handle is zero on Vulkan")
	else:
		for info in [array_info, slice_info, depth_info]:
			if int(info["error"]) != ERR_UNAVAILABLE:
				_failures.append("a non-Vulkan driver reported native info")
			if int(info["image"]) != 0 or int(info["view"]) != 0:
				_failures.append("a non-Vulkan driver returned a native handle")
		if command_buffer != 0:
			_failures.append("the command buffer handle is non-zero on a non-Vulkan driver")

	rd.free_rid(depth_view)
	rd.free_rid(depth_texture)
	rd.free_rid(slice_view)
	rd.free_rid(array_texture)
	rd.free()
	return _result(_failures.is_empty(), "")


func _result(p_pass: bool, p_reason: String) -> Dictionary:
	return {
		"test": "rd_native_interop",
		"pass": p_pass,
		"reason": p_reason,
		"failures": _failures,
	}
