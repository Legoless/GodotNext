extends Node

## 64-bit atomics fixture (#175 step 6; plan §21.1 C3, GR-1).
##
## On a local RenderingDevice, scatters --count seeded values with uint64 atomicMax and atomicMin
## into an --entries-entry uint64 storage buffer and into a --width x --height R64_UINT storage
## image, reads both back and compares them byte for byte with a CPU reference. Each value is
## depth << 32 | id: depth is a seeded 32-bit number (its top bit is often set, so the unsigned
## comparison matters) and id is the scatter index. Every scatter has a seeded buffer entry and a
## seeded pixel, so many entries and pixels get several values and some get none.
##
## Paths:
## - buffer: max and min into two storage buffers (SUPPORTS_BUFFER_INT64_ATOMICS). A device without
##   the feature fails the fixture with reason NO_BUFFER_INT64_ATOMICS; the shader is not created,
##   so no error is logged.
## - image: max and min into two R64_UINT images, where the device reports
##   SUPPORTS_IMAGE_INT64_ATOMICS.
## - add: atomicAdd of each depth into one of 256 uint64 counters, where the device reports
##   SUPPORTS_INT64_ATOMIC_ARITH. The sums pass 2^32, so the carry into the high word is checked.
## The result lists every path as "pass", "fail" or "unsupported".
##
## Before creating each shader, the fixture checks that the compiled SPIR-V declares the
## capabilities RenderingDevice scans for: Int64Atomics in every shader, Int64ImageEXT in the image
## shader.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure. The SHA-256 of
## each read-back lets runs on different drivers be compared.
##
## User arguments: --count=N --entries=N --width=N --height=N --seed=N, --out=<dir> to write the
## compiled shaders there as <name>.spv, and --require=<paths> (a comma-separated list of buffer,
## image and add) to fail the run when one of those paths is "unsupported", so a gate run cannot pass
## without testing the paths it is meant to test.

const SceneArgs = preload("res://scenes/scene_args.gd")

const DEFAULT_COUNT := 1 << 20
const DEFAULT_ENTRIES := 1 << 20
const DEFAULT_WIDTH := 1920
const DEFAULT_HEIGHT := 1080
const DEFAULT_SEED := 0x41543634
const ADD_COUNTERS := 256
const GROUP_SIZE := 64
# The CPU reference packs a value into one non-negative int: depth * 2^ID_BITS + id.
const ID_BITS := 24
const MAX_COUNT := 1 << ID_BITS
const SPIRV_MAGIC := 0x07230203
const SPV_OP_CAPABILITY := 17
const SPV_CAPABILITY_INT64_ATOMICS := 12
const SPV_CAPABILITY_INT64_IMAGE_EXT := 5016
# The --require names and the result paths each one covers.
const REQUIRE_PATHS := { "buffer": ["buffer_max", "buffer_min"], "image": ["image_max", "image_min"], "add": ["add"] }

const BUFFER_SOURCE := """#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Scatter {
	uint entry;
	uint pixel;
	uint id;
	uint depth;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Scatters {
	Scatter data[];
}
scatters;

layout(set = 0, binding = 1, std430) restrict buffer MaxValues {
	uint64_t data[];
}
max_values;

layout(set = 0, binding = 2, std430) restrict buffer MinValues {
	uint64_t data[];
}
min_values;

layout(push_constant, std430) uniform Params {
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	Scatter s = scatters.data[i];
	uint64_t value = (uint64_t(s.depth) << 32) | uint64_t(s.id);
	atomicMax(max_values.data[s.entry], value);
	atomicMin(min_values.data[s.entry], value);
}
"""

const IMAGE_SOURCE := """#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_image_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Scatter {
	uint entry;
	uint pixel;
	uint id;
	uint depth;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Scatters {
	Scatter data[];
}
scatters;

layout(set = 0, binding = 1, r64ui) uniform restrict u64image2D max_image;
layout(set = 0, binding = 2, r64ui) uniform restrict u64image2D min_image;

layout(push_constant, std430) uniform Params {
	uint count;
	uint width;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	Scatter s = scatters.data[i];
	uint64_t value = (uint64_t(s.depth) << 32) | uint64_t(s.id);
	ivec2 coord = ivec2(int(s.pixel % params.width), int(s.pixel / params.width));
	imageAtomicMax(max_image, coord, value);
	imageAtomicMin(min_image, coord, value);
}
"""

const ADD_SOURCE := """#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Scatter {
	uint entry;
	uint pixel;
	uint id;
	uint depth;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Scatters {
	Scatter data[];
}
scatters;

layout(set = 0, binding = 1, std430) restrict buffer Sums {
	uint64_t data[];
}
sums;

layout(push_constant, std430) uniform Params {
	uint count;
	uint counters;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.count) {
		return;
	}
	atomicAdd(sums.data[i % params.counters], uint64_t(scatters.data[i].depth));
}
"""

var _failures := PackedStringArray()


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var params := {
		"count": clampi(SceneArgs.int_value("--count", DEFAULT_COUNT), 1, MAX_COUNT),
		"entries": maxi(1, SceneArgs.int_value("--entries", DEFAULT_ENTRIES)),
		"width": maxi(1, SceneArgs.int_value("--width", DEFAULT_WIDTH)),
		"height": maxi(1, SceneArgs.int_value("--height", DEFAULT_HEIGHT)),
		"seed": SceneArgs.int_value("--seed", DEFAULT_SEED),
		"require": _required_paths(),
	}
	var result := {
		"test": "atomics64",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": params,
	}

	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result

	var features := {
		"buffer": rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_INT64_ATOMICS),
		"image": rd.has_feature(RenderingDevice.SUPPORTS_IMAGE_INT64_ATOMICS),
		"arith": rd.has_feature(RenderingDevice.SUPPORTS_INT64_ATOMIC_ARITH),
	}
	result["adapter"] = rd.get_device_name()
	result["features"] = features
	_check(not features["image"] or features["buffer"], "SUPPORTS_IMAGE_INT64_ATOMICS comes with SUPPORTS_BUFFER_INT64_ATOMICS")

	var paths := { "buffer_max": "unsupported", "buffer_min": "unsupported", "image_max": "unsupported", "image_min": "unsupported", "add": "unsupported" }
	var digests := {}
	var mismatches := {}
	if not features["buffer"]:
		result["reason"] = "NO_BUFFER_INT64_ATOMICS"
		_check(false, "the device reports SUPPORTS_BUFFER_INT64_ATOMICS")
	else:
		var out_dir := SceneArgs.value("--out", "")
		var input := _make_input(params)
		var reference := _make_reference(params, input)
		result["coverage"] = reference["coverage"]
		var scatters := rd.storage_buffer_create(input["bytes"].size(), input["bytes"])
		_check(scatters.is_valid(), "the scatter buffer is created")
		if scatters.is_valid():
			_run_buffer(rd, params, scatters, reference, out_dir, paths, digests, mismatches)
			if features["image"]:
				_run_image(rd, params, scatters, reference, out_dir, paths, digests, mismatches)
			if features["arith"]:
				_run_add(rd, params, scatters, reference, out_dir, paths, digests, mismatches)
			rd.free_rid(scatters)
	rd.free()

	for required in params["require"]:
		if not REQUIRE_PATHS.has(required):
			_check(false, "--require names an unknown path '%s' (known: buffer, image, add)" % required)
			continue
		for path in REQUIRE_PATHS[required]:
			_check(paths[path] != "unsupported", "the required path %s ran (--require=%s)" % [path, required])

	result["paths"] = paths
	result["digests"] = digests
	result["mismatches"] = mismatches
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


# The --require list, without empty entries.
static func _required_paths() -> PackedStringArray:
	var required := PackedStringArray()
	for item in SceneArgs.value("--require", "").split(","):
		if not item.strip_edges().is_empty():
			required.append(item.strip_edges())
	return required


# The scatters: { entry, pixel, id, depth } as four uint32 each, plus the depths as ints for the
# reference.
func _make_input(params: Dictionary) -> Dictionary:
	var count: int = params["count"]
	var entries: int = params["entries"]
	var pixels: int = params["width"] * params["height"]
	var rng := RandomNumberGenerator.new()
	rng.seed = params["seed"]
	var words := PackedInt32Array()
	words.resize(count * 4)
	var entry_list := PackedInt32Array()
	entry_list.resize(count)
	var pixel_list := PackedInt32Array()
	pixel_list.resize(count)
	var depth_list := PackedInt64Array()
	depth_list.resize(count)
	for i in count:
		var entry := rng.randi() % entries
		var pixel := rng.randi() % pixels
		var depth := rng.randi()
		words[i * 4] = entry
		words[i * 4 + 1] = pixel
		words[i * 4 + 2] = i
		# A PackedInt32Array keeps the low 32 bits, so the depth's bit pattern is stored unchanged.
		words[i * 4 + 3] = depth
		entry_list[i] = entry
		pixel_list[i] = pixel
		depth_list[i] = depth
	return { "bytes": words.to_byte_array(), "entries": entry_list, "pixels": pixel_list, "depths": depth_list }


# The expected contents of every output, as the bytes the GPU writes.
func _make_reference(params: Dictionary, input: Dictionary) -> Dictionary:
	var count: int = params["count"]
	var entries: int = params["entries"]
	var pixels: int = params["width"] * params["height"]
	var entry_list: PackedInt32Array = input["entries"]
	var pixel_list: PackedInt32Array = input["pixels"]
	var depth_list: PackedInt64Array = input["depths"]
	# Keys depth * 2^ID_BITS + id stay below 2^56, so the signed comparison is the unsigned one of
	# the 64-bit values. -1 marks an entry no scatter reached.
	var buffer_max := PackedInt64Array()
	buffer_max.resize(entries)
	buffer_max.fill(-1)
	var buffer_min := buffer_max.duplicate()
	var image_max := PackedInt64Array()
	image_max.resize(pixels)
	image_max.fill(-1)
	var image_min := image_max.duplicate()
	var sums := PackedInt64Array()
	sums.resize(ADD_COUNTERS)
	for i in count:
		var depth := depth_list[i]
		var key := (depth << ID_BITS) | i
		var entry := entry_list[i]
		var pixel := pixel_list[i]
		if key > buffer_max[entry]:
			buffer_max[entry] = key
		if buffer_min[entry] < 0 or key < buffer_min[entry]:
			buffer_min[entry] = key
		if key > image_max[pixel]:
			image_max[pixel] = key
		if image_min[pixel] < 0 or key < image_min[pixel]:
			image_min[pixel] = key
		sums[i % ADD_COUNTERS] += depth
	# Untouched entries keep their initial value: 0 for max, all ones for min.
	return {
		"coverage": { "buffer": _coverage(buffer_max, buffer_min), "image": _coverage(image_max, image_min) },
		"buffer_max": _to_gpu_bytes(buffer_max, 0),
		"buffer_min": _to_gpu_bytes(buffer_min, -1),
		"image_max": _to_gpu_bytes(image_max, 0),
		"image_min": _to_gpu_bytes(image_min, -1),
		"add": sums.to_byte_array(),
	}


# How many entries no scatter reached, and how many got two or more values (ids are unique, so their
# max and min differ), which is where the atomics decide the result.
func _coverage(max_keys: PackedInt64Array, min_keys: PackedInt64Array) -> Dictionary:
	var untouched := 0
	var contended := 0
	for i in max_keys.size():
		if max_keys[i] < 0:
			untouched += 1
		elif max_keys[i] != min_keys[i]:
			contended += 1
	return { "entries": max_keys.size(), "untouched": untouched, "contended": contended }


# Each key becomes its uint64 value, id in the low word and depth in the high word; -1 becomes
# p_untouched in both words.
func _to_gpu_bytes(keys: PackedInt64Array, p_untouched: int) -> PackedByteArray:
	var words := PackedInt32Array()
	words.resize(keys.size() * 2)
	var id_mask := (1 << ID_BITS) - 1
	for i in keys.size():
		var key := keys[i]
		if key < 0:
			words[i * 2] = p_untouched
			words[i * 2 + 1] = p_untouched
		else:
			words[i * 2] = key & id_mask
			words[i * 2 + 1] = key >> ID_BITS
	return words.to_byte_array()


func _run_buffer(rd: RenderingDevice, params: Dictionary, scatters: RID, reference: Dictionary, out_dir: String, paths: Dictionary, digests: Dictionary, mismatches: Dictionary) -> void:
	var shader := _create_shader(rd, BUFFER_SOURCE, "atomics64_buffer", false, out_dir)
	if not shader.is_valid():
		paths["buffer_max"] = "fail"
		paths["buffer_min"] = "fail"
		return
	var size: int = params["entries"] * 8
	# Both buffers get explicit contents: a buffer created without data is not cleared, and the
	# reference starts max at 0 and min at all ones.
	var zeros := PackedByteArray()
	zeros.resize(size)
	zeros.fill(0)
	var ones := PackedByteArray()
	ones.resize(size)
	ones.fill(255)
	var max_values := rd.storage_buffer_create(size, zeros)
	var min_values := rd.storage_buffer_create(size, ones)
	var uniform_set := rd.uniform_set_create([_buffer_uniform(0, scatters), _buffer_uniform(1, max_values), _buffer_uniform(2, min_values)], shader, 0)
	_check(uniform_set.is_valid(), "the buffer uniform set is created")
	if uniform_set.is_valid():
		_dispatch(rd, shader, uniform_set, PackedInt32Array([params["count"], 0, 0, 0]), params["count"])
		paths["buffer_max"] = _compare("buffer_max", rd.buffer_get_data(max_values), reference, 8, digests, mismatches)
		paths["buffer_min"] = _compare("buffer_min", rd.buffer_get_data(min_values), reference, 8, digests, mismatches)
	else:
		paths["buffer_max"] = "fail"
		paths["buffer_min"] = "fail"
	# Freeing the shader also frees the uniform set made from it.
	rd.free_rid(shader)
	rd.free_rid(max_values)
	rd.free_rid(min_values)


func _run_image(rd: RenderingDevice, params: Dictionary, scatters: RID, reference: Dictionary, out_dir: String, paths: Dictionary, digests: Dictionary, mismatches: Dictionary) -> void:
	var shader := _create_shader(rd, IMAGE_SOURCE, "atomics64_image", true, out_dir)
	if not shader.is_valid():
		paths["image_max"] = "fail"
		paths["image_min"] = "fail"
		return
	var format := RDTextureFormat.new()
	format.format = RenderingDevice.DATA_FORMAT_R64_UINT
	format.width = params["width"]
	format.height = params["height"]
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_STORAGE_ATOMIC_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	_check(rd.texture_is_format_supported_for_usage(format.format, format.usage_bits), "R64_UINT supports storage, atomic, update and copy usage")
	var size: int = params["width"] * params["height"] * 8
	var zeros := PackedByteArray()
	zeros.resize(size)
	zeros.fill(0)
	var ones := PackedByteArray()
	ones.resize(size)
	ones.fill(255)
	var max_image := rd.texture_create(format, RDTextureView.new(), [zeros])
	var min_image := rd.texture_create(format, RDTextureView.new(), [ones])
	_check(max_image.is_valid() and min_image.is_valid(), "the R64_UINT images are created")
	var uniform_set := RID()
	if max_image.is_valid() and min_image.is_valid():
		uniform_set = rd.uniform_set_create([_buffer_uniform(0, scatters), _image_uniform(1, max_image), _image_uniform(2, min_image)], shader, 0)
		_check(uniform_set.is_valid(), "the image uniform set is created")
	if uniform_set.is_valid():
		_dispatch(rd, shader, uniform_set, PackedInt32Array([params["count"], params["width"], 0, 0]), params["count"])
		paths["image_max"] = _compare("image_max", rd.texture_get_data(max_image, 0), reference, 8, digests, mismatches)
		paths["image_min"] = _compare("image_min", rd.texture_get_data(min_image, 0), reference, 8, digests, mismatches)
	else:
		paths["image_max"] = "fail"
		paths["image_min"] = "fail"
	rd.free_rid(shader)
	if max_image.is_valid():
		rd.free_rid(max_image)
	if min_image.is_valid():
		rd.free_rid(min_image)


func _run_add(rd: RenderingDevice, params: Dictionary, scatters: RID, reference: Dictionary, out_dir: String, paths: Dictionary, digests: Dictionary, mismatches: Dictionary) -> void:
	var shader := _create_shader(rd, ADD_SOURCE, "atomics64_add", false, out_dir)
	if not shader.is_valid():
		paths["add"] = "fail"
		return
	# The sums start at 0: a buffer created without data is not cleared.
	var zeros := PackedByteArray()
	zeros.resize(ADD_COUNTERS * 8)
	zeros.fill(0)
	var sums := rd.storage_buffer_create(zeros.size(), zeros)
	var uniform_set := rd.uniform_set_create([_buffer_uniform(0, scatters), _buffer_uniform(1, sums)], shader, 0)
	_check(uniform_set.is_valid(), "the add uniform set is created")
	if uniform_set.is_valid():
		_dispatch(rd, shader, uniform_set, PackedInt32Array([params["count"], ADD_COUNTERS, 0, 0]), params["count"])
		paths["add"] = _compare("add", rd.buffer_get_data(sums), reference, 8, digests, mismatches)
	else:
		paths["add"] = "fail"
	rd.free_rid(shader)
	rd.free_rid(sums)


func _create_shader(rd: RenderingDevice, source_text: String, shader_name: String, image: bool, out_dir: String) -> RID:
	var source := RDShaderSource.new()
	source.source_compute = source_text
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "%s compiles: %s" % [shader_name, spirv.compile_error_compute])
	if not spirv.compile_error_compute.is_empty():
		return RID()
	var bytecode := spirv.bytecode_compute
	var capabilities := _spirv_capabilities(bytecode)
	_check(capabilities.has(SPV_CAPABILITY_INT64_ATOMICS), "%s declares Int64Atomics" % shader_name)
	_check(capabilities.has(SPV_CAPABILITY_INT64_IMAGE_EXT) == image, "%s %s Int64ImageEXT" % [shader_name, "declares" if image else "does not declare"])
	if not out_dir.is_empty():
		DirAccess.make_dir_recursive_absolute(out_dir)
		var file := FileAccess.open(out_dir.path_join(shader_name + ".spv"), FileAccess.WRITE)
		if file != null:
			file.store_buffer(bytecode)
	var shader := rd.shader_create_from_spirv(spirv, shader_name)
	_check(shader.is_valid(), "%s is created" % shader_name)
	return shader


# The capabilities a SPIR-V module declares, which come first after its header.
static func _spirv_capabilities(bytecode: PackedByteArray) -> PackedInt32Array:
	var capabilities := PackedInt32Array()
	if bytecode.size() < 20 or bytecode.decode_u32(0) != SPIRV_MAGIC:
		return capabilities
	var offset := 20
	while offset + 8 <= bytecode.size():
		var word := bytecode.decode_u32(offset)
		var length := word >> 16
		if word & 0xffff != SPV_OP_CAPABILITY or length < 2:
			break
		capabilities.append(bytecode.decode_u32(offset + 4))
		offset += length * 4
	return capabilities


func _dispatch(rd: RenderingDevice, shader: RID, uniform_set: RID, push_constants: PackedInt32Array, count: int) -> void:
	var pipeline := rd.compute_pipeline_create(shader)
	_check(pipeline.is_valid(), "the compute pipeline is created")
	if not pipeline.is_valid():
		return
	var push := push_constants.to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, ceili(count / float(GROUP_SIZE)), 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()
	rd.free_rid(pipeline)


func _compare(label: String, data: PackedByteArray, reference: Dictionary, element_size: int, digests: Dictionary, mismatches: Dictionary) -> String:
	var expected: PackedByteArray = reference[label]
	digests[label] = _digest(data)
	if data == expected:
		mismatches[label] = 0
		return "pass"
	if data.size() != expected.size():
		_check(false, "%s read back %d bytes, expected %d" % [label, data.size(), expected.size()])
		mismatches[label] = -1
		return "fail"
	var different := 0
	var first := -1
	for i in range(0, data.size(), element_size):
		if data.slice(i, i + element_size) != expected.slice(i, i + element_size):
			different += 1
			if first < 0:
				first = i / element_size
	mismatches[label] = different
	_check(false, "%s: %d of %d values differ from the CPU reference, first at index %d" % [label, different, data.size() / element_size, first])
	return "fail"


func _buffer_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


func _image_uniform(binding: int, texture: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	uniform.binding = binding
	uniform.add_id(texture)
	return uniform


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
