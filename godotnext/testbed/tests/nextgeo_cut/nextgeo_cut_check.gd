extends Node3D
## NextGeo cut check (#76). Loads the generated Dense City Micro Detail scene,
## then walks a seeded set of camera views and instance scales, comparing the
## GPU cut hash with the CPU reference each frame. Requires the scene to exist
## and rendering/nextgeo/debug_stats=true. Use --views=N (default 24).

const CITY := "res://generated/dense_city/micro_detail.tscn"
const SCALES: Array[float] = [0.5, 1.0, 4.0]

var _views := 24
var _static := false
var _frames := 0
var _matched := 0
var _checked := 0
var _max_phase2 := 0
var _last_tick := -1
var _rng := RandomNumberGenerator.new()
var _city: Node3D
var _camera: Camera3D


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for index in range(args.size()):
		if args[index].begins_with("--views="):
			_views = int(args[index].trim_prefix("--views="))
		elif args[index] == "--static":
			_static = true
	if not ResourceLoader.exists(CITY):
		print("NEXTGEO_CUT skip: %s is missing." % CITY)
		get_tree().quit(0)
		return
	var scene: PackedScene = load(CITY)
	_city = scene.instantiate()
	add_child(_city)
	_camera = Camera3D.new()
	_camera.current = true
	add_child(_camera)
	_rng.seed = 1337


func _process(_delta: float) -> void:
	_frames += 1
	if _frames <= 2:
		return
	var view := _frames - 3
	if view < _views:
		_city.scale = Vector3.ONE * SCALES[view % SCALES.size()]
		var angle := _rng.randf_range(0.0, TAU)
		# Alternate far orbit views with close street-level views so buildings
		# occlude one another (the phase-2 path).
		var radius := _rng.randf_range(8.0, 80.0) if view % 2 == 0 else _rng.randf_range(80.0, 320.0)
		var eye := Vector3(cos(angle) * radius, _rng.randf_range(1.7, 30.0), sin(angle) * radius)
		_camera.transform = Transform3D(Basis.looking_at((-eye).normalized(), Vector3.UP), eye)
		var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
		if view == 0:
			print("NEXTGEO_CUT_VIEW0 %s" % JSON.stringify(stats))
		var tick := int(stats.get("tick", -1))
		if stats.has("cut_match") and tick != _last_tick:
			_last_tick = tick
			_checked += 1
			if bool(stats["cut_match"]):
				_matched += 1
			_max_phase2 = maxi(_max_phase2, int(stats.get("phase2_instances", 0)))
			if not bool(stats["cut_match"]):
				print("NEXTGEO_CUT mismatch view=%d gpu=%s cpu=%s gpu_clusters=%s cpu_clusters=%s" % [
					view, stats.get("cut_hash", 0), stats.get("cpu_cut_hash", 0), stats.get("clusters", 0), stats.get("cpu_clusters", 0)])
		return
	print("NEXTGEO_CUT_SUMMARY views=%d checked=%d matches=%d max_phase2=%d" % [_views, _checked, _matched, _max_phase2])
	get_tree().quit(0)
