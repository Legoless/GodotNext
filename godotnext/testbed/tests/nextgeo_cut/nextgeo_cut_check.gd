extends Node3D
## NextGeo cut check (#76). Loads the generated Dense City Micro Detail scene
## and prints the GPU cut stats after warmup. Requires the scene to exist
## (run generators/dense_city/dense_city_generator.gd --variant=micro_detail)
## and rendering/nextgeo/debug_stats=true.

const CITY := "res://generated/dense_city/micro_detail.tscn"

var _frames := 0


func _ready() -> void:
	if not ResourceLoader.exists(CITY):
		print("NEXTGEO_CUT skip: %s is missing." % CITY)
		get_tree().quit(0)
		return
	var scene: PackedScene = load(CITY)
	var root := scene.instantiate()
	add_child(root)
	var camera := Camera3D.new()
	camera.position = Vector3(-40.0, 3.0, 0.0)
	camera.rotation_degrees = Vector3(0.0, -90.0, 0.0)
	camera.current = true
	add_child(camera)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 20:
		var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		print("NEXTGEO_CUT %s" % JSON.stringify(stats))
		get_tree().quit(0)
