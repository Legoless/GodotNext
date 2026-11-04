extends Node

## Scenario-wide RT registry check (#39).
##
## A box 10 m behind the camera must enter the TLAS with the box in front of it.
## The selection is not a frustum cull. Prints one "GODOTNEXT_RESULT <json>" line.

const AS_BUDGET_BYTES := 1024 * 1024 * 1024


var _failures := PackedStringArray()


func _ready() -> void:
	var result := await run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func run() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)

	var world := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.2, 0.2, 0.2)
	environment.nextgi_enabled = true
	environment.sdfgi_enabled = false
	world.environment = environment
	viewport.add_child(world)

	var camera := Camera3D.new()
	camera.current = true
	viewport.add_child(camera)

	# Godot cameras look down -Z, so +Z is behind the camera.
	_add_box(viewport, Vector3(0, 0, 10))
	_add_box(viewport, Vector3(0, 0, -5))

	var info := {}
	var matched := false
	for _frame in 90:
		await get_tree().process_frame
		info = RenderingServer.nextgi_get_tier_info()
		if int(info.get("rt_instance_count", 0)) >= 2:
			matched = true
			break
	_check(info.get("tier", "") == "HWRT", "HWRT tier, got %s" % info.get("tier", ""))
	_check(matched, "instance behind the camera is in the TLAS, count %s" % info.get("rt_instance_count", 0))
	var as_bytes := int(info.get("rt_as_bytes", 0))
	_check(as_bytes <= AS_BUDGET_BYTES, "AS bytes %d stay within the budget" % as_bytes)
	var build_triangles := int(info.get("rt_blas_build_triangles", 0))
	_check(build_triangles <= 250000, "BLAS triangles this frame %d stay within 250k" % build_triangles)
	return {
		"pass": _failures.is_empty(),
		"failures": " | ".join(_failures),
		"rt_instance_count": int(info.get("rt_instance_count", 0)),
		"rt_as_bytes": as_bytes,
		"rt_blas_build_triangles": build_triangles,
		"rt_tlas_build_count": int(info.get("rt_tlas_build_count", 0)),
	}


func _add_box(parent: Node, position: Vector3) -> void:
	var mesh_instance := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(1, 1, 1)
	mesh_instance.mesh = box
	mesh_instance.position = position
	mesh_instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	parent.add_child(mesh_instance)
