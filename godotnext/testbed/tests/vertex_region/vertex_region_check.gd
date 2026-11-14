extends Node3D

## A vertex-region update keeps the mesh's distance field (#252; plan §21.2 L2, as UE: a mesh
## distance field is not rebuilt at runtime and does not follow later deformation).
##
## A floor, a BoxMesh and an ArrayMesh box (positions only and uncompressed, so stride 12). At
## UPDATE_FRAME one RenderingServer.mesh_surface_update_vertex_region() moves the ArrayMesh's
## positions MOVE_M up. Pass/fail, sampled every SAMPLE_EVERY frames through END_FRAME:
## - software tier: swrt_trace_instances and swrt_record_count keep their pre-update value
##   (all INSTANCES), and the atlas counts the kept update (mesh_sdf_kept_after_update >= 1);
## - HWRT: rt_instance_count keeps its pre-update value.
## Run with NextGI on, as the runner (run_vertex_region_check.sh) does for hardware_ray_tracing
## Off, Auto and debug/deny_ray_query; --expect=<TIER> names the tier the run must be on.
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const UPDATE_FRAME := 300
const END_FRAME := 700
const SAMPLE_EVERY := 50
const MOVE_M := 0.5
const INSTANCES := 3
const POSITION_STRIDE := 12

var _frames := 0
var _expect := ""
var _mesh: ArrayMesh
var _positions := PackedVector3Array()
var _before := {}
var _samples: Array[Dictionary] = []
var _failures := PackedStringArray()


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--expect="):
			_expect = arg.trim_prefix("--expect=")

	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(20.0, 0.2, 20.0)
	_add_instance("Floor", floor_mesh, Vector3(0.0, -0.1, 0.0))
	var box := BoxMesh.new()
	box.size = Vector3(1.0, 1.0, 1.0)
	_add_instance("Box", box, Vector3(-1.5, 0.5, 0.0))
	_mesh = _make_array_box(Vector3(1.0, 1.0, 1.0))
	_add_instance("ArrayBox", _mesh, Vector3(1.5, 0.5, 0.0))

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 3.0, 6.0)
	camera.rotation_degrees = Vector3(-25.0, 0.0, 0.0)
	camera.current = true
	add_child(camera)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = Sky.new()
	environment.sky.sky_material = ProceduralSkyMaterial.new()
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)


func _add_instance(p_name: String, p_mesh: Mesh, p_position: Vector3) -> void:
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = p_mesh
	instance.position = p_position
	add_child(instance)


func _make_array_box(p_size: Vector3) -> ArrayMesh:
	var h := p_size * 0.5
	for z in [-h.z, h.z]:
		for y in [-h.y, h.y]:
			for x in [-h.x, h.x]:
				_positions.push_back(Vector3(x, y, z))
	# Corners are indexed x + 2y + 4z; each face is two counter-clockwise triangles seen from outside.
	var faces := [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]]
	var indices := PackedInt32Array()
	for face in faces:
		indices.append_array(PackedInt32Array([face[0], face[2], face[1], face[0], face[3], face[2]]))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = _positions
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _counts() -> Dictionary:
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	return {
		"frame": _frames,
		"tier": str(info.get("tier", "")),
		"swrt_trace_instances": int(info.get("swrt_trace_instances", -1)),
		"swrt_record_count": int(info.get("swrt_record_count", -1)),
		"rt_instance_count": int(info.get("rt_instance_count", -1)),
		"mesh_sdf_kept_after_update": int(info.get("mesh_sdf_kept_after_update", -1)),
	}


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == UPDATE_FRAME:
		_before = _counts()
		var stride := RenderingServer.mesh_surface_get_format_vertex_stride(_mesh.surface_get_format(0), _positions.size())
		if stride != POSITION_STRIDE:
			_failures.append("the ArrayMesh's vertex stride is %d, expected %d (positions only, uncompressed)" % [stride, POSITION_STRIDE])
		var moved := PackedVector3Array()
		for position in _positions:
			moved.push_back(position + Vector3(0.0, MOVE_M, 0.0))
		RenderingServer.mesh_surface_update_vertex_region(_mesh.get_rid(), 0, 0, moved.to_byte_array())
	elif _frames > UPDATE_FRAME and (_frames - UPDATE_FRAME) % SAMPLE_EVERY == 0:
		_samples.append(_counts())
		if _frames >= END_FRAME:
			_finish()


func _finish() -> void:
	var tier := str(_before.get("tier", ""))
	if not _expect.is_empty() and tier != _expect:
		_failures.append("tier is %s, expected %s" % [tier, _expect])
	var keys: Array[String] = []
	if tier == "SWRT":
		keys = ["swrt_trace_instances", "swrt_record_count"]
		for key in keys:
			if int(_before[key]) != INSTANCES:
				_failures.append("%s is %d before the update, expected %d" % [key, int(_before[key]), INSTANCES])
		if int(_samples[-1]["mesh_sdf_kept_after_update"]) < 1:
			_failures.append("mesh_sdf_kept_after_update is %d, expected the update to be counted" % int(_samples[-1]["mesh_sdf_kept_after_update"]))
	elif tier == "HWRT":
		keys = ["rt_instance_count"]
		if int(_before["rt_instance_count"]) < INSTANCES:
			_failures.append("rt_instance_count is %d before the update, expected at least %d" % [int(_before["rt_instance_count"]), INSTANCES])
	else:
		_failures.append("NextGI is not on an active tier (%s)" % tier)
	for sample in _samples:
		for key in keys:
			if int(sample[key]) != int(_before[key]):
				_failures.append("frame %d: %s is %d, %d before the update" % [int(sample["frame"]), key, int(sample[key]), int(_before[key])])
	var result := {
		"test": "vertex_region",
		"pass": _failures.is_empty(),
		"tier": tier,
		"expect": _expect,
		"before": _before,
		"after": _samples[-1],
		"failures": _failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if _failures.is_empty() else 1)
