extends Node3D
## NextGeo draw path (c) visibility raster & per-upscaler velocity check (#83).
##
## Builds a procedural virtualized mesh via `ImporterMesh.build_virtual_geometry`
## alongside a plain legacy moving `BoxMesh` and a panning camera, with
## `rendering/nextgeo/visibility_only = true` and `debug_stats = true`.
##
## In sweep mode (default), steps through TAA -> FSR2 -> MetalFX (on Metal) ->
## DLSS/DEV DENSE (when active) and checks:
## - `nextgeo/visibility` (25|7 packed ID, `< 128` triangle, `0xFFFFFFFF` bg)
## - `nextgeo/instance_id` (record slot vs `0xFFFFFFFF` legacy/bg)
## - `raster_indirect_draws == 2` (`1` indirect draw per phase per view)
## - TAA background `(0, 0)`, FSR2 background `(-1, -1)`, MetalFX / DENSE
##   exact-sentinel camera velocity fill + preserved virtual raster velocity +
##   `Q21 == 0`.
##
## In capture mode (`--upscaler=taa|fsr2|metalfx|dlss [--frames=N]
## [--velocity-dump=/abs/dir]`), holds one mode for `--write-movie` runs.

const ProbeScript = preload("res://tests/nextgeo_visibility/nextgeo_visibility_probe.gd")
const SENTINEL_U32 := 0xFFFFFFFF

var _camera: Camera3D
var _virtual_mover: MeshInstance3D
var _legacy_mover: MeshInstance3D
var _probe: CompositorEffect

var _single_mode := ""
var _capture_frames := 16
var _velocity_dump_dir := ""

var _phases: Array[String] = []
var _phase_idx := 0
var _phase_frame := 0
var _checked_modes: Dictionary = {}
var _failures: Array[String] = []


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)

	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--upscaler="):
			_single_mode = arg.trim_prefix("--upscaler=").to_lower()
		elif arg.begins_with("--frames="):
			_capture_frames = maxi(int(arg.trim_prefix("--frames=")), 6)
		elif arg.begins_with("--velocity-dump="):
			_velocity_dump_dir = arg.trim_prefix("--velocity-dump=")

	_build_scene()
	if not _single_mode.is_empty():
		_phases = [_single_mode]
	else:
		_phases = ["taa", "fsr2"]
		if RenderingServer.get_current_rendering_driver_name().to_lower() == "metal":
			_phases.append("metalfx")
		_phases.append("dlss_probe")
	_apply_mode(_phases[0])


func _build_scene() -> void:
	var box := BoxMesh.new()
	box.size = Vector3(1.8, 1.8, 0.5)
	box.subdivide_width = 8
	box.subdivide_height = 8
	box.subdivide_depth = 4
	var vmat := StandardMaterial3D.new()
	vmat.albedo_color = Color(0.25, 0.7, 0.35)
	box.material = vmat

	var imp := ImporterMesh.from_mesh(box)
	var err := imp.build_virtual_geometry("user://nextgeo_visibility_check.ngm")
	var vmesh := imp.get_mesh()
	if err != OK or vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("ImporterMesh.build_virtual_geometry failed (err=%d)" % err)

	_virtual_mover = MeshInstance3D.new()
	_virtual_mover.name = "VirtualMover"
	_virtual_mover.mesh = vmesh
	_virtual_mover.position = Vector3(-0.95, 0.0, 0.0)
	add_child(_virtual_mover)

	var lbox := BoxMesh.new()
	lbox.size = Vector3(1.8, 1.8, 0.5)
	var lmat := StandardMaterial3D.new()
	lmat.albedo_color = Color(0.8, 0.35, 0.25)
	lbox.material = lmat

	_legacy_mover = MeshInstance3D.new()
	_legacy_mover.name = "LegacyMover"
	_legacy_mover.mesh = lbox
	_legacy_mover.position = Vector3(0.95, 0.0, 0.0)
	add_child(_legacy_mover)

	_camera = Camera3D.new()
	_camera.name = "MainCam"
	_camera.fov = 50.0
	_camera.position = Vector3(0.0, 0.0, 5.0)
	_camera.current = true
	add_child(_camera)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45.0, 30.0, 0.0)
	add_child(sun)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.12, 0.14, 0.18)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.4, 0.45)

	var comp := Compositor.new()
	var effects: Array[CompositorEffect] = []
	_probe = ProbeScript.new()
	effects.append(_probe)
	if not _velocity_dump_dir.is_empty():
		var dump := load("res://harness/velocity_dump.gd").new() as CompositorEffect
		dump.set("output_dir", _velocity_dump_dir)
		effects.append(dump)
	comp.compositor_effects = effects

	var world_env := WorldEnvironment.new()
	world_env.environment = env
	world_env.compositor = comp
	add_child(world_env)


func _apply_mode(p_mode: String) -> void:
	var vp := get_viewport()
	_phase_frame = 0
	_camera.position = Vector3(0.0, 0.0, 5.0)
	_camera.rotation_degrees = Vector3.ZERO
	_virtual_mover.position = Vector3(-1.10, 0.0, 0.0)
	_legacy_mover.position = Vector3(1.10, 0.0, 0.0)
	match p_mode:
		"taa":
			vp.use_taa = true
			vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			vp.scaling_3d_scale = 1.0
		"fsr2":
			vp.use_taa = false
			vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			vp.scaling_3d_scale = 0.67
		"metalfx":
			vp.use_taa = false
			vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_METALFX_TEMPORAL
			vp.scaling_3d_scale = 0.67
		"dlss", "dlss_probe":
			vp.use_taa = false
			vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
			vp.scaling_3d_scale = 0.67
		_:
			_failures.append("unknown upscaler mode: %s" % p_mode)


func _step_motion(p_frame: int) -> void:
	# Pitch the camera (+X rotation) so even infinite-depth background sky pixels
	# get non-zero camera velocity on DENSE paths, while virtual (+X) and legacy
	# (-X) movers translate in opposite horizontal directions.
	var step := float(p_frame)
	_camera.position = Vector3(0.0, 0.0, 5.0)
	_camera.rotation_degrees = Vector3(step * 0.35, 0.0, 0.0)
	_virtual_mover.position = Vector3(-1.20 + step * 0.035, 0.0, 0.0)
	_legacy_mover.position = Vector3(1.20 - step * 0.035, 0.0, 0.0)
	var vp_size := get_viewport().get_visible_rect().size
	if vp_size.x > 1.0 and vp_size.y > 1.0:
		_probe.set("virt_uv", _camera.unproject_position(_virtual_mover.global_position) / vp_size)
		_probe.set("leg_uv", _camera.unproject_position(_legacy_mover.global_position) / vp_size)


func _process(_delta: float) -> void:
	_phase_frame += 1
	_step_motion(_phase_frame)

	var mode := _phases[_phase_idx]
	var target_frame := _capture_frames if not _single_mode.is_empty() else 6
	if _phase_frame < 4:
		return

	if not _single_mode.is_empty():
		_verify_sample(mode)
		if _phase_frame >= target_frame:
			_finish()
		return

	if _phase_frame >= target_frame:
		_verify_sample(mode)
		_phase_idx += 1
		if _phase_idx >= _phases.size():
			_finish()
			return
		_apply_mode(_phases[_phase_idx])


func _verify_sample(p_mode: String) -> void:
	var sample: Dictionary = _probe.get("last_sample")
	if sample.is_empty():
		_failures.append("%s: missing GPU probe sample" % p_mode)
		return

	var status: Dictionary = RenderingServer.viewport_get_scaling_3d_status(get_viewport().get_viewport_rid())
	var active_mode := int(status.get("active_mode", -1))
	if p_mode == "dlss_probe" and active_mode != Viewport.SCALING_3D_MODE_DLSS:
		_checked_modes["dlss"] = "fallback(%d)" % active_mode
		return

	var stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())
	var indirect_draws := int(stats.get("raster_indirect_draws", 0))
	if indirect_draws != 2:
		_failures.append("%s: expected raster_indirect_draws == 2, got %d" % [p_mode, indirect_draws])

	var virtual_pixels := int(sample["virtual_pixels"])
	var bg_vis := int(sample["bg_vis"])
	var bg_inst := int(sample["bg_inst"])
	var virt_vis := int(sample["virt_vis"])
	var virt_inst := int(sample["virt_inst"])
	var leg_vis := int(sample["leg_vis"])
	var leg_inst := int(sample["leg_inst"])
	var bg_vel: Vector2 = sample["bg_vel"]
	var virt_vel: Vector2 = sample["virt_vel"]
	var leg_vel: Vector2 = sample["leg_vel"]
	var sentinel_count := int(sample["sentinel_count"])
	var nonfinite_count := int(sample["nonfinite_count"])

	if virtual_pixels <= 0:
		_failures.append("%s: virtual_pixels == 0" % p_mode)
	if bg_vis != SENTINEL_U32 or bg_inst != SENTINEL_U32:
		_failures.append("%s: background vis/inst not 0xFFFFFFFF (%d, %d)" % [p_mode, bg_vis, bg_inst])
	if leg_vis != SENTINEL_U32 or leg_inst != SENTINEL_U32:
		_failures.append("%s: legacy mover vis/inst not 0xFFFFFFFF (%d, %d)" % [p_mode, leg_vis, leg_inst])
	if virt_vis == SENTINEL_U32 or virt_inst == SENTINEL_U32:
		_failures.append("%s: virtual mover vis/inst stayed 0xFFFFFFFF (%d, %d)" % [p_mode, virt_vis, virt_inst])
	else:
		var tri := virt_vis & 0x7F
		if tri >= 128:
			_failures.append("%s: unpacked triangle %d >= 128" % [p_mode, tri])
	if nonfinite_count != 0:
		_failures.append("%s: nonfinite velocity texels = %d" % [p_mode, nonfinite_count])

	# Virtual and legacy movers translate in opposite X directions.
	if absf(virt_vel.x) < 1e-4:
		_failures.append("%s: virtual mover X velocity too small (%s)" % [p_mode, str(virt_vel)])
	if absf(leg_vel.x) < 1e-4:
		_failures.append("%s: legacy mover X velocity too small (%s)" % [p_mode, str(leg_vel)])
	if virt_vel.x * leg_vel.x >= 0.0:
		_failures.append("%s: virtual (%s) and legacy (%s) X velocities should have opposite signs" % [p_mode, str(virt_vel), str(leg_vel)])

	match p_mode:
		"taa":
			if bg_vel.length() > 1e-5:
				_failures.append("taa: expected (0,0) background velocity, got %s" % str(bg_vel))
		"fsr2":
			if not is_equal_approx(bg_vel.x, -1.0) or not is_equal_approx(bg_vel.y, -1.0):
				_failures.append("fsr2: expected (-1,-1) background velocity, got %s" % str(bg_vel))
			if sentinel_count <= 0:
				_failures.append("fsr2: expected background (-1,-1) sentinels > 0, got 0")
		"metalfx", "dlss", "dlss_probe":
			if active_mode != (Viewport.SCALING_3D_MODE_METALFX_TEMPORAL if p_mode == "metalfx" else Viewport.SCALING_3D_MODE_DLSS):
				_failures.append("%s: unexpected active_mode=%d" % [p_mode, active_mode])
			if sentinel_count != 0:
				_failures.append("%s: Q21 sentinel_count=%d (must be 0)" % [p_mode, sentinel_count])
			if absf(bg_vel.y) < 1e-4:
				_failures.append("%s: expected non-zero camera Y velocity on background, got %s" % [p_mode, str(bg_vel)])
			if absf(virt_vel.x - bg_vel.x) < 1e-4:
				_failures.append("%s: virtual pixel lost raster velocity (virt=%s bg=%s)" % [p_mode, str(virt_vel), str(bg_vel)])

	var key := "dlss" if p_mode == "dlss_probe" else p_mode
	_checked_modes[key] = {
		"active_mode": active_mode,
		"virtual_pixels": virtual_pixels,
		"bg_vel": [snappedf(bg_vel.x, 1e-5), snappedf(bg_vel.y, 1e-5)],
		"virt_vel": [snappedf(virt_vel.x, 1e-5), snappedf(virt_vel.y, 1e-5)],
		"leg_vel": [snappedf(leg_vel.x, 1e-5), snappedf(leg_vel.y, 1e-5)],
		"q21": sentinel_count + nonfinite_count if p_mode in ["metalfx", "dlss", "dlss_probe"] else nonfinite_count,
		"raster_indirect_draws": indirect_draws,
	}


func _finish() -> void:
	for m in _phases:
		var k := "dlss" if m == "dlss_probe" else m
		if not _checked_modes.has(k):
			_failures.append("mode never verified: %s" % k)
	var ok := _failures.is_empty()
	print("NEXTGEO_VISIBILITY_SUMMARY pass=%s modes=%s failures=%s" % [
		str(ok),
		JSON.stringify(_checked_modes),
		JSON.stringify(_failures),
	])
	get_tree().quit(0 if ok else 1)
