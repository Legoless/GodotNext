extends CompositorEffect

const SHADER_PATH := "res://harness/velocity_dump.glsl"

var output_dir := ""
var max_frames := 0
var _frame := 0
var _csv: FileAccess
var _shader := RID()
var _pipeline := RID()
var _counts_buffer := RID()
var _init_failed := false


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	needs_motion_vectors = true


func _ensure_pipeline(rd: RenderingDevice) -> bool:
	if _pipeline.is_valid():
		return true
	if _init_failed or rd == null:
		return false
	var spirv: RDShaderSPIRV = null
	if ResourceLoader.exists(SHADER_PATH):
		var loaded := load(SHADER_PATH) as RDShaderFile
		if loaded != null:
			spirv = loaded.get_spirv()
	if spirv == null:
		var text := FileAccess.get_file_as_string(SHADER_PATH)
		var compute_marker := text.find("#version 450")
		if compute_marker == -1:
			push_error("Q21 velocity_dump.glsl is missing #version 450")
			_init_failed = true
			return false
		var src := RDShaderSource.new()
		src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
		src.source_compute = text.substr(compute_marker)
		spirv = rd.shader_compile_spirv_from_source(src)
	if spirv == null or not spirv.compile_error_compute.is_empty():
		push_error("Q21 velocity shader compile failed: %s" % (spirv.compile_error_compute if spirv != null else "null SPIR-V"))
		_init_failed = true
		return false
	_shader = rd.shader_create_from_spirv(spirv, "Q21VelocityDump")
	if not _shader.is_valid():
		_init_failed = true
		return false
	_pipeline = rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		_init_failed = true
		return false
	_counts_buffer = rd.storage_buffer_create(8, PackedInt32Array([0, 0]).to_byte_array())
	return _counts_buffer.is_valid()


func _render_callback(_callback_type: int, render_data: RenderData) -> void:
	if max_frames > 0 and _frame >= max_frames:
		return
	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null:
		return
	var rd := RenderingServer.get_rendering_device()
	if not _ensure_pipeline(rd):
		return
	if not output_dir.is_empty() and _csv == null:
		DirAccess.make_dir_recursive_absolute(output_dir)
		_csv = FileAccess.open(output_dir.path_join("q21.csv"), FileAccess.WRITE)
		if _csv == null:
			push_error("Q21 failed to open q21.csv in %s" % output_dir)
			_init_failed = true
			return
		_csv.store_line("frame,view,sentinel,nonfinite")
	var mode := buffers.get_scaling_3d_mode()
	# Dense paths (DLSS / copy upscaler) and MetalFX report (-1, -1) sentinels;
	# on FSR2 and TAA, (-1, -1) is the legitimate static/sky clear value.
	var count_sentinel := 1 if (mode == Viewport.SCALING_3D_MODE_DLSS or mode == Viewport.SCALING_3D_MODE_METALFX_TEMPORAL) else 0
	var size := buffers.get_internal_size()
	for view in buffers.get_view_count():
		var velocity := buffers.get_velocity_layer(view, false)
		if not velocity.is_valid():
			if _csv != null:
				push_error("Q21 velocity buffer is missing")
			continue
		rd.buffer_clear(_counts_buffer, 0, 8)
		var image_uniform := RDUniform.new()
		image_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		image_uniform.binding = 0
		image_uniform.add_id(velocity)
		var count_uniform := RDUniform.new()
		count_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		count_uniform.binding = 1
		count_uniform.add_id(_counts_buffer)
		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, [image_uniform, count_uniform])
		if uniform_set.is_valid():
			var push := PackedInt32Array([size.x, size.y, count_sentinel, 0]).to_byte_array()
			var x_groups := int((size.x + 7) / 8)
			var y_groups := int((size.y + 7) / 8)
			var compute := rd.compute_list_begin()
			rd.compute_list_bind_compute_pipeline(compute, _pipeline)
			rd.compute_list_bind_uniform_set(compute, uniform_set, 0)
			rd.compute_list_set_push_constant(compute, push, push.size())
			rd.compute_list_dispatch(compute, x_groups, y_groups, 1)
			rd.compute_list_end()
			if _csv != null:
				var data := rd.buffer_get_data(_counts_buffer)
				if data.size() >= 8:
					_csv.store_line("%d,%d,%d,%d" % [_frame, view, data.decode_u32(0), data.decode_u32(4)])
					_csv.flush()
	_frame += 1


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	if _csv != null:
		_csv.close()
		_csv = null
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		if _counts_buffer.is_valid():
			rd.free_rid(_counts_buffer)
		if _pipeline.is_valid():
			rd.free_rid(_pipeline)
		if _shader.is_valid():
			rd.free_rid(_shader)
