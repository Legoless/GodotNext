extends SceneTree

## Driver-callback pipeline-stage check for #101.
##
## Runs a compute pass that writes texture A, then a dev-only driver callback that
## clears A, then a compute pass that samples A into B. The callback declares its
## pipeline stages; `--force-zero-stages` drops them. B must hold the clear colour
## either way on a device whose barriers honor stages; on Vulkan, the forced-zero
## run is the sync-hazard reproducer used with the synchronization validation layer.
##
## Run (dev build required):
##   GODOTNEXT_BACKGROUND=1 godot --path godotnext/testbed --rendering-driver metal \
##     --script res://tests/rd_callback_sync.gd [-- --force-zero-stages]
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SIZE := 64
const WRITE_COLOR := Color(1.0, 0.0, 0.0, 1.0)
const CLEAR_COLOR := Color(0.25, 0.5, 0.75, 1.0)
const TOLERANCE := 1.0 / 255.0

var _failures := PackedStringArray()


func _initialize() -> void:
	var force_zero := "--force-zero-stages" in OS.get_cmdline_user_args()
	var result := _run(force_zero)
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if result["pass"] else 1)


func _run(force_zero: bool) -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return _result(false, force_zero, "NO_RENDERING_DEVICE")
	if not rd.has_method("_dev_driver_callback_clear"):
		return _result(false, force_zero, "DEV_BUILD_REQUIRED")

	var write_shader := _load_shader(rd, "res://tests/rd_callback_write.glsl")
	var copy_shader := _load_shader(rd, "res://tests/rd_callback_copy.glsl")
	if not write_shader.is_valid() or not copy_shader.is_valid():
		return _result(false, force_zero, "SHADER_COMPILE_FAILED")

	var usage := RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	var format := RDTextureFormat.new()
	format.width = SIZE
	format.height = SIZE
	format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	format.usage_bits = usage
	var a := rd.texture_create(format, RDTextureView.new(), [])
	var b := rd.texture_create(format, RDTextureView.new(), [])
	if not a.is_valid() or not b.is_valid():
		return _result(false, force_zero, "TEXTURE_CREATE_FAILED")

	# Pass 1: write A.
	var write_image := RDUniform.new()
	write_image.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	write_image.binding = 0
	write_image.add_id(a)
	var write_set := rd.uniform_set_create([write_image], write_shader, 0)
	var write_pipeline := rd.compute_pipeline_create(write_shader)
	var write_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(write_list, write_pipeline)
	rd.compute_list_bind_uniform_set(write_list, write_set, 0)
	rd.compute_list_set_push_constant(write_list, _color_bytes(WRITE_COLOR), 16)
	rd.compute_list_dispatch(write_list, SIZE / 8, SIZE / 8, 1)
	rd.compute_list_end()

	# Pass 2: the driver callback clears A with the declared stages.
	rd.call("_dev_driver_callback_clear", a, CLEAR_COLOR, force_zero)

	# Pass 3: sample A into B.
	var sampler := rd.sampler_create(RDSamplerState.new())
	var copy_source := RDUniform.new()
	copy_source.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	copy_source.binding = 0
	copy_source.add_id(sampler)
	copy_source.add_id(a)
	var copy_dest := RDUniform.new()
	copy_dest.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	copy_dest.binding = 1
	copy_dest.add_id(b)
	var copy_set := rd.uniform_set_create([copy_source, copy_dest], copy_shader, 0)
	var copy_pipeline := rd.compute_pipeline_create(copy_shader)
	var copy_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(copy_list, copy_pipeline)
	rd.compute_list_bind_uniform_set(copy_list, copy_set, 0)
	rd.compute_list_dispatch(copy_list, SIZE / 8, SIZE / 8, 1)
	rd.compute_list_end()

	rd.submit()
	rd.sync()

	var data := rd.texture_get_data(b, 0)
	var ok := _check_cleared(data)
	rd.free_rid(copy_set)
	rd.free_rid(copy_pipeline)
	rd.free_rid(copy_shader)
	rd.free_rid(write_set)
	rd.free_rid(write_pipeline)
	rd.free_rid(write_shader)
	rd.free_rid(sampler)
	rd.free_rid(a)
	rd.free_rid(b)
	rd.free()
	return _result(ok, force_zero, "" if ok else "CLEAR_MISSING")


func _check_cleared(p_data: PackedByteArray) -> bool:
	# RGBA16F: eight bytes per texel. Compare the first and last texels.
	if p_data.is_empty():
		return false
	for offset in [0, p_data.size() - 8]:
		var r := _half_to_float(p_data.decode_u16(offset))
		var g := _half_to_float(p_data.decode_u16(offset + 2))
		var bl := _half_to_float(p_data.decode_u16(offset + 4))
		if absf(r - CLEAR_COLOR.r) > TOLERANCE or absf(g - CLEAR_COLOR.g) > TOLERANCE or absf(bl - CLEAR_COLOR.b) > TOLERANCE:
			return false
	return true


func _half_to_float(p_bits: int) -> float:
	# Minimal half-to-float for the 0..1 values this fixture compares.
	var sign := -1.0 if (p_bits & 0x8000) != 0 else 1.0
	var exponent := (p_bits >> 10) & 0x1F
	var mantissa := p_bits & 0x3FF
	if exponent == 0:
		return sign * pow(2.0, -14.0) * (float(mantissa) / 1024.0)
	return sign * pow(2.0, float(exponent - 15)) * (1.0 + float(mantissa) / 1024.0)


func _color_bytes(p_color: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(16)
	bytes.encode_float(0, p_color.r)
	bytes.encode_float(4, p_color.g)
	bytes.encode_float(8, p_color.b)
	bytes.encode_float(12, 1.0)
	return bytes


func _load_shader(rd: RenderingDevice, p_path: String) -> RID:
	var file := load(p_path) as RDShaderFile
	if file == null:
		_failures.append("shader file loads: " + p_path)
		return RID()
	var spirv := file.get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty():
		_failures.append("shader compiles: " + p_path)
		return RID()
	return rd.shader_create_from_spirv(spirv, p_path.get_file().get_basename())


func _result(p_pass: bool, p_forced: bool, p_reason: String) -> Dictionary:
	return {
		"test": "rd_callback_sync",
		"forced_zero": p_forced,
		"pass": p_pass,
		"reason": p_reason,
		"failures": _failures,
	}
