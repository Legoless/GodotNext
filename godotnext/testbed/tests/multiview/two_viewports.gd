extends Node3D

## Two-viewport NextGI check for #58 (G3-8 at build scope).
##
## Two SubViewports share one World3D with a NextGI-enabled environment and a
## camera each. G3-8 (§12, v3.7 wording): the scenario passes (AS updates, AS
## builds, SRC capture, SRC relight) run once per frame, in the first view, and
## each view runs its own WRC update (#253). Checked on STAMP_FRAMES frames of
## RenderingDevice timestamps: the first view's slots are stamped "NextGI: <slot>",
## the second view's "NextGI:View1:<slot>". The tier info must count a WRC per
## view. This is a functional check; timing is PQ.
##
## Run through run_two_viewports.sh, which enables NextGI and timestamp capture.

const FRAMES := 32
const STAMP_FRAMES := 8
const SCENARIO_SLOTS := ["AS updates", "AS builds", "SRC capture", "SRC relight"]

var _failures := PackedStringArray()


func _ready() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.02, 0.03)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(1, 1, 1)
	environment.ambient_light_energy = 0.2
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-60, -30, 0)
	add_child(light)

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 2, 2)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	add_child(instance)

	for index in 2:
		var viewport := SubViewport.new()
		viewport.name = "Viewport%d" % index
		viewport.size = Vector2i(512, 288)
		viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		var camera := Camera3D.new()
		camera.name = "Camera%d" % index
		camera.position = Vector3(index * 3.0 - 1.5, 0, 6)
		viewport.add_child(camera)
		camera.current = true
		add_child(viewport)
	_run()


func _run() -> void:
	for i in FRAMES:
		await RenderingServer.frame_post_draw
	var checked := 0
	var seen := {}
	var device := RenderingServer.get_rendering_device()
	for i in FRAMES * 4:
		if checked >= STAMP_FRAMES:
			break
		await RenderingServer.frame_post_draw
		var count := int(device.get_captured_timestamps_count())
		var stamp_frame := int(device.get_captured_timestamps_frame())
		if count == 0 or seen.has(stamp_frame):
			continue
		seen[stamp_frame] = true
		var names := PackedStringArray()
		for index in count:
			names.append(device.get_captured_timestamp_name(index))
		_check_stamps(names)
		checked += 1
	if checked < STAMP_FRAMES:
		_failures.append("only %d of %d frames had timestamps; run Godot with --gpu-profile" % [checked, STAMP_FRAMES])
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	if int(info.get("wrc_views", -1)) != 2:
		_failures.append("two views keep %d WRCs, expected one each" % int(info.get("wrc_views", -1)))

	var status: Dictionary = RenderingServer.nextgi_get_status()
	var tier := str(status.get("tier", ""))
	if tier != "HWRT":
		_failures.append("tier is %s, expected HWRT" % tier)
	if get_child_count() < 5:
		_failures.append("the two viewports were not created")

	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "nextgi_two_viewports",
		"pass": ok,
		"tier": tier,
		"viewports": 2,
		"stamp_frames": checked,
		"wrc_views": int(info.get("wrc_views", -1)),
		"failures": _failures,
	}))
	get_tree().quit(0 if ok else 1)


# One frame's timestamps: each scenario slot once, in the first view; a WRC update in each view.
func _check_stamps(p_names: PackedStringArray) -> void:
	for slot in SCENARIO_SLOTS:
		var first := p_names.count("NextGI: " + slot)
		var second := p_names.count("NextGI:View1:" + slot)
		if first != 1 or second != 0:
			_fail_once("the scenario pass %s ran %d times in the first view and %d in the second, expected once in the first" % [slot, first, second])
	for stamp in ["NextGI: WRC update", "NextGI:View1:WRC update"]:
		var runs := p_names.count(stamp)
		if runs != 1:
			_fail_once("%s ran %d times in a frame, expected once: each view updates its own WRC" % [stamp, runs])


func _fail_once(p_failure: String) -> void:
	if not _failures.has(p_failure):
		_failures.append(p_failure)
