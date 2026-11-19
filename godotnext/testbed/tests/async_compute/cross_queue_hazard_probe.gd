extends SceneTree

## Cross-queue positive control for Vulkan synchronization validation (#193).
##
## A run that reports 0 synchronization hazards only counts if the validation layer can see a
## hazard between the main and the async compute queue. This probe makes one on purpose: two RD
## textures alias one VkImage (texture_create_from_extension), so the render graph tracks them
## separately and sees no dependency between them. In each of FRAMES frames an async compute list
## writes texture A with a storage-image dispatch, and the main queue clears texture B. The async
## list has no main-queue producer, so the frame forks before the clear and joins after it: the
## clear runs inside the fork-join window, unsynchronized with the async write of the same image.
##
## Expected with the Khronos validation layer, synchronization validation on and submit-time
## (full) validation on, which is the default from layer 1.4.x on: "vkQueueSubmit(): ...
## WRITE_RACING_WRITE hazard detected" errors that name two different VkQueues. Count them with
## the "hazard detected" text: from layer 1.4.x the messages no longer contain "SYNC-HAZARD".
## No such error means that submit-time synchronization validation is off, and a 0-hazard run
## beside it proves nothing about cross-queue hazards. With async compute off, both commands run
## on the main queue and the layer reports one "vkCmdClearColorImage(): WRITE_AFTER_WRITE hazard
## detected" at record time instead (and the probe exits 1, since nothing forked).
##
## Vulkan only (it aliases a VkImage). Needs rendering/rendering_device/async_compute on and a
## device with SUPPORTS_ASYNC_COMPUTE; with --local it uses a local RenderingDevice (submit() and
## sync() per frame), otherwise the main device, recorded on the render thread.
##
## Run (synchronization validation on, for example VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1):
##   godot --path godotnext/testbed --rendering-driver vulkan --gpu-validation \
##     -s res://tests/async_compute/cross_queue_hazard_probe.gd [-- --local]
## with override.cfg [rendering] rendering_device/async_compute=true.
##
## Prints one "GODOTNEXT_RESULT <json>" line. Exits 0 when the probe frames forked to the async
## queue and none fell back (the hazard was submitted), else 1. The hazard itself is judged from
## the log.

const FRAMES := 6
const SIZE := 64
const SHADER := """
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba8, set = 0, binding = 0) uniform restrict writeonly image2D target_image;
void main() {
	imageStore(target_image, ivec2(gl_GlobalInvocationID.xy), vec4(1.0, 0.0, 0.0, 1.0));
}
"""

var _rd: RenderingDevice
var _local := false
var _shader: RID
var _pipeline: RID
var _uniform_set: RID
var _tex_a: RID
var _tex_b: RID
var _frame := 0
var _before := Vector2i(-1, -1)
var _failures := PackedStringArray()


func _initialize() -> void:
	_local = OS.get_cmdline_user_args().has("--local")
	_rd = RenderingServer.create_local_rendering_device() if _local else RenderingServer.get_rendering_device()
	if _rd == null:
		_failures.append("no RenderingDevice")
		_finish()
		return
	if _local:
		_setup()
		_before = _counters()
		for _i in FRAMES:
			_record()
			_rd.submit()
			_rd.sync()
		_finish()
	else:
		RenderingServer.call_on_render_thread(_setup)


func _setup() -> void:
	if not _rd.has_feature(RenderingDevice.SUPPORTS_ASYNC_COMPUTE):
		_failures.append("the device has no SUPPORTS_ASYNC_COMPUTE")
		return
	var source := RDShaderSource.new()
	source.source_compute = SHADER
	var spirv := _rd.shader_compile_spirv_from_source(source)
	if spirv == null or not spirv.compile_error_compute.is_empty():
		_failures.append("the probe shader does not compile")
		return
	_shader = _rd.shader_create_from_spirv(spirv, "cross_queue_hazard_probe")
	_pipeline = _rd.compute_pipeline_create(_shader)
	var tf := RDTextureFormat.new()
	tf.width = SIZE
	tf.height = SIZE
	tf.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	tf.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	_tex_a = _rd.texture_create(tf, RDTextureView.new())
	var image := _rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE, _tex_a, 0)
	_tex_b = _rd.texture_create_from_extension(RenderingDevice.TEXTURE_TYPE_2D, tf.format, RenderingDevice.TEXTURE_SAMPLES_1, tf.usage_bits, image, SIZE, SIZE, 1, 1)
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	uniform.binding = 0
	uniform.add_id(_tex_a)
	_uniform_set = _rd.uniform_set_create([uniform], _shader, 0)
	if not (_pipeline.is_valid() and _tex_a.is_valid() and _tex_b.is_valid() and _uniform_set.is_valid()):
		_failures.append("the probe resources were not created")


func _record() -> void:
	if not _failures.is_empty():
		return
	var list := _rd.compute_list_begin(RenderingDevice.COMPUTE_QUEUE_ASYNC)
	_rd.compute_list_bind_compute_pipeline(list, _pipeline)
	_rd.compute_list_bind_uniform_set(list, _uniform_set, 0)
	_rd.compute_list_dispatch(list, SIZE / 8, SIZE / 8, 1)
	_rd.compute_list_end()
	_rd.texture_clear(_tex_b, Color(0, 1, 0, 1), 0, 1, 0, 1)


func _process(_delta: float) -> bool:
	if _local:
		return false
	if _frame == 1:
		_before = _counters()
	if _frame >= 1 and _frame <= FRAMES:
		RenderingServer.call_on_render_thread(_record)
	_frame += 1
	if _frame == FRAMES + 6:
		_finish()
	return false


# The device's "async:<submitted>/<fell back>" frame counters from get_perf_report() (#174).
func _counters() -> Vector2i:
	var report := _rd.get_perf_report() if _rd != null else ""
	var at := report.find(" async:")
	if at < 0:
		return Vector2i(-1, -1)
	var pair := report.substr(at + 7).get_slice(" ", 0).split("/")
	return Vector2i(int(pair[0]), int(pair[1])) if pair.size() == 2 else Vector2i(-1, -1)


func _finish() -> void:
	var after := _counters() if _rd != null else Vector2i(-1, -1)
	var submitted := after.x - _before.x
	var fell_back := after.y - _before.y
	if _failures.is_empty():
		if after.x < 0 or _before.x < 0:
			_failures.append("get_perf_report() has no async: counter")
		elif submitted < 1 or fell_back != 0:
			_failures.append("%d frames forked and %d fell back in %d probe frames, expected forks and no fallback: is rendering/rendering_device/async_compute on?" % [submitted, fell_back, FRAMES])
	for rid in [_uniform_set, _tex_b, _tex_a, _pipeline, _shader]:
		if rid.is_valid():
			_rd.free_rid(rid)
	if _local and _rd != null:
		_rd.free()
	var result := {
		"test": "cross_queue_hazard_probe",
		"device": "local" if _local else "main",
		"frames": FRAMES,
		"submitted": submitted,
		"fell_back": fell_back,
		"failures": _failures,
		"pass": _failures.is_empty(),
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if _failures.is_empty() else 1)
