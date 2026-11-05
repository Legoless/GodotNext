extends Node3D
## MegaLights Hall (#213). The committed scene carries the 64-light feature
## mix; `--lights=N` extends the count to the 1024 or 4096 variant with a
## deterministic grid of extra shadowed omnis. `--frames=N` quits.

const SceneArgs = preload("res://scenes/scene_args.gd")

const COUNTS: Array[int] = [64, 1024, 4096]

var _frame := 0
var _limit := -1
var _requested := 64
var _spawned := 0


func _ready() -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	_limit = SceneArgs.int_value("--frames", -1)
	var requested := SceneArgs.int_value("--lights", 64)
	if requested in COUNTS:
		_requested = requested
	else:
		push_warning("mega_lights_hall: unknown light count %d, using 64." % requested)
	_spawn_extra_lights()


func _process(_delta: float) -> void:
	if bool(get_meta("skip_runtime", false)):
		return
	_frame += 1
	if _limit >= 0 and _frame >= _limit:
		get_tree().quit(0)


func _spawn_extra_lights() -> void:
	var lights := get_node_or_null("Lights")
	if lights == null:
		return
	var existing := lights.get_child_count()
	var target := _requested
	if target <= existing:
		return
	var rng := RandomNumberGenerator.new()
	rng.seed = int(get_meta("seed", 1)) + 7
	for index in range(existing, target):
		var omni := OmniLight3D.new()
		omni.name = "Extra%05d" % index
		var column := index % 32
		var row := (index / 32) % 32
		var layer := index / 1024
		omni.position = Vector3(-31.0 + float(column) * 2.0, 1.2 + float(layer) * 2.2, -18.0 + float(row) * 1.2)
		omni.light_color = Color(0.6 + rng.randf_range(0.0, 0.4), 0.55, 1.0 - rng.randf_range(0.0, 0.4))
		omni.light_energy = 0.8
		omni.omni_range = 3.0
		omni.shadow_enabled = true
		lights.add_child(omni)
	_spawned = target - existing
	print("mega_lights_hall: %d base + %d extra = %d lights." % [existing, _spawned, existing + _spawned])
