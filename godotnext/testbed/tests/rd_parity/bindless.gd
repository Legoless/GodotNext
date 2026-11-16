extends Node

## Bindless tables fixture (#176 step 8; plan §21.1 C4, GR-2).
##
## Runs on the main RenderingDevice across real frames, so the frame ring is the engine's
## (get_frame_delay(), at least 2), nothing waits for the GPU between frames, and every image comes back
## through buffer_get_data_async() once its frame has finished. A slot written again while an earlier
## frame can still read it, or a resource freed while one can, then shows as a wrong pixel or a validation
## error.
##
## Puts --textures distinct 4x4 RGBA8 textures into a texture table of --slots slots (slot s holds texture
## s % --textures), two samplers (nearest, repeat and clamp) into a sampler table, and --buffers storage
## buffers of 16 words into a storage-buffer table. A compute shader reads all three tables at a different,
## non-uniform index in every pixel of a --width x --height image: a texel from the texture slot through
## the sampler slot, at a texel center that is moved by -1, 0 or +1 texture widths so the two samplers
## differ, XOR one word of the buffer slot. The result is compared word for word with a CPU reference.
##
## Then --churn-frames frames of churn. Each frame first records a --churn-width x --churn-height image that
## reads allocated slots, then releases --churn texture slots (the image just recorded reads some of them),
## frees some backing textures while slots may still hold them (the free waits for their last slot), and
## allocates --churn slots with live textures. A slot handed out again less than get_frame_delay() frames
## after its release fails the run, and so does an image that differs from the CPU reference, a readback
## that never arrives, or one that arrives in the frame that recorded it (the device waited). GR-2's
## 10,000-frame soak in PQ is this run with --churn-frames=10000.
##
## Then the deferred frees the churn does not reach (a shared texture's owner, a storage buffer and a
## sampler), and, once the tables are freed, no texture freed while a slot held it may still be valid.
##
## The texture table's memory must be at most 4 MiB at 65,536 slots (plan §21.1.4). The value is
## bindless_table_get_memory_usage(), which the driver computes from the slot count and its entry size; it
## is not a measured allocation.
##
## It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure. A device without
## SUPPORTS_BINDLESS fails with reason NO_BINDLESS.
##
## User arguments: --textures=N --slots=N --buffers=N --width=N --height=N --seed=N --churn-frames=N
## --churn=N --churn-width=N --churn-height=N, --out=<dir> to write the compiled shader there as
## bindless.spv, and --tables=all|textures|fetch: "textures" leaves out the storage-buffer table (the
## shader then reads only the texture and sampler tables, and every word is 0); "fetch" also leaves out
## the sampler table and reads the texel with texelFetch (no offset, no sampler). Both narrow down a
## driver problem with one kind of table.
##
## --table-limit=1 also asks for one more texture table and one more sampler table while the run's three
## tables exist, and reports in "table_limit" whether each was created. A driver may refuse them (Direct3D 12
## holds two texture or storage-buffer tables and one sampler table at a time) with errors, which are then
## the run's only errors. The run must still pass: a refusal leaves the uniform sets the run creates
## afterwards, and its images, intact.

const SceneArgs = preload("res://scenes/scene_args.gd")

const DEFAULT_TEXTURES := 4096
const DEFAULT_SLOTS := 65536
const DEFAULT_BUFFERS := 256
const DEFAULT_WIDTH := 512
const DEFAULT_HEIGHT := 512
const DEFAULT_SEED := 0x42494e44
const DEFAULT_CHURN_FRAMES := 16
const DEFAULT_CHURN := 512
const DEFAULT_CHURN_WIDTH := 64
const DEFAULT_CHURN_HEIGHT := 64
const TEXTURE_SIZE := 4
const BUFFER_WORDS := 16
const GROUP_SIZE := 8
const MAX_TABLE_MEMORY := 4 * 1024 * 1024
const MEMORY_LINE_SLOTS := 65536
const MASK_32 := 0xffffffff

const SOURCE := """#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
%s

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Probe {
	uint texture_slot;
	uint lookup; // Texel x (2 bits), y (2 bits), offset x + 1 (2 bits), offset y + 1 (2 bits), sampler slot (1 bit).
	uint buffer_slot;
	uint element;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Probes {
	Probe data[];
}
probes;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Results {
	uint data[];
}
results;

layout(set = 1, binding = 0) uniform texture2D textures[];
#ifdef SAMPLER_TABLE
layout(set = 2, binding = 0) uniform sampler samplers[];
#endif
#ifdef BUFFER_TABLE
layout(set = 3, binding = 0, std430) restrict readonly buffer Words {
	uint data[];
}
words[];
#endif

layout(push_constant, std430) uniform Params {
	uint width;
	uint height;
	uint pad0;
	uint pad1;
}
params;

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= params.width || pixel.y >= params.height) {
		return;
	}
	uint index = pixel.y * params.width + pixel.x;
	Probe probe = probes.data[index];
	vec2 texel = vec2(float(probe.lookup & 3u), float((probe.lookup >> 2u) & 3u));
	vec2 offset = vec2(float((probe.lookup >> 4u) & 3u), float((probe.lookup >> 6u) & 3u)) - 1.0;
	uint sampler_slot = (probe.lookup >> 8u) & 1u;
	vec2 uv = (texel + 0.5) / 4.0 + offset;
#ifdef SAMPLER_TABLE
	vec4 color = textureLod(sampler2D(textures[nonuniformEXT(probe.texture_slot)], samplers[nonuniformEXT(sampler_slot)]), uv, 0.0);
#else
	vec4 color = texelFetch(textures[nonuniformEXT(probe.texture_slot)], ivec2(texel), 0);
#endif
#ifdef BUFFER_TABLE
	uint word = words[nonuniformEXT(probe.buffer_slot)].data[probe.element];
#else
	uint word = 0u;
#endif
	results.data[index] = packUnorm4x8(color) ^ word;
}
"""

var _failures := PackedStringArray()
var _rd: RenderingDevice
# Images whose readback has not arrived: key -> { label, pixels, expected, buffers, frame }.
var _readbacks := {}
var _next_readback := 0
# Buffers of arrived images, freed at the next frame (not inside the device's callback).
var _to_free: Array[RID] = []
var _full := {}
var _churn_compared := 0
var _churn_mismatches := 0
var _min_latency := -1
var _max_latency := -1


func _ready() -> void:
	var result: Dictionary = await _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var params := {
		"textures": maxi(1, SceneArgs.int_value("--textures", DEFAULT_TEXTURES)),
		"slots": maxi(1, SceneArgs.int_value("--slots", DEFAULT_SLOTS)),
		"buffers": maxi(1, SceneArgs.int_value("--buffers", DEFAULT_BUFFERS)),
		"width": maxi(1, SceneArgs.int_value("--width", DEFAULT_WIDTH)),
		"height": maxi(1, SceneArgs.int_value("--height", DEFAULT_HEIGHT)),
		"seed": SceneArgs.int_value("--seed", DEFAULT_SEED),
		"churn_frames": maxi(0, SceneArgs.int_value("--churn-frames", DEFAULT_CHURN_FRAMES)),
		"churn": maxi(1, SceneArgs.int_value("--churn", DEFAULT_CHURN)),
		"churn_width": maxi(1, SceneArgs.int_value("--churn-width", DEFAULT_CHURN_WIDTH)),
		"churn_height": maxi(1, SceneArgs.int_value("--churn-height", DEFAULT_CHURN_HEIGHT)),
		"buffer_table": SceneArgs.value("--tables", "all") == "all",
		"sampler_table": SceneArgs.value("--tables", "all") != "fetch",
		"table_limit": SceneArgs.int_value("--table-limit", 0) != 0,
	}
	var result := {
		"test": "bindless",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": params,
	}

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	_rd = rd

	result["adapter"] = rd.get_device_name()
	result["frame_delay"] = rd.get_frame_delay()
	_check(rd.get_frame_delay() >= 2, "the main device's frame ring has at least 2 frames (%d)" % rd.get_frame_delay())
	result["supported"] = rd.has_feature(RenderingDevice.SUPPORTS_BINDLESS)
	result["limits"] = {
		"textures": rd.limit_get(RenderingDevice.LIMIT_MAX_BINDLESS_TEXTURES),
		"samplers": rd.limit_get(RenderingDevice.LIMIT_MAX_BINDLESS_SAMPLERS),
		"storage_buffers": rd.limit_get(RenderingDevice.LIMIT_MAX_BINDLESS_STORAGE_BUFFERS),
	}
	if not result["supported"]:
		result["reason"] = "NO_BINDLESS"
		_check(false, "the device reports SUPPORTS_BINDLESS")
	else:
		_check(result["limits"]["textures"] >= 65536 and result["limits"]["storage_buffers"] >= 65536, "texture and storage-buffer tables offer at least 65,536 slots")
		var churn_headroom: int = params["churn"] * (rd.get_frame_delay() + 1)
		_check(params["slots"] > churn_headroom, "--slots leaves room for the churn")
		if params["slots"] > churn_headroom:
			await _run_tables(rd, params, result)

	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


func _run_tables(rd: RenderingDevice, params: Dictionary, result: Dictionary) -> void:
	var shader := _create_shader(rd, SceneArgs.value("--out", ""), params["buffer_table"], params["sampler_table"])
	if not shader.is_valid():
		return
	var pipeline := rd.compute_pipeline_create(shader)
	_check(pipeline.is_valid(), "the compute pipeline is created")

	var rng := RandomNumberGenerator.new()
	rng.seed = params["seed"]

	# The tables.
	var texture_table := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_TEXTURE, params["slots"])
	var sampler_table := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_SAMPLER, 4)
	var buffer_table := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER, params["slots"])
	_check(texture_table.is_valid() and sampler_table.is_valid() and buffer_table.is_valid(), "the three tables are created")
	if not (texture_table.is_valid() and sampler_table.is_valid() and buffer_table.is_valid()):
		rd.free_rid(shader)
		return
	var uses_buffer_table: bool = params["buffer_table"]
	if params["table_limit"]:
		result["table_limit"] = _check_table_limit(rd)

	var memory: int = rd.bindless_table_get_memory_usage(texture_table)
	result["texture_table_memory"] = memory
	if params["slots"] == MEMORY_LINE_SLOTS:
		_check(memory > 0 and memory <= MAX_TABLE_MEMORY, "the 65,536-slot texture table takes at most 4 MiB (%d bytes)" % memory)
	result["storage_buffer_table_memory"] = rd.bindless_table_get_memory_usage(buffer_table)

	# Samplers: slot 0 repeats, slot 1 clamps to the edge. Both are nearest.
	var samplers := []
	for repeat_mode in [RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT, RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE]:
		var state := RDSamplerState.new()
		state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
		state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
		state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
		state.repeat_u = repeat_mode
		state.repeat_v = repeat_mode
		var sampler := rd.sampler_create(state)
		samplers.append(sampler)
		_check(rd.bindless_table_allocate(sampler_table, sampler) == samplers.size() - 1, "sampler %d gets slot %d" % [samplers.size() - 1, samplers.size() - 1])

	# Storage buffers.
	var buffers := []
	for b in params["buffers"]:
		var buffer_words := PackedInt32Array()
		buffer_words.resize(BUFFER_WORDS)
		for e in BUFFER_WORDS:
			buffer_words[e] = _hash3(0x4255, b, e)
		var buffer := rd.storage_buffer_create(BUFFER_WORDS * 4, buffer_words.to_byte_array())
		buffers.append(buffer)
		_check(rd.bindless_table_allocate(buffer_table, buffer) == b, "buffer %d gets slot %d" % [b, b])

	# Textures, then the texture slots. Slot s holds texture s % textures; the table keeps room for the
	# churn's allocations.
	var live_textures := []
	var texture_keys := {} # Texture RID -> the key its texels come from.
	var next_key := 0
	for t in params["textures"]:
		var texture := _create_texture(rd, next_key)
		live_textures.append(texture)
		texture_keys[texture] = next_key
		next_key += 1
	var slot_keys := PackedInt64Array() # The key of the texture each slot holds; -1 for a free slot.
	slot_keys.resize(params["slots"])
	slot_keys.fill(-1)
	var allocated_slots := PackedInt32Array()
	var initial_slots: int = params["slots"] - params["churn"] * (rd.get_frame_delay() + 1)
	var wrong_slots := 0
	for s in initial_slots:
		var texture: RID = live_textures[s % live_textures.size()]
		var slot := rd.bindless_table_allocate(texture_table, texture)
		if slot != s:
			wrong_slots += 1
		if slot >= 0:
			slot_keys[slot] = texture_keys[texture]
			allocated_slots.append(slot)
	_check(wrong_slots == 0, "the first %d texture slots are handed out in order (%d were not)" % [initial_slots, wrong_slots])
	result["textures_resident"] = live_textures.size()
	result["texture_slots_allocated"] = allocated_slots.size()

	var uses_sampler_table: bool = params["sampler_table"]
	var sets := [_table_set(rd, shader, 1, texture_table)]
	if uses_sampler_table:
		sets.append(_table_set(rd, shader, 2, sampler_table))
	if uses_buffer_table:
		sets.append(_table_set(rd, shader, 3, buffer_table))
	for uniform_set in sets:
		_check(uniform_set.is_valid(), "the table uniform sets are created")

	# The full-size check. Its result arrives frames later, with the churn's.
	_record_frame(rd, pipeline, shader, sets, params["width"], params["height"], allocated_slots, slot_keys, params["buffers"], uses_buffer_table, uses_sampler_table, rng, "full")

	# The churn: one iteration per engine frame.
	var churn_frames: int = params["churn_frames"]
	var churn: int = params["churn"]
	var frame_delay := rd.get_frame_delay()
	var released_at := PackedInt64Array() # The engine frame each slot was released in; -1 if never.
	released_at.resize(params["slots"])
	released_at.fill(-1)
	var early_reuses := 0
	var failed_allocations := 0
	var deferred_frees := 0
	var uneven_frames := 0
	var freed_textures := [] # Freed by the fixture, still alive while slots hold them.
	var last_frame := Engine.get_frames_drawn()
	for f in churn_frames:
		await get_tree().process_frame
		_free_arrived()
		var now := Engine.get_frames_drawn()
		if now != last_frame + 1:
			uneven_frames += 1
		last_frame = now
		# An image that reads allocated slots, recorded before this frame's releases.
		_record_frame(rd, pipeline, shader, sets, params["churn_width"], params["churn_height"], allocated_slots, slot_keys, params["buffers"], uses_buffer_table, uses_sampler_table, rng, "churn frame %d" % (f + 1))
		# Release random slots.
		for i in mini(churn, allocated_slots.size()):
			var index := rng.randi() % allocated_slots.size()
			var slot := allocated_slots[index]
			allocated_slots[index] = allocated_slots[allocated_slots.size() - 1]
			allocated_slots.resize(allocated_slots.size() - 1)
			rd.bindless_table_release(texture_table, slot)
			slot_keys[slot] = -1
			released_at[slot] = now
		# Free some backing textures while slots may still hold them, and replace them.
		for i in maxi(1, churn / 64):
			var index := rng.randi() % live_textures.size()
			var texture: RID = live_textures[index]
			rd.free_rid(texture)
			deferred_frees += 1
			freed_textures.append(texture)
			var replacement := _create_texture(rd, next_key)
			texture_keys[replacement] = next_key
			next_key += 1
			live_textures[index] = replacement
		# Allocate slots with live textures.
		for i in churn:
			var texture: RID = live_textures[rng.randi() % live_textures.size()]
			var slot := rd.bindless_table_allocate(texture_table, texture)
			if slot < 0:
				failed_allocations += 1
				continue
			if released_at[slot] >= 0 and now - released_at[slot] < frame_delay:
				early_reuses += 1
			slot_keys[slot] = texture_keys[texture]
			allocated_slots.append(slot)

	# Wait for the readbacks: each arrives once the frame ring has come back to its frame.
	var waited := 0
	while not _readbacks.is_empty() and waited < frame_delay + 8:
		await get_tree().process_frame
		_free_arrived()
		waited += 1
	_check(_readbacks.is_empty(), "every image's readback arrives (%d did not)" % _readbacks.size())
	_check(_churn_compared == churn_frames, "every churn frame's image is compared (%d of %d)" % [_churn_compared, churn_frames])
	_check(not _full.is_empty(), "the full-size image is compared")
	_check(_min_latency >= 1, "no readback arrives in the frame that recorded it (the device did not wait; earliest %d)" % _min_latency)
	_check(early_reuses == 0, "no slot is handed out again before %d frames have passed (%d were)" % [frame_delay, early_reuses])
	_check(failed_allocations == 0, "every churn allocation gets a slot (%d did not)" % failed_allocations)
	result["full"] = _full
	result["churn"] = {
		"frames": churn_frames,
		"compared": _churn_compared,
		"early_reuses": early_reuses,
		"failed_allocations": failed_allocations,
		"deferred_frees": deferred_frees,
		"mismatches": _churn_mismatches,
		"uneven_frames": uneven_frames,
		"readback_latency_frames": [_min_latency, _max_latency],
	}

	# Freeing the tables releases their slots, which completes the deferred frees; freeing the shader
	# frees the uniform sets made from it.
	result["deferred_frees"] = _check_deferred_frees(rd, shader, texture_table, sampler_table, buffer_table)

	# A texture freed while slots held it stays valid until its last slot is released.
	var alive_after_free := 0
	for texture in freed_textures:
		if rd.texture_is_valid(texture):
			alive_after_free += 1
	rd.free_rid(pipeline)
	rd.free_rid(shader)
	rd.free_rid(texture_table)
	rd.free_rid(sampler_table)
	rd.free_rid(buffer_table)
	# Freeing the table released the rest of their slots, which completed their frees.
	var alive_after_table_free := 0
	for texture in freed_textures:
		if rd.texture_is_valid(texture):
			alive_after_table_free += 1
	_check(alive_after_table_free == 0, "every texture freed while a slot held it is freed once the table is (%d are still valid)" % alive_after_table_free)
	result["churn"]["freed_textures_alive_before_table_free"] = alive_after_free
	result["churn"]["freed_textures_alive_after_table_free"] = alive_after_table_free
	for texture in live_textures:
		rd.free_rid(texture)
	for sampler in samplers:
		rd.free_rid(sampler)
	for buffer in buffers:
		rd.free_rid(buffer)
	_free_arrived()
	# Let the frame ring wrap, so the driver frees all of it while validation still runs.
	for i in frame_delay + 1:
		await get_tree().process_frame


# Records the shader over a p_width x p_height image whose probes read allocated slots only, and asks for
# the result. _on_readback() compares it with the CPU reference when its frame has finished.
func _record_frame(rd: RenderingDevice, pipeline: RID, shader: RID, sets: Array, p_width: int, p_height: int, allocated_slots: PackedInt32Array, slot_keys: PackedInt64Array, buffer_count: int, p_buffer_table: bool, p_sampler_table: bool, rng: RandomNumberGenerator, label: String) -> void:
	var pixels := p_width * p_height
	var probes := PackedInt32Array()
	probes.resize(pixels * 4)
	var expected := PackedInt32Array()
	expected.resize(pixels)
	for i in pixels:
		var slot := allocated_slots[rng.randi() % allocated_slots.size()]
		var lookup := rng.randi() & 0x1ff
		# Offsets use 0, 1 and 2 (-1, 0 and +1): fold the unused value 3 onto 1.
		if (lookup >> 4) & 3 == 3:
			lookup &= ~(3 << 4)
			lookup |= 1 << 4
		if (lookup >> 6) & 3 == 3:
			lookup &= ~(3 << 6)
			lookup |= 1 << 6
		var buffer_slot := rng.randi() % buffer_count
		var element := rng.randi() % BUFFER_WORDS
		probes[i * 4] = slot
		probes[i * 4 + 1] = lookup
		probes[i * 4 + 2] = buffer_slot
		probes[i * 4 + 3] = element
		var word := _hash3(0x4255, buffer_slot, element) if p_buffer_table else 0
		var texel := _expected_texel(slot_keys[slot], lookup) if p_sampler_table else _hash3(slot_keys[slot], lookup & 3, (lookup >> 2) & 3)
		expected[i] = (texel ^ word) & MASK_32

	var probe_buffer := rd.storage_buffer_create(pixels * 16, probes.to_byte_array())
	var zeros := PackedByteArray()
	zeros.resize(pixels * 4)
	zeros.fill(0)
	var result_buffer := rd.storage_buffer_create(pixels * 4, zeros)
	var io_set := rd.uniform_set_create([_buffer_uniform(0, probe_buffer), _buffer_uniform(1, result_buffer)], shader, 0)
	_check(io_set.is_valid(), "%s: the probe uniform set is created" % label)

	var push := PackedInt32Array([p_width, p_height, 0, 0]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, io_set, 0)
	for s in sets.size():
		rd.compute_list_bind_uniform_set(list, sets[s], s + 1)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, ceili(p_width / float(GROUP_SIZE)), ceili(p_height / float(GROUP_SIZE)), 1)
	rd.compute_list_end()

	var key := _next_readback
	_next_readback += 1
	_readbacks[key] = {
		"label": label,
		"pixels": pixels,
		"expected": expected.to_byte_array(),
		"buffers": [probe_buffer, result_buffer],
		"frame": Engine.get_frames_drawn(),
	}
	var error := rd.buffer_get_data_async(result_buffer, _on_readback.bind(key))
	_check(error == OK, "%s: the readback is requested (error %d)" % [label, error])


# The device calls this once the frame that recorded image p_key has finished.
func _on_readback(data: PackedByteArray, p_key: int) -> void:
	if not _readbacks.has(p_key):
		return
	var entry: Dictionary = _readbacks[p_key]
	_readbacks.erase(p_key)
	var label: String = entry["label"]
	var pixels: int = entry["pixels"]
	var expected_bytes: PackedByteArray = entry["expected"]
	var mismatches := 0
	var first := -1
	if data.size() != expected_bytes.size():
		mismatches = pixels
	elif data != expected_bytes:
		for i in pixels:
			if data.decode_u32(i * 4) != expected_bytes.decode_u32(i * 4):
				mismatches += 1
				if first < 0:
					first = i
	_check(mismatches == 0, "%s: %d of %d pixels differ from the CPU reference, first at %d" % [label, mismatches, pixels, first])

	var latency := Engine.get_frames_drawn() - int(entry["frame"])
	_min_latency = latency if _min_latency < 0 else mini(_min_latency, latency)
	_max_latency = maxi(_max_latency, latency)
	if label == "full":
		_full = { "pixels": pixels, "mismatches": mismatches, "sha256": _digest(data) }
	else:
		_churn_compared += 1
		_churn_mismatches += mismatches
	_to_free.append_array(entry["buffers"])


func _free_arrived() -> void:
	for buffer in _to_free:
		_rd.free_rid(buffer)
	_to_free.clear()


# The deferred frees the churn does not reach: a shared texture keeps the texture it shares alive, and a
# storage buffer and a sampler are freed when their slot is released. A uniform set made from the buffer
# shows when the buffer is really freed (freeing it frees the set). A sampler that never was would show as
# leaked when the device is freed.
func _check_deferred_frees(rd: RenderingDevice, shader: RID, texture_table: RID, sampler_table: RID, buffer_table: RID) -> Dictionary:
	var checks := {}

	var shared_owner := _create_texture(rd, 0x7fff0000)
	var view := rd.texture_create_shared(RDTextureView.new(), shared_owner)
	var view_slot := rd.bindless_table_allocate(texture_table, view)
	_check(view_slot >= 0, "a shared texture gets a slot")
	rd.free_rid(shared_owner)
	checks["owner_alive_while_view_held"] = rd.texture_is_valid(shared_owner)
	rd.bindless_table_release(texture_table, view_slot)
	checks["owner_alive_after_release"] = rd.texture_is_valid(shared_owner)
	checks["view_alive_after_release"] = rd.texture_is_valid(view)
	_check(checks["owner_alive_while_view_held"], "a freed texture stays valid while a slot holds a shared texture of it")
	_check(not checks["owner_alive_after_release"] and not checks["view_alive_after_release"], "the freed texture and its shared texture are freed when the slot is released")

	var words := PackedInt32Array()
	words.resize(BUFFER_WORDS)
	words.fill(0)
	var buffer := rd.storage_buffer_create(BUFFER_WORDS * 4, words.to_byte_array())
	var other := rd.storage_buffer_create(BUFFER_WORDS * 4, words.to_byte_array())
	var buffer_set := rd.uniform_set_create([_buffer_uniform(0, buffer), _buffer_uniform(1, other)], shader, 0)
	var buffer_slot := rd.bindless_table_allocate(buffer_table, buffer)
	_check(buffer_set.is_valid() and buffer_slot >= 0, "a storage buffer gets a slot")
	rd.free_rid(buffer)
	checks["buffer_alive_while_held"] = rd.uniform_set_is_valid(buffer_set)
	rd.bindless_table_release(buffer_table, buffer_slot)
	checks["buffer_alive_after_release"] = rd.uniform_set_is_valid(buffer_set)
	_check(checks["buffer_alive_while_held"] and not checks["buffer_alive_after_release"], "a freed storage buffer lives until its slot is released")
	rd.free_rid(other)

	var sampler := rd.sampler_create(RDSamplerState.new())
	var sampler_slot := rd.bindless_table_allocate(sampler_table, sampler)
	_check(sampler_slot >= 0, "a sampler gets a slot")
	rd.free_rid(sampler)
	rd.bindless_table_release(sampler_table, sampler_slot)
	return checks


# The texel a probe reads from the texture with p_key: repeat keeps the texel; clamp moves it to the
# edge when the offset is not 0.
func _expected_texel(p_key: int, p_lookup: int) -> int:
	var x := p_lookup & 3
	var y := (p_lookup >> 2) & 3
	var offset_x := ((p_lookup >> 4) & 3) - 1
	var offset_y := ((p_lookup >> 6) & 3) - 1
	if (p_lookup >> 8) & 1 == 1:
		if offset_x != 0:
			x = TEXTURE_SIZE - 1 if offset_x > 0 else 0
		if offset_y != 0:
			y = TEXTURE_SIZE - 1 if offset_y > 0 else 0
	return _hash3(p_key, x, y)


func _create_texture(rd: RenderingDevice, p_key: int) -> RID:
	var texels := PackedInt32Array()
	texels.resize(TEXTURE_SIZE * TEXTURE_SIZE)
	for y in TEXTURE_SIZE:
		for x in TEXTURE_SIZE:
			texels[y * TEXTURE_SIZE + x] = _hash3(p_key, x, y)
	var format := RDTextureFormat.new()
	format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	format.width = TEXTURE_SIZE
	format.height = TEXTURE_SIZE
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
	var texture := rd.texture_create(format, RDTextureView.new(), [texels.to_byte_array()])
	_check(texture.is_valid(), "texture %d is created" % p_key)
	return texture


# --table-limit: one more texture table and one more sampler table beside the run's three. Each is either
# created (and freed again) or refused with an invalid RID; the rest of the run checks what a refusal leaves.
func _check_table_limit(rd: RenderingDevice) -> Dictionary:
	var extra_textures := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_TEXTURE, 16)
	var extra_samplers := rd.bindless_table_create(RenderingDevice.UNIFORM_TYPE_SAMPLER, 4)
	var created := {
		"extra_texture_table": extra_textures.is_valid(),
		"extra_sampler_table": extra_samplers.is_valid(),
	}
	for table: RID in [extra_textures, extra_samplers]:
		if table.is_valid():
			rd.free_rid(table)
	return created


func _table_set(rd: RenderingDevice, shader: RID, set_index: int, table: RID) -> RID:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_BINDLESS_TABLE
	uniform.binding = 0
	uniform.add_id(table)
	return rd.uniform_set_create([uniform], shader, set_index)


func _create_shader(rd: RenderingDevice, out_dir: String, p_buffer_table: bool, p_sampler_table: bool) -> RID:
	var source := RDShaderSource.new()
	var defines := ("#define BUFFER_TABLE\n" if p_buffer_table else "") + ("#define SAMPLER_TABLE\n" if p_sampler_table else "")
	source.source_compute = SOURCE % defines
	var spirv := rd.shader_compile_spirv_from_source(source)
	_check(spirv.compile_error_compute.is_empty(), "the shader compiles: %s" % spirv.compile_error_compute)
	if not spirv.compile_error_compute.is_empty():
		return RID()
	if not out_dir.is_empty():
		DirAccess.make_dir_recursive_absolute(out_dir)
		var file := FileAccess.open(out_dir.path_join("bindless.spv"), FileAccess.WRITE)
		if file != null:
			file.store_buffer(spirv.bytecode_compute)
	var shader := rd.shader_create_from_spirv(spirv, "bindless")
	_check(shader.is_valid(), "the shader is created")
	return shader


func _buffer_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


# A 32-bit hash of three numbers (a multiply-xorshift mix), the same on every platform.
static func _hash3(p_a: int, p_b: int, p_c: int) -> int:
	var h := (p_a * 0x9e3779b1 + p_b * 0x85ebca77 + p_c * 0xc2b2ae3d) & MASK_32
	h = ((h ^ (h >> 15)) * 0x2c1b3c6d) & MASK_32
	h = ((h ^ (h >> 12)) * 0x297a2d39) & MASK_32
	return (h ^ (h >> 15)) & MASK_32


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
