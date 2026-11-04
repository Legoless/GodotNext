extends Node

## GPU-written TLAS instance parity (#57, §9 RT API item 9).
##
## Builds the same 1024 instances over four BLASes twice: once with the CPU
## `tlas_build` path, once by a compute pass that encodes the driver's own
## instance records and a `tlas_build_from_buffer`. Both TLASes then trace the
## same 4096 seeded rays, and the Q17 records must agree: custom index,
## primitive and committed exact; t within 1e-5 relative; barycentrics within
## 1e-5 absolute.
##
## Run (see README.md for all hosts):
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/rd_smoke/tlas_from_buffer.gd
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const GRID := 32
const INSTANCES := GRID * GRID
const RAYS := 4096
const ID_BASE := 1000
const SPACING := 10.0
const RECORD_SIZE := 32
const SEED := 0x4E455854
const REL_T_TOLERANCE := 1e-5
const ABS_BARY_TOLERANCE := 1e-5

const QUAD_VERTS: Array[float] = [-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0]
const QUAD_IDX: Array[int] = [0, 1, 2, 0, 2, 3]
const BIPY_VERTS: Array[float] = [0, 0.5, 0, 0, -0.5, 0, 0.5, 0, 0, 0, 0, 0.5, -0.5, 0, 0, 0, 0, -0.5]
const BIPY_IDX: Array[int] = [0, 3, 2, 0, 4, 3, 0, 5, 4, 0, 2, 5, 1, 2, 3, 1, 3, 4, 1, 4, 5, 1, 5, 2]
const TRI_VERTS: Array[float] = [0, 0, 0, 1, 0, 0, 0, 1, 0]
const SMALL_QUAD_VERTS: Array[float] = [-0.25, -0.25, 0, 0.25, -0.25, 0, 0.25, 0.25, 0, -0.25, 0.25, 0]

var _failures := PackedStringArray()


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "tlas_from_buffer", "pass": false, "reason": "NO_RENDERING_DEVICE" }
	if not rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY):
		rd.free()
		return { "test": "tlas_from_buffer", "pass": false, "reason": "NO_RAY_QUERY" }

	var record_size: int = rd.acceleration_structure_get_instance_size()
	_check(record_size == 64 or record_size == 72, "instance record size is 64 (Vulkan) or 72 (Metal), got %d" % record_size)

	var trace_kernel := _load_imported_kernel(rd, "res://tests/rd_smoke/rq_kernel.glsl")
	var fill_kernel := _compile_fill_kernel(rd, record_size)
	if not trace_kernel.is_valid() or not fill_kernel.is_valid():
		rd.free()
		return { "test": "tlas_from_buffer", "pass": false, "reason": "SHADER_COMPILE_FAILED", "failures": _failures }

	var empty_indices: Array[int] = []
	var built_quad := _build_blas(rd, QUAD_VERTS, QUAD_IDX)
	var built_bipy := _build_blas(rd, BIPY_VERTS, BIPY_IDX)
	var built_tri := _build_blas(rd, TRI_VERTS, empty_indices)
	var built_small := _build_blas(rd, SMALL_QUAD_VERTS, QUAD_IDX)
	var blas: Array[RID] = [built_quad[0], built_bipy[0], built_tri[0], built_small[0]]
	var blas_buffers: Array[RID] = []
	for built in [built_quad, built_bipy, built_tri, built_small]:
		for i in range(1, built.size()):
			if (built[i] as RID).is_valid():
				blas_buffers.append(built[i])

	var transforms := _instance_transforms()
	var instances: Array[RDAccelerationStructureInstance] = []
	for i in INSTANCES:
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blas[_which(i)]
		instance.id = ID_BASE + i
		instance.mask = 0xFF
		instance.hit_sbt_range = 0
		instance.transform = transforms[i]
		instances.append(instance)

	var cpu_tlas := rd.tlas_create(INSTANCES, 0)
	_check(cpu_tlas.is_valid() and rd.tlas_build(cpu_tlas, instances) == OK, "CPU TLAS builds")

	var gpu_tlas := _build_gpu_tlas(rd, fill_kernel, blas, record_size)
	var rays := _rays()

	var cpu_results := _trace(rd, trace_kernel, cpu_tlas, rays)
	var gpu_results := _trace(rd, trace_kernel, gpu_tlas, rays)
	var stats := _compare(cpu_results, gpu_results)

	var ok: bool = _failures.is_empty() and int(stats["mismatch"]) == 0
	_free_all(rd, [trace_kernel, fill_kernel, cpu_tlas, gpu_tlas])
	_free_all(rd, blas)
	_free_all(rd, blas_buffers)
	rd.free()
	return {
		"test": "tlas_from_buffer",
		"pass": ok,
		"record_size": record_size,
		"instances": INSTANCES,
		"rays": RAYS,
		"id_exact": stats["id_exact"],
		"max_rel_t": stats["max_rel_t"],
		"max_abs_bary": stats["max_abs_bary"],
		"mismatch": stats["mismatch"],
		"failures": _failures,
	}


func _which(index: int) -> int:
	return (index % GRID + index / GRID) % 4


func _instance_transforms() -> Array[Transform3D]:
	var transforms: Array[Transform3D] = []
	for i in INSTANCES:
		var x := (float(i % GRID) - float(GRID) * 0.5 + 0.5) * SPACING
		var y := (float(i / GRID) - float(GRID) * 0.5 + 0.5) * SPACING
		transforms.append(Transform3D(Basis(), Vector3(x, y, 0)))
	return transforms


func _build_blas(rd: RenderingDevice, verts: Array[float], indices: Array[int]) -> Array[RID]:
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_bytes := PackedFloat32Array(verts).to_byte_array()
	var vertex_buffer := rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, input_bits)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = verts.size() / 3
	var index_buffer := RID()
	if not indices.is_empty():
		var index_bytes := PackedInt32Array(indices).to_byte_array()
		index_buffer = rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, input_bits)
		geometry.index_buffer = index_buffer
		geometry.index_count = indices.size()
		geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	var blas := rd.blas_create([geometry], 0)
	_check(rd.blas_build(blas) == OK, "BLAS builds (%d triangles)" % (indices.size() / 3 if not indices.is_empty() else verts.size() / 9))
	return [blas, vertex_buffer, index_buffer]


func _compile_fill_kernel(rd: RenderingDevice, record_size: int) -> RID:
	var source := FileAccess.get_file_as_string("res://tests/rd_smoke/tlas_from_buffer_fill.glsl")
	_check(not source.is_empty(), "fill kernel source loads")
	if record_size == 72:
		source = source.replace("#version 460", "#version 460\n#define LAYOUT_72")
	var shader_source := RDShaderSource.new()
	shader_source.set_stage_source(RenderingDevice.SHADER_STAGE_COMPUTE, source)
	shader_source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	var spirv := rd.shader_compile_spirv_from_source(shader_source)
	_check(spirv != null and spirv.compile_error_compute.is_empty(), "fill kernel compiles: " + (spirv.compile_error_compute if spirv != null else "no SPIR-V"))
	if spirv == null:
		return RID()
	return rd.shader_create_from_spirv(spirv, "tlas_from_buffer_fill")


func _load_imported_kernel(rd: RenderingDevice, path: String) -> RID:
	var file := load(path) as RDShaderFile
	_check(file != null, "shader file loads: " + path)
	if file == null:
		return RID()
	var spirv := file.get_spirv()
	var shader := rd.shader_create_from_spirv(spirv, path.get_file().get_basename())
	return shader


func _build_gpu_tlas(rd: RenderingDevice, fill_kernel: RID, blas: Array[RID], record_size: int) -> RID:
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var records := rd.storage_buffer_create(INSTANCES * record_size, PackedByteArray(), 0, input_bits)
	var pipeline := rd.compute_pipeline_create(fill_kernel)
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = 0
	uniform.add_id(records)
	var uniform_set := rd.uniform_set_create([uniform], fill_kernel, 0)
	_check(pipeline.is_valid() and uniform_set.is_valid(), "fill pipeline and uniform set are created")

	var refs := PackedInt32Array()
	for rid in blas:
		var reference: int = rd.acceleration_structure_get_instance_reference(rid)
		refs.append(reference & 0xFFFFFFFF)
		refs.append((reference >> 32) & 0xFFFFFFFF)
	var push := PackedInt32Array([INSTANCES, GRID, refs[0], refs[1], refs[2], refs[3], refs[4], refs[5], refs[6], refs[7]]).to_byte_array()
	var compute_list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_set_push_constant(compute_list, push, push.size())
	rd.compute_list_dispatch(compute_list, ceili(INSTANCES / 64.0), 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()

	var referenced: Array[RID] = [blas[0], blas[1], blas[2], blas[3]]
	var tlas := rd.tlas_create(INSTANCES, 0)
	var err := rd.tlas_build_from_buffer(tlas, records, 0, INSTANCES, referenced, RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_BUILD)
	_check(err == OK, "GPU TLAS builds from the instance buffer (error %d)" % err)
	_free_all(rd, [uniform_set, pipeline, records])
	return tlas


func _rays() -> PackedByteArray:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var bytes := PackedByteArray()
	bytes.resize(RAYS * 32)
	for i in RAYS:
		var cell := rng.randi_range(0, INSTANCES - 1)
		var x := (float(cell % GRID) - float(GRID) * 0.5 + 0.5) * SPACING + rng.randf_range(-0.3, 0.3)
		var y := (float(cell / GRID) - float(GRID) * 0.5 + 0.5) * SPACING + rng.randf_range(-0.3, 0.3)
		bytes.encode_float(i * 32 + 0, x)
		bytes.encode_float(i * 32 + 4, y)
		bytes.encode_float(i * 32 + 8, 5.0)
		bytes.encode_float(i * 32 + 12, 0.0)
		bytes.encode_float(i * 32 + 16, 0.0)
		bytes.encode_float(i * 32 + 20, 0.0)
		bytes.encode_float(i * 32 + 24, -1.0)
		bytes.encode_float(i * 32 + 28, 20.0)
	return bytes


func _trace(rd: RenderingDevice, shader: RID, tlas: RID, rays: PackedByteArray) -> PackedByteArray:
	var pipeline := rd.compute_pipeline_create(shader)
	var ray_buffer := rd.storage_buffer_create(rays.size(), rays)
	var result_buffer := rd.storage_buffer_create(RAYS * RECORD_SIZE)
	var uniforms: Array[RDUniform] = []
	var bindings: Array[RID] = [tlas, ray_buffer, result_buffer]
	var types: Array[int] = [RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER]
	for i in 3:
		var uniform := RDUniform.new()
		uniform.uniform_type = types[i] as RenderingDevice.UniformType
		uniform.binding = i
		uniform.add_id(bindings[i])
		uniforms.append(uniform)
	var uniform_set := rd.uniform_set_create(uniforms, shader, 0)
	var data := PackedByteArray()
	if pipeline.is_valid() and uniform_set.is_valid():
		var push := PackedInt32Array([RAYS, 0, 0, 0]).to_byte_array()
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
		rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		rd.compute_list_set_push_constant(compute_list, push, push.size())
		rd.compute_list_dispatch(compute_list, ceili(RAYS / 64.0), 1, 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		data = rd.buffer_get_data(result_buffer)
	_free_all(rd, [uniform_set, result_buffer, ray_buffer, pipeline])
	return data


func _compare(cpu: PackedByteArray, gpu: PackedByteArray) -> Dictionary:
	var stats := { "id_exact": 0, "max_rel_t": 0.0, "max_abs_bary": 0.0, "mismatch": 0 }
	_check(cpu.size() == gpu.size() and cpu.size() == RAYS * RECORD_SIZE, "both traces returned %d records" % RAYS)
	if cpu.size() != gpu.size():
		return stats
	for i in RAYS:
		var o := i * RECORD_SIZE
		var cpu_id := cpu.decode_u32(o + 4)
		var gpu_id := gpu.decode_u32(o + 4)
		var cpu_primitive := cpu.decode_u32(o + 8)
		var gpu_primitive := gpu.decode_u32(o + 8)
		var cpu_committed := cpu.decode_u32(o + 24)
		var gpu_committed := gpu.decode_u32(o + 24)
		if cpu_id == gpu_id and cpu_primitive == gpu_primitive and cpu_committed == gpu_committed:
			stats["id_exact"] += 1
		else:
			stats["mismatch"] += 1
		if cpu_committed == 0 or gpu_committed == 0:
			continue
		var cpu_t := cpu.decode_float(o)
		var gpu_t := gpu.decode_float(o)
		var rel_t := absf(cpu_t - gpu_t) / maxf(absf(cpu_t), 1e-9)
		stats["max_rel_t"] = maxf(stats["max_rel_t"], rel_t)
		var bary := maxf(absf(cpu.decode_float(o + 12) - gpu.decode_float(o + 12)), absf(cpu.decode_float(o + 16) - gpu.decode_float(o + 16)))
		stats["max_abs_bary"] = maxf(stats["max_abs_bary"], bary)
		if bary > ABS_BARY_TOLERANCE:
			stats["mismatch"] += 1
	if stats["id_exact"] != RAYS:
		_failures.append("GPU TLAS IDs differ on %d of %d rays" % [RAYS - stats["id_exact"], RAYS])
	if stats["max_rel_t"] > REL_T_TOLERANCE:
		_failures.append("max relative t %f exceeds %f" % [stats["max_rel_t"], REL_T_TOLERANCE])
	return stats


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _free_all(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)
