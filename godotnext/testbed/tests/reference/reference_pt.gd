extends Node
## Reference path tracer v0 (#19). Builds one BLAS per soup and a hit_sbt_range 0
## TLAS, then dispatches ref_pt.glsl. Without SUPPORTS_RAY_QUERY it prints the
## fallback result and exits 0.
##
## GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##   MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
##   godot --path godotnext/testbed --rendering-driver metal \
##   res://tests/reference/reference.tscn -- --fixture=furnace --spp=131072 --seed=1

const Soups = preload("res://tests/reference/soups.gd")

const FLAG_PATCH := 1
const FLAG_DIRECT := 2
const BATCH := 32
const FURNACE_EXPECTED := 5.0
const FURNACE_TOLERANCE := 0.01
const Q3_LIMIT := 0.001
const LUMA := Vector3(0.2126, 0.7152, 0.0722)


static func fallback_result(fixture: String) -> Dictionary:
	return {
		"pass": true,
		"fallback": "NO_RAY_QUERY",
		"fixture": fixture,
		"reason": "SUPPORTS_RAY_QUERY is absent, so the reference path tracer leaves the device idle.",
	}


func _ready() -> void:
	var exit_code := _main()
	get_tree().quit(exit_code)


func _main() -> int:
	var fixture := _arg("fixture", "furnace")
	if fixture == "fallback":
		_report(fallback_result(fixture))
		return 0
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		_report({"pass": false, "fixture": fixture, "reason": "NO_RENDERING_DEVICE"})
		return 1
	if not rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY):
		rd.free()
		_report(fallback_result(fixture))
		return 0
	var soup := _soup(fixture)
	if soup.is_empty():
		rd.free()
		_report({"pass": false, "fixture": fixture, "reason": "unknown fixture"})
		return 1
	var geometry_failures := _validate(fixture, soup)
	if not geometry_failures.is_empty():
		rd.free()
		_report({"pass": false, "fixture": fixture, "failures": geometry_failures})
		return 1
	var run := _trace(rd, soup, fixture)
	rd.free()
	_report(run["result"])
	return 0 if bool(run["result"]["pass"]) else 1


func _soup(fixture: String) -> Dictionary:
	if fixture == "furnace":
		return Soups.build_furnace()
	if fixture == "sealed_a":
		return Soups.build_sealed(0.10)
	if fixture == "sealed_b":
		return Soups.build_sealed(0.20)
	return {}


func _validate(fixture: String, soup: Dictionary) -> PackedStringArray:
	if fixture == "furnace":
		return Soups.validate_furnace(soup)
	return Soups.validate_sealed(soup)


func _trace(rd: RenderingDevice, soup: Dictionary, fixture: String) -> Dictionary:
	var failures := PackedStringArray()
	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = _kernel_source()
	var spirv := rd.shader_compile_spirv_from_source(source, false)
	if not spirv.compile_error_compute.is_empty():
		failures.append(spirv.compile_error_compute)
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}
	var shader := rd.shader_create_from_spirv(spirv, "ref_pt")
	if not shader.is_valid():
		failures.append("shader_create_from_spirv failed")
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		failures.append("compute_pipeline_create failed")
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}

	var freed: Array[RID] = []
	var input_bits := RenderingDevice.BUFFER_CREATION_DEVICE_ADDRESS_BIT | RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT
	var vertices: PackedFloat32Array = soup["vertices"]
	var vertex_buffer := rd.storage_buffer_create(vertices.size() * 4, vertices.to_byte_array(), 0, input_bits)
	var geometry := RDAccelerationStructureGeometry.new()
	geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geometry.vertex_buffer = vertex_buffer
	geometry.vertex_stride = 12
	geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geometry.vertex_count = int(soup["triangle_count"]) * 3
	var blas := rd.blas_create([geometry], RenderingDevice.ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT)
	if not blas.is_valid() or rd.blas_build(blas) != OK:
		_free(rd, [vertex_buffer, blas, pipeline, shader])
		failures.append("BLAS build failed")
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}
	var tlas := rd.tlas_create(1, 0)
	var instance := RDAccelerationStructureInstance.new()
	instance.blas = blas
	instance.id = 0
	instance.mask = 0xFF
	instance.hit_sbt_range = 0
	instance.transform = Transform3D.IDENTITY
	if not tlas.is_valid() or rd.tlas_build(tlas, [instance]) != OK:
		_free(rd, [tlas, blas, vertex_buffer, pipeline, shader])
		failures.append("TLAS build failed")
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}

	var camera: Dictionary = soup["camera"]
	var width := int(camera["width"])
	var height := int(camera["height"])
	var pixel_count := width * height
	var radiance := rd.storage_buffer_create(pixel_count * 16, _zeros(pixel_count * 16))
	var triangle_floats: PackedFloat32Array = soup["triangle_floats"]
	var materials: PackedFloat32Array = soup["materials"]
	var lights: PackedFloat32Array = soup["lights"]
	var patches: PackedFloat32Array = soup["patches"]
	var triangle_buffer := rd.storage_buffer_create(triangle_floats.size() * 4, triangle_floats.to_byte_array())
	var material_buffer := rd.storage_buffer_create(materials.size() * 4, materials.to_byte_array())
	var light_buffer := rd.storage_buffer_create(lights.size() * 4, lights.to_byte_array())
	var patch_buffer := rd.storage_buffer_create(patches.size() * 4, patches.to_byte_array())
	var instance_buffer := rd.storage_buffer_create(16, PackedInt32Array([0, 0, 0, 0]).to_byte_array())
	var uniform_set := rd.uniform_set_create([
		_uniform(RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, tlas),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 1, radiance),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 2, triangle_buffer),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 3, material_buffer),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 4, light_buffer),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 5, patch_buffer),
		_uniform(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, 6, instance_buffer),
	], shader, 0)
	freed = [uniform_set, instance_buffer, patch_buffer, light_buffer, material_buffer, triangle_buffer, radiance, tlas, blas, vertex_buffer, pipeline, shader]
	if not uniform_set.is_valid():
		_free(rd, freed)
		failures.append("uniform set failed")
		return {"result": {"pass": false, "fixture": fixture, "failures": failures}}

	var spp := _int_arg("spp", 131072 if fixture == "furnace" else 65536)
	var seed := _int_arg("seed", 1)
	var runs := _int_arg("runs", 1)
	var patch_mode := fixture != "furnace"
	var hashes: PackedStringArray = []
	var payload := {}
	for run in runs:
		var full := _accumulate(rd, soup, pipeline, uniform_set, radiance, pixel_count, spp, seed, FLAG_PATCH if patch_mode else 0)
		var direct := PackedFloat32Array()
		if patch_mode:
			direct = _accumulate(rd, soup, pipeline, uniform_set, radiance, pixel_count, spp, seed, FLAG_PATCH | FLAG_DIRECT)
		payload = _payload(fixture, soup, spp, seed, full, direct)
		var digest := _sha256(payload["bytes"])
		hashes.append(digest)
		print("ref_pt run %d sha256 %s" % [run + 1, digest])
	if runs > 1:
		for hash in hashes:
			if hash != hashes[0]:
				failures.append("same-seed runs differ")
				break
	var reported: PackedStringArray = payload["result"].get("failures", PackedStringArray())
	reported.append_array(failures)
	payload["result"]["sha256"] = hashes
	payload["result"]["failures"] = reported
	payload["result"]["pass"] = reported.is_empty() and bool(payload["result"]["pass"])
	var out := _arg("out", "")
	if not out.is_empty():
		_write_outputs(out, fixture, payload)
	_free(rd, freed)
	return {"result": payload["result"]}


func _accumulate(rd: RenderingDevice, soup: Dictionary, pipeline: RID, uniform_set: RID, radiance: RID, pixel_count: int, spp: int, seed: int, flags: int) -> PackedFloat32Array:
	var cleared := _zeros(pixel_count * 16)
	rd.buffer_update(radiance, 0, cleared.size(), cleared)
	var done := 0
	while done < spp:
		var count := mini(BATCH, spp - done)
		var push := _push(soup, done, count, seed, flags)
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
		rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		rd.compute_list_set_push_constant(compute_list, push, push.size())
		var camera: Dictionary = soup["camera"]
		rd.compute_list_dispatch(compute_list, ceili(float(camera["width"]) / 8.0), ceili(float(camera["height"]) / 8.0), 1)
		rd.compute_list_end()
		rd.submit()
		rd.sync()
		done += count
		if done == count or done == spp or done % 8192 == 0:
			print("ref_pt flags %d samples %d/%d" % [flags, done, spp])
	return _means(rd.buffer_get_data(radiance).to_float32_array())


func _payload(fixture: String, soup: Dictionary, spp: int, seed: int, full: PackedFloat32Array, direct: PackedFloat32Array) -> Dictionary:
	if fixture == "furnace":
		return _furnace_payload(soup, spp, seed, full)
	return _sealed_payload(fixture, soup, spp, seed, full, direct)


func _furnace_payload(soup: Dictionary, spp: int, seed: int, means: PackedFloat32Array) -> Dictionary:
	var camera: Dictionary = soup["camera"]
	var width := int(camera["width"])
	var height := int(camera["height"])
	var pixels := width * height
	var sum := 0.0
	var worst := 0.0
	var outside := 0
	for i in pixels:
		var value := (means[i * 4] + means[i * 4 + 1] + means[i * 4 + 2]) / 3.0
		sum += value
		var rel := absf(value - FURNACE_EXPECTED) / FURNACE_EXPECTED
		worst = maxf(worst, rel)
		if rel > FURNACE_TOLERANCE:
			outside += 1
	var mean := sum / float(pixels)
	var image := Image.create_from_data(width, height, false, Image.FORMAT_RGBAF, means.to_byte_array())
	var exr := image.save_exr_to_buffer()
	var summary := {
		"fixture": "furnace",
		"seed": seed,
		"spp": spp,
		"width": width,
		"height": height,
		"expected": FURNACE_EXPECTED,
		"mean": mean,
		"max_rel_deviation": worst,
		"pixels_outside_1pct": outside,
		"pass": outside == 0,
	}
	var text := JSON.stringify(summary)
	var bytes := PackedByteArray()
	bytes.append_array(exr)
	bytes.append_array(text.to_utf8_buffer())
	return {
		"result": summary,
		"bytes": bytes,
		"json": text,
		"image": image,
	}


func _sealed_payload(fixture: String, soup: Dictionary, spp: int, seed: int, full: PackedFloat32Array, direct: PackedFloat32Array) -> Dictionary:
	var records: Array = soup["patch_records"]
	var lit := 0.0
	var dark := 0.0
	var rows: Array = []
	for i in records.size():
		var record: Dictionary = records[i]
		var full_rgb := Vector3(full[i * 4], full[i * 4 + 1], full[i * 4 + 2])
		var direct_rgb := Vector3(direct[i * 4], direct[i * 4 + 1], direct[i * 4 + 2])
		var indirect := full_rgb - direct_rgb
		var luma := indirect.dot(LUMA)
		if String(record["room"]) == "lit":
			lit += luma
		else:
			dark += luma
		var position: Vector3 = record["position"]
		var normal: Vector3 = record["normal"]
		rows.append({
			"room": record["room"],
			"wall": record["wall"],
			"position": [position.x, position.y, position.z],
			"normal": [normal.x, normal.y, normal.z],
			"full": [full_rgb.x, full_rgb.y, full_rgb.z],
			"direct": [direct_rgb.x, direct_rgb.y, direct_rgb.z],
			"indirect": [indirect.x, indirect.y, indirect.z],
			"indirect_luminance": luma,
		})
	lit /= 16.0
	dark /= 16.0
	var failures := PackedStringArray()
	var q3 := -1.0
	if lit <= 1.0e-4:
		failures.append("lit indirect luminance is %.6g" % lit)
	elif dark < -1.0e-3:
		failures.append("dark indirect luminance is %.6g" % dark)
	else:
		q3 = maxf(dark, 0.0) / lit
		if q3 >= Q3_LIMIT:
			failures.append("Q3 %.6g is not under 0.001" % q3)
	var summary := {
		"fixture": fixture,
		"seed": seed,
		"spp": spp,
		"thickness": soup["thickness"],
		"lit_indirect_luminance": lit,
		"dark_indirect_luminance": dark,
		"q3": q3,
		"patches": rows,
		"pass": failures.is_empty(),
		"failures": failures,
	}
	return {
		"result": summary,
		"bytes": JSON.stringify(summary).to_utf8_buffer(),
		"json": JSON.stringify(summary),
		"image": null,
	}


func _write_outputs(out: String, fixture: String, payload: Dictionary) -> void:
	DirAccess.make_dir_recursive_absolute(out)
	var json_path := out.path_join(fixture + ".json")
	var file := FileAccess.open(json_path, FileAccess.WRITE)
	if file:
		file.store_string(String(payload["json"]))
		file.close()
	var image: Variant = payload["image"]
	if image is Image:
		(image as Image).save_exr(out.path_join(fixture + ".exr"))


func _means(values: PackedFloat32Array) -> PackedFloat32Array:
	var means := PackedFloat32Array()
	means.resize(values.size())
	for i in range(0, values.size(), 4):
		var count := values[i + 3]
		var inv := 0.0 if count <= 0.0 else 1.0 / count
		means[i] = values[i] * inv
		means[i + 1] = values[i + 1] * inv
		means[i + 2] = values[i + 2] * inv
		means[i + 3] = 1.0
	return means


func _push(soup: Dictionary, sample_offset: int, sample_count: int, seed: int, flags: int) -> PackedByteArray:
	var camera: Dictionary = soup["camera"]
	var origin: Vector3 = camera["origin"]
	var forward: Vector3 = camera["forward"]
	var up: Vector3 = camera["up"]
	var right: Vector3 = camera["right"]
	var aspect := float(camera["width"]) / float(camera["height"])
	var buf := StreamPeerBuffer.new()
	buf.big_endian = false
	buf.put_32(int(camera["width"]))
	buf.put_32(int(camera["height"]))
	buf.put_32(sample_offset)
	buf.put_32(sample_count)
	buf.put_32(seed)
	buf.put_32(flags)
	buf.put_32(int(soup["light_count"]))
	buf.put_32(int(soup["triangle_count"]))
	_put_vec3(buf, origin)
	buf.put_float(float(camera["fov_y"]))
	_put_vec3(buf, forward)
	buf.put_float(aspect)
	_put_vec3(buf, up)
	buf.put_float(0.0)
	_put_vec3(buf, right)
	buf.put_float(0.0)
	return buf.data_array


func _put_vec3(buf: StreamPeerBuffer, value: Vector3) -> void:
	buf.put_float(value.x)
	buf.put_float(value.y)
	buf.put_float(value.z)


func _uniform(type: RenderingDevice.UniformType, binding: int, rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = type
	uniform.binding = binding
	uniform.add_id(rid)
	return uniform


func _zeros(size: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(size)
	return bytes


func _kernel_source() -> String:
	var file := FileAccess.open("res://tests/reference/ref_pt.glsl", FileAccess.READ)
	var kept: PackedStringArray = []
	for line in file.get_as_text().split("\n"):
		var stripped := line.strip_edges()
		if stripped.begins_with("#[") and stripped.ends_with("]"):
			continue
		kept.append(line)
	return "\n".join(kept)


func _sha256(bytes: PackedByteArray) -> String:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(bytes)
	return ctx.finish().hex_encode()


func _report(result: Dictionary) -> void:
	print("GODOTNEXT_RESULT " + JSON.stringify(result))


func _arg(name: String, default_value: String) -> String:
	var args := OS.get_cmdline_user_args()
	var prefix := "--" + name + "="
	for i in args.size():
		var token := String(args[i])
		if token.begins_with(prefix):
			return token.substr(prefix.length())
		if token == "--" + name and i + 1 < args.size():
			return String(args[i + 1])
	return default_value


func _int_arg(name: String, default_value: int) -> int:
	var text := _arg(name, "")
	if text.is_empty() or not text.is_valid_int():
		return default_value
	return int(text)


func _free(rd: RenderingDevice, rids: Array) -> void:
	for rid in rids:
		if rid is RID and (rid as RID).is_valid():
			rd.free_rid(rid)
