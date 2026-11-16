extends Node

## Storage-buffer atomics fixture (#277; plan §21.1, GR-2).
##
## On a local RenderingDevice, counts 32-bit atomicAdd on storage buffers in a compute shader and in
## a fragment shader, next to plain stores to the same buffers as a control. The compute pass runs
## --count workgroups of one invocation. The fragment pass draws --count instances of a triangle
## that covers a 1x1 target, so each instance runs the fragment shader once.
##
## Each pass binds two storage buffers, A (binding 0) and B (binding 1), each one counter word
## followed by --count slots, and a third, R (binding 2), for return values. Invocation i:
## - adds 1 to A's counter and ignores the result, as a plain counter does;
## - adds 1 to B's counter and stores the value the add returned in R[i];
## - stores i + 1 in slot i of A and of B.
## The counters start at a nonzero sentinel and R starts at all ones, so "no add landed" differs
## from a zeroed buffer or a failed readback, and R shows whether the adds ran (they return the
## sentinel up to sentinel + count - 1, in any order) even when their writes did not land.
##
## Paths, each "pass" or "fail" (or "not_checked", below), for the compute and the fragment pass:
## - <pass>_atomic: A's counter is the sentinel + count.
## - <pass>_atomic_return: B's counter is the sentinel + count, and R holds each returned value once.
## - <pass>_store: every slot of A and B holds i + 1 (the control).
##
## Under D3D12 GPU-based validation, atomics whose result is unused are lost (harness/d3d12_gbv.gd).
## When the run asked for it, none of A's adds landed (A's counter is still the sentinel), and B's
## path passed, <pass>_atomic is "not_checked" instead of "fail", and the result's "not_checked"
## field names it and gives the reason. Every other outcome is checked as usual, including a
## partial, excess or zeroed count on A, so the fixture shows the limit and passes again by itself
## once the atomics land.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure. The result names
## the adapter and whether the run asked for D3D12 GPU-based validation, so a validation run and a
## cross-check on another adapter (such as WARP) say what they ran on.
##
## User arguments: --count=N (default 100).

const D3D12Gbv = preload("res://harness/d3d12_gbv.gd")
const SceneArgs = preload("res://scenes/scene_args.gd")

const DEFAULT_COUNT := 100
const MAX_COUNT := 1 << 16
const SENTINEL := 0x1000

const BUFFERS := """
layout(set = 0, binding = 0, std430) restrict buffer BufferA {
	uint data[];
}
a;

layout(set = 0, binding = 1, std430) restrict buffer BufferB {
	uint data[];
}
b;

layout(set = 0, binding = 2, std430) restrict writeonly buffer Returns {
	uint data[];
}
returns;

void count_and_store(uint i) {
	atomicAdd(a.data[0], 1u);
	returns.data[i] = atomicAdd(b.data[0], 1u);
	a.data[1u + i] = i + 1u;
	b.data[1u + i] = i + 1u;
}
"""

const COMPUTE_SOURCE := """#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
%s
void main() {
	count_and_store(gl_WorkGroupID.x);
}
""" % BUFFERS

const VERTEX_SOURCE := """#version 450

layout(location = 0) flat out uint instance;

void main() {
	vec2 position = gl_VertexIndex == 0 ? vec2(-1.0, -1.0) : (gl_VertexIndex == 1 ? vec2(3.0, -1.0) : vec2(-1.0, 3.0));
	instance = uint(gl_InstanceIndex);
	gl_Position = vec4(position, 0.0, 1.0);
}
"""

const FRAGMENT_SOURCE := """#version 450

layout(location = 0) flat in uint instance;
layout(location = 0) out uint frag_instance;
%s
void main() {
	count_and_store(instance);
	frag_instance = instance;
}
""" % BUFFERS

var _failures := PackedStringArray()
var _not_checked := PackedStringArray()
var _gbv_requested := false


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var count := clampi(SceneArgs.int_value("--count", DEFAULT_COUNT), 1, MAX_COUNT)
	_gbv_requested = D3D12Gbv.requested()
	var result := {
		"test": "storage_atomics",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"d3d12_gpu_based_validation": _gbv_requested,
		"params": { "count": count, "sentinel": SENTINEL },
	}

	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	result["adapter"] = rd.get_device_name()

	var paths := {}
	var counts := {}
	_run_compute(rd, count, paths, counts)
	_run_fragment(rd, count, paths, counts)
	rd.free()

	result["paths"] = paths
	result["counts"] = counts
	if not _not_checked.is_empty():
		result["not_checked"] = { "paths": _not_checked, "reason": D3D12Gbv.REASON }
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


func _run_compute(rd: RenderingDevice, count: int, paths: Dictionary, counts: Dictionary) -> void:
	var source := RDShaderSource.new()
	source.source_compute = COMPUTE_SOURCE
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "the compute shader compiles: %s" % spirv.compile_error_compute)
	var shader := RID()
	if spirv.compile_error_compute.is_empty():
		shader = rd.shader_create_from_spirv(spirv, "storage_atomics_compute")
	_check(shader.is_valid(), "the compute shader is created")
	if not shader.is_valid():
		_fail_paths("compute", paths)
		return

	var buffers := _create_buffers(rd, count)
	var uniform_set := rd.uniform_set_create(_uniforms(buffers), shader, 0)
	var pipeline := rd.compute_pipeline_create(shader)
	_check(uniform_set.is_valid() and pipeline.is_valid(), "the compute uniform set and pipeline are created")
	if uniform_set.is_valid() and pipeline.is_valid():
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list, pipeline)
		rd.compute_list_bind_uniform_set(list, uniform_set, 0)
		rd.compute_list_dispatch(list, count, 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		_evaluate("compute", rd, buffers, count, paths, counts)
	else:
		_fail_paths("compute", paths)

	if pipeline.is_valid():
		rd.free_rid(pipeline)
	# Freeing the shader also frees the uniform set made from it.
	rd.free_rid(shader)
	for buffer: RID in buffers:
		if buffer.is_valid():
			rd.free_rid(buffer)


func _run_fragment(rd: RenderingDevice, count: int, paths: Dictionary, counts: Dictionary) -> void:
	var source := RDShaderSource.new()
	source.source_vertex = VERTEX_SOURCE
	source.source_fragment = FRAGMENT_SOURCE
	var spirv := rd.shader_compile_spirv_from_source(source)
	var errors := spirv.compile_error_vertex + spirv.compile_error_fragment
	_check(errors.is_empty(), "the raster shaders compile: %s" % errors)
	var shader := RID()
	if errors.is_empty():
		shader = rd.shader_create_from_spirv(spirv, "storage_atomics_raster")
	_check(shader.is_valid(), "the raster shader is created")
	if not shader.is_valid():
		_fail_paths("fragment", paths)
		return

	var format := RDTextureFormat.new()
	format.format = RenderingDevice.DATA_FORMAT_R32_UINT
	format.width = 1
	format.height = 1
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT
	var target := rd.texture_create(format, RDTextureView.new())
	var framebuffer := rd.framebuffer_create([target])
	var buffers := _create_buffers(rd, count)
	var uniform_set := rd.uniform_set_create(_uniforms(buffers), shader, 0)
	var blend := RDPipelineColorBlendState.new()
	blend.attachments = [RDPipelineColorBlendStateAttachment.new()]
	var pipeline := RID()
	if framebuffer.is_valid():
		pipeline = rd.render_pipeline_create(shader, rd.framebuffer_get_format(framebuffer), RenderingDevice.INVALID_FORMAT_ID, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)
	_check(framebuffer.is_valid() and uniform_set.is_valid() and pipeline.is_valid(), "the raster target, uniform set and pipeline are created")
	if framebuffer.is_valid() and uniform_set.is_valid() and pipeline.is_valid():
		var list := rd.draw_list_begin(framebuffer, RenderingDevice.DRAW_CLEAR_COLOR_0, PackedColorArray([Color(0, 0, 0, 0)]))
		rd.draw_list_bind_render_pipeline(list, pipeline)
		rd.draw_list_bind_uniform_set(list, uniform_set, 0)
		rd.draw_list_draw(list, false, count, 3)
		rd.draw_list_end()
		rd.submit()
		rd.sync()
		_evaluate("fragment", rd, buffers, count, paths, counts)
	else:
		_fail_paths("fragment", paths)

	if pipeline.is_valid():
		rd.free_rid(pipeline)
	rd.free_rid(shader)
	for buffer: RID in buffers:
		if buffer.is_valid():
			rd.free_rid(buffer)
	if framebuffer.is_valid():
		rd.free_rid(framebuffer)
	rd.free_rid(target)


# A and B: the sentinel counter, then zeroed slots. R: all ones.
func _create_buffers(rd: RenderingDevice, count: int) -> Array[RID]:
	var counted := PackedInt32Array()
	counted.resize(1 + count)
	counted.fill(0)
	counted[0] = SENTINEL
	var returns := PackedByteArray()
	returns.resize(count * 4)
	returns.fill(255)
	var bytes := counted.to_byte_array()
	var buffers: Array[RID] = [rd.storage_buffer_create(bytes.size(), bytes), rd.storage_buffer_create(bytes.size(), bytes), rd.storage_buffer_create(returns.size(), returns)]
	return buffers


func _uniforms(buffers: Array[RID]) -> Array[RDUniform]:
	var uniforms: Array[RDUniform] = []
	for binding in buffers.size():
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		uniform.binding = binding
		uniform.add_id(buffers[binding])
		uniforms.append(uniform)
	return uniforms


func _evaluate(pass_name: String, rd: RenderingDevice, buffers: Array[RID], count: int, paths: Dictionary, counts: Dictionary) -> void:
	var a := rd.buffer_get_data(buffers[0])
	var b := rd.buffer_get_data(buffers[1])
	var r := rd.buffer_get_data(buffers[2])
	if a.size() != (1 + count) * 4 or b.size() != (1 + count) * 4 or r.size() != count * 4:
		_check(false, "%s: the readbacks have the buffers' sizes" % pass_name)
		_fail_paths(pass_name, paths)
		return

	# How many adds landed on each counter, and how many returned values are distinct ones in range.
	var a_added := a.decode_u32(0) - SENTINEL
	var b_added := b.decode_u32(0) - SENTINEL
	var seen := PackedByteArray()
	seen.resize(count)
	seen.fill(0)
	var returns_valid := 0
	for i in count:
		var returned := r.decode_u32(i * 4) - SENTINEL
		if returned >= 0 and returned < count and seen[returned] == 0:
			seen[returned] = 1
			returns_valid += 1
	var stores := 0
	for i in count:
		if a.decode_u32((1 + i) * 4) == i + 1:
			stores += 1
		if b.decode_u32((1 + i) * 4) == i + 1:
			stores += 1

	counts[pass_name] = { "atomic": a_added, "atomic_return": b_added, "returns_valid": returns_valid, "stores": stores }
	var returned_ok := b_added == count and returns_valid == count
	paths[pass_name + "_atomic_return"] = _path(returned_ok, "%s: %d of %d atomic adds landed on B's counter, and %d of %d returned values are distinct and in range" % [pass_name, b_added, count, returns_valid, count])
	paths[pass_name + "_store"] = _path(stores == 2 * count, "%s: %d of %d stores landed" % [pass_name, stores, 2 * count])
	if a_added == 0 and _gbv_requested and returned_ok:
		# The GPU-based validation limit: every unused-result add is lost, the used-result ones land.
		paths[pass_name + "_atomic"] = D3D12Gbv.NOT_CHECKED
		_not_checked.append(pass_name + "_atomic")
	else:
		paths[pass_name + "_atomic"] = _path(a_added == count, "%s: %d of %d atomic adds landed on A's counter" % [pass_name, a_added, count])


func _path(ok: bool, message: String) -> String:
	_check(ok, message)
	return "pass" if ok else "fail"


func _fail_paths(pass_name: String, paths: Dictionary) -> void:
	for suffix in ["_atomic", "_atomic_return", "_store"]:
		paths[pass_name + suffix] = "fail"


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
