extends Node

## AS calibration benchmark (#9, G0-6; plan §2.1 lines 1a and 1b, §13.1).
##
## On a local RenderingDevice, in the RT-arena input format (float32 positions,
## uint32 indices, storage-buffer inputs with DEVICE_ADDRESS and
## ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY, opaque geometry):
##
## - BLAS: 64 seeded grid-displaced meshes from 1k to 64k triangles (about 2M in
##   total), built with PREFER_FAST_TRACE and with PREFER_FAST_BUILD. Each build
##   is bracketed by its own timestamps; throughput is triangles per GPU ms per
##   flag and size bucket.
## - TLAS: tlas_create(N) and tlas_build for N = 10k, 50k and 100k instances of
##   the FAST_TRACE BLASes, seeded transforms within 200 m, hit_sbt_range 0,
##   mask 0xFF; PREFER_FAST_TRACE and PREFER_FAST_BUILD. GPU ms, and the CPU ms
##   of the tlas_build call.
## - Refit through RD (#45): a FAST_TRACE + ALLOW_UPDATE BLAS of 10k to 200k
##   triangles, vertices changed every iteration, rebuilt in UPDATE mode.
## - MEMORY_ACCELERATION_STRUCTURES for every configuration (#22).
##
## Every point runs --warmup untimed and --iters timed iterations; the output
## keeps p50 and p95. --runs repeats the whole measurement. The derivation
## (godotnext_metrics.as_calibration) turns the result into lines 1a and 1b.
## A device without ray query prints the fallback result and exits 0.
##
## Usage (user arguments after "--"):
##   --warmup=20 --iters=200 --runs=1 --out=<dir>
##   --tlas=10000,50000,100000 --refit=10000,50000,100000,200000
## Prints one "GODOTNEXT_RESULT <json>" line; with --out it also writes
## <out>/as_calibration.json.

const SCHEMA := 1
const SEED := 0x41534331
const BLAS_COUNT := 64
const BLAS_MIN_TRIANGLES := 1024
const BLAS_MAX_TRIANGLES := 65536
const TILE_SIZE := 10.0
const DISPLACEMENT := 0.25
const SCENE_RADIUS := 200.0
const DEFAULT_TLAS_SIZES := "10000,50000,100000"
const DEFAULT_REFIT_SIZES := "10000,50000,100000,200000"
# Size buckets for BLAS throughput: [label, max triangles inclusive].
const BUCKETS: Array = [["1k-8k", 8192], ["8k-32k", 32768], ["32k-64k", 65536]]
const REASON_NAMES := {
	RenderingDevice.RAY_QUERY_SUPPORTED: "RAY_QUERY_SUPPORTED",
	RenderingDevice.RAY_QUERY_REASON_NO_RAY_QUERY: "NO_RAY_QUERY",
	RenderingDevice.RAY_QUERY_REASON_D3D12_NO_DXR: "D3D12_NO_DXR",
	RenderingDevice.RAY_QUERY_REASON_METAL_SYNC_MODE: "METAL_SYNC_MODE",
	RenderingDevice.RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS: "METAL_NO_ARGUMENT_BUFFERS",
}

var _failures := PackedStringArray()


func _ready() -> void:
	var result := _run()
	result["pass"] = _failures.is_empty() and bool(result.get("pass", true))
	result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


## The result for a device without ray query: the benchmark does not run, the
## reason is reported, and the run passes. Static so the fallback check can
## call it with mocked capabilities.
static func fallback_result(has_ray_query: bool, reason: int, driver: String, adapter: String) -> Dictionary:
	if has_ray_query:
		return {}
	return {
		"test": "as_calibration",
		"schema": SCHEMA,
		"fallback": true,
		"reason": REASON_NAMES.get(reason, "UNKNOWN_%d" % reason),
		"driver": driver,
		"adapter": adapter,
		"pass": reason != RenderingDevice.RAY_QUERY_SUPPORTED,
	}


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "test": "as_calibration", "pass": false, "reason": "NO_RENDERING_DEVICE" }
	var driver := RenderingServer.get_current_rendering_driver_name()
	var adapter := rd.get_device_name()
	var fallback := fallback_result(rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY), rd.get_ray_query_unsupported_reason(), driver, adapter)
	if not fallback.is_empty():
		rd.free()
		return fallback

	var warmup := _int_arg("warmup", 20)
	var iters := maxi(1, _int_arg("iters", 200))
	var runs := maxi(1, _int_arg("runs", 1))
	var tlas_sizes := _int_list(_arg("tlas", DEFAULT_TLAS_SIZES))
	var refit_sizes := _int_list(_arg("refit", DEFAULT_REFIT_SIZES))

	var started := Time.get_ticks_msec()
	var meshes := _make_meshes()
	print("as_calibration: %d meshes, %d triangles in %d ms" % [meshes.size(), _sum_triangles(meshes), Time.get_ticks_msec() - started])
	var inputs := _upload_meshes(rd, meshes)
	var run_results: Array = []
	for run in runs:
		run_results.append(_measure(rd, meshes, inputs, tlas_sizes, refit_sizes, warmup, iters))
		print("as_calibration: run %d of %d done after %d ms" % [run + 1, runs, Time.get_ticks_msec() - started])
	_free(rd, inputs)
	rd.free()

	var result := {
		"test": "as_calibration",
		"schema": SCHEMA,
		"driver": driver,
		"adapter": adapter,
		"seed": SEED,
		"warmup": warmup,
		"iters": iters,
		"blas_meshes": meshes.size(),
		"blas_triangles": _sum_triangles(meshes),
		"tlas_sizes": tlas_sizes,
		"refit_sizes": refit_sizes,
		"runs": run_results,
		"pass": true,
	}
	var out_dir := _arg("out", "")
	if not out_dir.is_empty():
		DirAccess.make_dir_recursive_absolute(out_dir)
		var file := FileAccess.open(out_dir.path_join("as_calibration.json"), FileAccess.WRITE)
		_check(file != null, "result written to " + out_dir)
		if file != null:
			file.store_string(JSON.stringify(result, "\t"))
			file.close()
	return result


func _measure(rd: RenderingDevice, meshes: Array, inputs: Array, tlas_sizes: Array, refit_sizes: Array, warmup: int, iters: int) -> Dictionary:
	var blas := {}
	var fast_trace: Array[RID] = []
	for flag_name in ["fast_trace", "fast_build"]:
		var flag := _flag(flag_name)
		var before := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
		var blases := _create_blases(rd, meshes, inputs, flag)
		blas[flag_name] = _time_blases(rd, meshes, blases, warmup, iters)
		# This flag's structures only: the FAST_TRACE set is still alive while FAST_BUILD is measured.
		blas[flag_name]["memory_bytes"] = rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) - before
		if flag_name == "fast_trace":
			fast_trace = blases
		else:
			_free(rd, blases)

	var tlas := {}
	for count in tlas_sizes:
		var instances := _make_instances(fast_trace, count)
		var per_flag := {}
		for flag_name in ["fast_trace", "fast_build"]:
			per_flag[flag_name] = _time_tlas(rd, instances, _flag(flag_name), warmup, iters)
		tlas[str(count)] = per_flag
	_free(rd, fast_trace)

	var refit := {}
	for count in refit_sizes:
		refit[str(count)] = _time_refit(rd, count, warmup, iters)
	return { "blas": blas, "tlas": tlas, "refit_rd": refit }


func _flag(flag_name: String) -> int:
	return RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT if flag_name == "fast_trace" else RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT


# --- Geometry -----------------------------------------------------------------

# One grid mesh: columns x rows quads, two triangles each, z displaced by a
# seeded value per vertex. Returns {triangles, vertices, indices}.
func _grid_mesh(target_triangles: int, rng: RandomNumberGenerator) -> Dictionary:
	var quads := maxi(1, target_triangles / 2)
	var columns := int(ceil(sqrt(float(quads))))
	var rows := int(ceil(float(quads) / columns))
	var vertices := PackedFloat32Array()
	vertices.resize((columns + 1) * (rows + 1) * 3)
	var step_x := TILE_SIZE / columns
	var step_y := TILE_SIZE / rows
	var o := 0
	for y in rows + 1:
		for x in columns + 1:
			vertices[o] = x * step_x
			vertices[o + 1] = y * step_y
			vertices[o + 2] = rng.randf_range(-DISPLACEMENT, DISPLACEMENT)
			o += 3
	var indices := PackedInt32Array()
	indices.resize(columns * rows * 6)
	o = 0
	var stride := columns + 1
	for y in rows:
		for x in columns:
			var a := y * stride + x
			indices[o] = a
			indices[o + 1] = a + 1
			indices[o + 2] = a + stride
			indices[o + 3] = a + 1
			indices[o + 4] = a + stride + 1
			indices[o + 5] = a + stride
			o += 6
	return { "triangles": columns * rows * 2, "vertices": vertices, "indices": indices }


# 64 meshes with triangle counts spread evenly from 1k to 64k (about 2.1M in total).
func _make_meshes() -> Array:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var meshes: Array = []
	for i in BLAS_COUNT:
		var target := int(lerpf(BLAS_MIN_TRIANGLES, BLAS_MAX_TRIANGLES, float(i) / (BLAS_COUNT - 1)))
		meshes.append(_grid_mesh(target, rng))
	return meshes


func _sum_triangles(meshes: Array) -> int:
	var total := 0
	for mesh in meshes:
		total += int(mesh["triangles"])
	return total


# Uploads every mesh to storage buffers; returns [vertex, index, vertex, index, ...].
func _upload_meshes(rd: RenderingDevice, meshes: Array) -> Array:
	var buffers: Array = []
	for mesh in meshes:
		buffers.append_array(_upload(rd, mesh["vertices"], mesh["indices"]))
	return buffers


func _upload(rd: RenderingDevice, vertices: PackedFloat32Array, indices: PackedInt32Array) -> Array:
	var bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertex_bytes := vertices.to_byte_array()
	var index_bytes := indices.to_byte_array()
	var vertex_buffer := rd.storage_buffer_create(vertex_bytes.size(), vertex_bytes, 0, bits)
	var index_buffer := rd.storage_buffer_create(index_bytes.size(), index_bytes, 0, bits)
	_check(vertex_buffer.is_valid() and index_buffer.is_valid(), "geometry buffers are created")
	return [vertex_buffer, index_buffer]


func _geometry(vertex_buffer: RID, index_buffer: RID, vertex_count: int, index_count: int) -> RDAccelerationStructureGeometry:
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = vertex_count
	geometry.index_buffer = index_buffer
	geometry.index_count = index_count
	geometry.index_format = RenderingDevice.INDEX_BUFFER_FORMAT_UINT32
	return geometry


func _create_blases(rd: RenderingDevice, meshes: Array, inputs: Array, flags: int) -> Array[RID]:
	var blases: Array[RID] = []
	for i in meshes.size():
		var mesh: Dictionary = meshes[i]
		var geometry := _geometry(inputs[i * 2], inputs[i * 2 + 1], mesh["vertices"].size() / 3, mesh["indices"].size())
		var blas := rd.blas_create([geometry], flags)
		_check(blas.is_valid(), "BLAS %d is created" % i)
		blases.append(blas)
	return blases


# --- Timing -------------------------------------------------------------------

# Submits the frame, waits for it and returns the GPU time of each captured
# timestamp in nanoseconds, in capture order.
func _resolve_timestamps(rd: RenderingDevice) -> PackedInt64Array:
	rd.submit()
	rd.sync()
	var times := PackedInt64Array()
	for i in rd.get_captured_timestamps_count():
		times.append(rd.get_captured_timestamp_gpu_time(i))
	return times


func _time_blases(rd: RenderingDevice, meshes: Array, blases: Array[RID], warmup: int, iters: int) -> Dictionary:
	var per_mesh: Array = []
	for i in blases.size():
		per_mesh.append(PackedFloat64Array())
	for iteration in warmup + iters:
		rd.capture_timestamp("blas_start")
		for i in blases.size():
			_check(rd.blas_build(blases[i]) == OK, "BLAS %d builds" % i)
			rd.capture_timestamp("blas_%d" % i)
		var times := _resolve_timestamps(rd)
		_check(times.size() == blases.size() + 1, "BLAS timestamps resolve (%d of %d)" % [times.size(), blases.size() + 1])
		if iteration < warmup or times.size() != blases.size() + 1:
			continue
		for i in blases.size():
			per_mesh[i].append((times[i + 1] - times[i]) / 1e6)
	var meshes_out: Array = []
	var p50_total := 0.0
	var p95_total := 0.0
	var bucket_ms := {}
	var bucket_triangles := {}
	for bucket in BUCKETS:
		bucket_ms[bucket[0]] = 0.0
		bucket_triangles[bucket[0]] = 0
	for i in blases.size():
		var triangles: int = meshes[i]["triangles"]
		var p50 := _percentile(per_mesh[i], 0.50)
		var p95 := _percentile(per_mesh[i], 0.95)
		p50_total += p50
		p95_total += p95
		meshes_out.append({ "triangles": triangles, "p50_ms": p50, "p95_ms": p95 })
		var label := _bucket_of(triangles)
		bucket_ms[label] += p50
		bucket_triangles[label] += triangles
	var buckets := {}
	for label in bucket_ms:
		buckets[label] = {
			"triangles": bucket_triangles[label],
			"p50_ms": bucket_ms[label],
			"triangles_per_ms": bucket_triangles[label] / bucket_ms[label] if bucket_ms[label] > 0.0 else 0.0,
		}
	var total := _sum_triangles(meshes)
	return {
		"triangles": total,
		"p50_ms_total": p50_total,
		"p95_ms_total": p95_total,
		"triangles_per_ms": total / p50_total if p50_total > 0.0 else 0.0,
		"buckets": buckets,
		"meshes": meshes_out,
	}


func _bucket_of(triangles: int) -> String:
	for bucket in BUCKETS:
		if triangles <= int(bucket[1]):
			return bucket[0]
	return BUCKETS[-1][0]


# Seeded instances of the FAST_TRACE BLASes, uniform in a 200 m sphere with
# random rotations.
func _make_instances(blases: Array[RID], count: int) -> Array[RDAccelerationStructureInstance]:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED + count
	var instances: Array[RDAccelerationStructureInstance] = []
	instances.resize(count)
	for i in count:
		var direction := Vector3(rng.randfn(), rng.randfn(), rng.randfn()).normalized()
		var distance := SCENE_RADIUS * pow(rng.randf(), 1.0 / 3.0)
		var basis := Basis.from_euler(Vector3(rng.randf() * TAU, rng.randf() * TAU, rng.randf() * TAU))
		var instance := RDAccelerationStructureInstance.new()
		instance.blas = blases[i % blases.size()]
		instance.id = i
		instance.mask = 0xFF
		instance.hit_sbt_range = 0
		instance.transform = Transform3D(basis, direction * distance)
		instances[i] = instance
	return instances


func _time_tlas(rd: RenderingDevice, instances: Array[RDAccelerationStructureInstance], flags: int, warmup: int, iters: int) -> Dictionary:
	var before := rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES)
	var tlas := rd.tlas_create(instances.size(), flags)
	_check(tlas.is_valid(), "TLAS of %d instances is created" % instances.size())
	var gpu := PackedFloat64Array()
	var cpu := PackedFloat64Array()
	var memory := 0
	if tlas.is_valid():
		for iteration in warmup + iters:
			rd.capture_timestamp("tlas_start")
			var cpu_start := Time.get_ticks_usec()
			var err := rd.tlas_build(tlas, instances)
			var cpu_ms := (Time.get_ticks_usec() - cpu_start) / 1000.0
			rd.capture_timestamp("tlas_end")
			_check(err == OK, "TLAS of %d instances builds" % instances.size())
			var times := _resolve_timestamps(rd)
			if iteration < warmup or times.size() != 2:
				continue
			gpu.append((times[1] - times[0]) / 1e6)
			cpu.append(cpu_ms)
		memory = rd.get_memory_usage(RenderingDevice.MEMORY_ACCELERATION_STRUCTURES) - before
		rd.free_rid(tlas)
	return {
		"instances": instances.size(),
		"p50_ms": _percentile(gpu, 0.50),
		"p95_ms": _percentile(gpu, 0.95),
		"cpu_p50_ms": _percentile(cpu, 0.50),
		"cpu_p95_ms": _percentile(cpu, 0.95),
		"memory_bytes": memory,
	}


# Refit through RD: one FAST_TRACE + ALLOW_UPDATE BLAS, built once, then
# rebuilt in UPDATE mode after every vertex change. Two displaced vertex sets
# alternate, so every update moves every vertex.
func _time_refit(rd: RenderingDevice, triangles: int, warmup: int, iters: int) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED + triangles
	var mesh := _grid_mesh(triangles, rng)
	var other: PackedFloat32Array = mesh["vertices"].duplicate()
	for i in range(2, other.size(), 3):
		other[i] = rng.randf_range(-DISPLACEMENT, DISPLACEMENT)
	var sets: Array[PackedByteArray] = [mesh["vertices"].to_byte_array(), other.to_byte_array()]
	var buffers := _upload(rd, mesh["vertices"], mesh["indices"])
	var geometry := _geometry(buffers[0], buffers[1], mesh["vertices"].size() / 3, mesh["indices"].size())
	var flags := RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT | RenderingDevice.ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT
	var blas := rd.blas_create([geometry], flags)
	var gpu := PackedFloat64Array()
	_check(blas.is_valid() and rd.blas_build(blas) == OK, "refit BLAS of %d triangles builds" % mesh["triangles"])
	if blas.is_valid():
		_resolve_timestamps(rd)
		for iteration in warmup + iters:
			var data := sets[(iteration + 1) % 2]
			rd.buffer_update(buffers[0], 0, data.size(), data)
			rd.capture_timestamp("refit_start")
			var err := rd.blas_build(blas, RenderingDevice.ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE)
			rd.capture_timestamp("refit_end")
			_check(err == OK, "refit of %d triangles succeeds" % mesh["triangles"])
			var times := _resolve_timestamps(rd)
			if iteration < warmup or times.size() != 2:
				continue
			gpu.append((times[1] - times[0]) / 1e6)
		rd.free_rid(blas)
	_free(rd, buffers)
	var p50 := _percentile(gpu, 0.50)
	return {
		"triangles": mesh["triangles"],
		"p50_ms": p50,
		"p95_ms": _percentile(gpu, 0.95),
		"triangles_per_ms": mesh["triangles"] / p50 if p50 > 0.0 else 0.0,
	}


# --- Helpers ------------------------------------------------------------------

# Nearest-rank percentile; 0 for an empty list.
static func _percentile(values: PackedFloat64Array, fraction: float) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var rank := clampi(int(ceil(fraction * sorted.size())) - 1, 0, sorted.size() - 1)
	return sorted[rank]


func _check(condition: bool, what: String) -> void:
	if not condition and not _failures.has(what):
		_failures.append(what)


func _free(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)


func _int_list(text: String) -> Array:
	var values: Array = []
	for part in text.split(",", false):
		if part.strip_edges().is_valid_int():
			values.append(int(part))
	return values


func _arg(name: String, default_value: String) -> String:
	var prefix := "--" + name + "="
	for token in OS.get_cmdline_user_args():
		if String(token).begins_with(prefix):
			return String(token).substr(prefix.length())
	return default_value


func _int_arg(name: String, default_value: int) -> int:
	var text := _arg(name, "")
	return int(text) if text.is_valid_int() else default_value
