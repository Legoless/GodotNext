extends Node3D
## NextGeo cut parity check (#76). Loads the generated Dense City Micro Detail
## scene, then walks a seeded set of camera views and instance scales. Each
## view is held until the asynchronous debug readback for it arrives, so every
## single view is compared once: the GPU cut hash must equal the CPU reference
## cut hash (with rendering/nextgeo/occlusion=false, since the CPU reference
## does not model Hi-Z).
##
## Requires the scene to exist and rendering/nextgeo/debug_stats=true.
## Use --views=N (default 24).

const CITY := "res://generated/dense_city/micro_detail.tscn"
const SCALES: Array[float] = [0.5, 1.0, 4.0]

var _views := 24
var _checked := 0
var _matched := 0
var _max_phase2 := 0
var _max_overflow := 0
var _last_tick := -1
var _view := -1
var _rng := RandomNumberGenerator.new()
var _city: Node3D
var _camera: Camera3D
var _frames := 0


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	for index in range(args.size()):
		if args[index].begins_with("--views="):
			_views = int(args[index].trim_prefix("--views="))
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
	_apply_view(0)


func _apply_view(p_view: int) -> void:
	_view = p_view
	_city.scale = Vector3.ONE * SCALES[p_view % SCALES.size()]
	var angle := _rng.randf_range(0.0, TAU)
	# Alternate close street-level views with far orbit views so buildings
	# occlude one another.
	var radius := _rng.randf_range(8.0, 80.0) if p_view % 2 == 0 else _rng.randf_range(80.0, 320.0)
	var eye := Vector3(cos(angle) * radius, _rng.randf_range(1.7, 30.0), sin(angle) * radius)
	_camera.transform = Transform3D(Basis.looking_at((-eye).normalized(), Vector3.UP), eye)


func _process(_delta: float) -> void:
	_frames += 1
	if _frames <= 2:
		return
	var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var tick := int(stats.get("tick", -1))
	if tick == _last_tick or not stats.has("cut_match"):
		return
	_last_tick = tick
	if _view >= 0:
		_checked += 1
		if bool(stats["cut_match"]):
			_matched += 1
		else:
			print("NEXTGEO_CUT mismatch view=%d gpu=%s cpu=%s gpu_clusters=%s cpu_clusters=%s" % [
				_view, stats.get("cut_hash", 0), stats.get("cpu_cut_hash", 0),
				stats.get("clusters", 0), stats.get("cpu_clusters", 0)])
		_max_phase2 = maxi(_max_phase2, int(stats.get("phase2_instances", 0)))
		_max_overflow = maxi(_max_overflow, int(stats.get("overflow", 0)))
	if _checked >= _views:
		print("NEXTGEO_CUT_SUMMARY views=%d checked=%d matches=%d max_phase2=%d max_overflow=%d" % [_views, _checked, _matched, _max_phase2, _max_overflow])
		get_tree().quit(0 if _matched == _checked and _max_overflow == 0 else 1)
		return
	_apply_view(_checked)
