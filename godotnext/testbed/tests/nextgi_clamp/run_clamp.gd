extends Node3D

## Firefly-clamp counter check for #54 (Q16).
##
## Loads Fog + Emissive Street, turns NextGI on and reads the clamped-energy
## fraction after a few frames. The diffuse trace must have run and the
## fraction must be a fraction of the total sample luminance.
##
## Run through run_clamp.sh, which enables NextGI.

const FRAMES := 24

var _failures := PackedStringArray()


func _ready() -> void:
	var packed: PackedScene = load("res://scenes/fog_emissive_street/fog_emissive_street.tscn")
	var scene := packed.instantiate()
	add_child(scene)
	var world := scene.get_node_or_null("WorldEnvironment") as WorldEnvironment
	if world != null and world.environment != null:
		var environment := world.environment.duplicate()
		environment.nextgi_enabled = true
		world.environment = environment
	var camera := scene.get_node_or_null("CaptureCam") as Camera3D
	if camera != null:
		camera.current = true
	_run()


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw

	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := str(info.get("tier", ""))
	var fraction := float(info.get("clamped_energy_fraction", 0.0))
	var removed := int(info.get("clamped_energy_removed", 0))
	var total := int(info.get("clamped_energy_total", 0))

	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if total <= 0:
		_failures.append("the clamp counter never received samples")
	if fraction < 0.0 or fraction > 1.0:
		_failures.append("clamped_energy_fraction %f is out of range" % fraction)

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_clamp",
		"pass": ok,
		"tier": tier,
		"fraction": fraction,
		"removed": removed,
		"total": total,
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)
