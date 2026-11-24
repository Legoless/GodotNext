extends Node3D

## Product path tracer check (#167, plan §21.0.6).
##
## Color Box Room A, the phase smoke scene's Color Box Room (§22.3), path traced on the root viewport
## (Viewport.path_tracing_enabled) from CaptureCam. The check reads
## Viewport.get_path_tracing_info() after each drawn frame and proves:
## - the history accumulates one sample per drawn frame and nothing restarts it while nothing changes
##   (the negative control), nor while materials it does not trace change every frame (a CanvasItem shader
##   parameter and an unused 3D material), and it stops at the sample target and goes on when the target is raised;
## - where frames are drawn only on changes (low-processor mode, as in the editor) the path tracer asks for each
##   next frame while it accumulates, and stops asking once it has converged;
## - every change event restarts it on the frame the change is drawn: the camera's transform and field
##   of view, an instance moved, added, hidden and freed, a material parameter, a light's energy and
##   position, a directional light added and changed, the environment's background, the bounce limit and
##   the firefly clamp, and (with the hit groups) a global shader parameter a traced material reads; each with its
##   reset reason and at most one sample afterwards. An added instance
##   may restart it again on the next frames (its BLAS enters the TLAS, its hit group links); every other
##   change settles at once;
## - a mesh deformed in place every frame (RenderingServer.mesh_surface_update_vertex_region(), its AABB unchanged)
##   restarts the history on every frame, also once it is dynamic and its BLAS refits in place;
## - the cull-mode rule on camera rays, with emissive quads in front of the camera: a one-sided quad is
##   seen from its front only, a cull_front quad from its back only, a cull_disabled quad from both, and
##   a mirrored instance keeps its sides;
## - camera rays follow the projection: the quad covers the pixels Camera3D.unproject_position() puts it on, for a
##   perspective, an orthogonal and an off-axis frustum camera;
## - materials come from the #130 hit groups where they run: an emission texture (black) that the D16
##   table cannot hold darkens a white emissive quad;
## - the bounce limit (0 bounces is darker than 32) and the firefly clamp (it caps a bright emitter);
## - the status without NextGI in the environment (NO_NEXTGI) and under debug/reference_mode
##   (REFERENCE_MODE), and the history restarting when path tracing comes back.
## - path tracing off and on again within a few frames: the backend comes back (on Direct3D 12 the new hit
##   groups wait for the previous ones' bindless tables to leave the frame delay) with no error;
## - a second, path-traced SubViewport resized while it accumulates: its history restarts at the new size.
## - the same integrator as the D16 reference: with debug/path_tracer_reference_match on (the INLINE
##   backend, the D16 table materials, pixel centers, no culling) and 63 bounces, the product's history
##   after MATCH_SAMPLES samples matches the reference's every pixel exactly (the reference_match line);
## - the product's random numbers are decorrelated across pixels: in its history after MATCH_SAMPLES samples,
##   neighbors across a MATCH_SAMPLES-wide pixel block differ as much as neighbors inside one (the streaks line);
##   the reference match, with the D16 reference's seeding, is the positive control and is reported.
## It also reports, without failing on it, the product's mean (hit groups) against the D16 reference's at
## the same sample count (the reference_ratio line; GP's Q1 is measured in PQ, #170), and the same for direct
## lighting only (0 bounces against the reference's Direct Only mode), with the room's materials and with every
## non-emissive surface white, where the hit groups' linear albedo and the D16 table's parameter value are
## equal (the direct_ratio line).
##
## Run it with NextGI on, through run_path_tracer_check.sh:
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/path_tracer/path_tracer_check.tscn
## Arguments after "--": --expect-backend=hit_groups (the default where RT pipelines run) or inline (no RT
## pipelines, or rendering/global_illumination/nextgi/hardware_ray_tracing_mode Simplified Material or Inline,
## #286: run_path_tracer_check.sh with PATH_TRACER_HWRT_MODE=1 or 2).
## --smoke=<res://scene> [--camera=<name>] runs the smoke render only: that scene (the other P0 testbed scene,
## Concave Courtyard, for example) path traced from the camera (CaptureCam by default) until SMOKE_SAMPLES
## samples, with a finite, non-black history. Add --expect-black for a scene lit only by what this path
## tracer does not evaluate yet (Concave Courtyard: a directional light and a sky, #169): its history must
## then be all zero, as the D16 reference renders it. --nextgeo-rt=native sets rendering/nextgeo/ray_tracing_mode
## to Native (Experimental) before the run and checks that NextGI reports it; --nextgeo-rt=fallback checks the
## default.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 when every check passes, 1 otherwise.

const SETTING_READBACK := "rendering/global_illumination/nextgi/debug/path_tracer_readback"
const SETTING_REFERENCE_MODE := "rendering/global_illumination/nextgi/debug/reference_mode"
const SETTING_REFERENCE_READBACK := "rendering/global_illumination/nextgi/debug/reference_readback"
const SETTING_REFERENCE_SEED := "rendering/global_illumination/nextgi/debug/reference_seed"
const SETTING_REFERENCE_EPOCH := "rendering/global_illumination/nextgi/debug/reference_epoch"
const SETTING_REFERENCE_MATCH := "rendering/global_illumination/nextgi/debug/path_tracer_reference_match"
const MAX_WAIT_FRAMES := 1200
const HIT_GROUP_WAIT_FRAMES := 900
const IDLE_FRAMES := 30
const CULL_SAMPLES := 4
const BRIGHT := 10.0
const DARK := 5.0
const EMISSION_ENERGY := 50.0
const REFERENCE_SAMPLES := 256
const SMOKE_SAMPLES := 64
const DIRECT_SAMPLES := 64
const SETTING_NEXTGEO_RT := "rendering/nextgeo/ray_tracing_mode"
# The reference adds 32 samples per frame (NextGIReference), so its history holds exactly this many after one frame.
const MATCH_SAMPLES := 32
const MATCH_TOLERANCE := 1e-3
const SETTLE_FRAMES := 120
const DEFORM_FRAMES := 16
# A deformation burst must refit the mesh in place on at least this many frames (it is dynamic after 4 updates).
const DEFORM_MIN_REFITS := 5
const LOW_PROCESSOR_FRAMES := 60
# At 1 m an orthogonal camera read as the D16 symmetric perspective (field of view from the projection) covers the
# same pixels as the orthogonal one; at 1.5 m it does not.
const PROJECTION_DISTANCE := 1.5
const PROJECTION_TOLERANCE_PX := 2.5
# The product's neighbor difference across a block boundary over the one inside a block.
const STREAK_MAX_RATIO := 1.25
const GLOBAL_TINT := "path_tracer_check_tint"

var _failures := PackedStringArray()
var _result := {}
var _expect_backend := "HIT_GROUPS"
var _scene_path := "res://scenes/color_box_room/cbr_a.tscn"
var _camera_name := "CaptureCam"
var _smoke := false
var _expect_black := false
var _nextgeo_rt := ""
var _denoiser_arg := ""
var _room: Node3D
var _camera: Camera3D
var _environment: Environment
var _quad: MeshInstance3D
var _quad_material: StandardMaterial3D


func _ready() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--expect-backend="):
			_expect_backend = argument.get_slice("=", 1).to_upper()
		elif argument.begins_with("--smoke="):
			_scene_path = argument.get_slice("=", 1)
			_smoke = true
		elif argument.begins_with("--camera="):
			_camera_name = argument.get_slice("=", 1)
		elif argument == "--expect-black":
			_expect_black = true
		elif argument.begins_with("--nextgeo-rt="):
			_nextgeo_rt = argument.get_slice("=", 1).to_lower()
		elif argument.begins_with("--denoiser="):
			_denoiser_arg = argument.get_slice("=", 1).to_lower()
	if _nextgeo_rt == "native":
		ProjectSettings.set_setting(SETTING_NEXTGEO_RT, 1)
	_run.call_deferred()


func _fail(p_message: String) -> void:
	_failures.push_back(p_message)
	push_warning("path_tracer_check: " + p_message)


func _info() -> Dictionary:
	return get_viewport().get_path_tracing_info()


func _frame() -> void:
	await RenderingServer.frame_post_draw


func _frames(p_count: int) -> void:
	for i in p_count:
		await RenderingServer.frame_post_draw


func _run() -> void:
	_room = load(_scene_path).instantiate()
	add_child(_room)
	_camera = _room.get_node(_camera_name)
	_camera.current = true
	var world_environment: WorldEnvironment = _room.get_node("WorldEnvironment")
	_environment = world_environment.environment
	# The scenes keep NextGI off in their environments; the harness turns it on the same way.
	_environment.nextgi_enabled = true
	var viewport := get_viewport()
	if _denoiser_arg == "off":
		viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_OFF
	elif _denoiser_arg == "builtin":
		viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_BUILTIN
	elif _denoiser_arg == "oidn":
		viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_OIDN
	viewport.path_tracing_samples = 4096
	viewport.path_tracing_enabled = true
	_result["scene"] = _scene_path

	await _wait_active()
	if _smoke and _failures.is_empty():
		await _check_smoke()
	elif _failures.is_empty():
		await _check_idle()
		await _check_unrelated_materials()
		await _check_events()
		await _check_deformation()
		await _check_target()
		await _check_low_processor()
		await _check_denoiser()
		await _check_cull()
		await _check_projection()
		await _check_hit_materials()
		await _check_bounces_and_clamp()
		await _check_status()
		await _check_quick_restart()
		await _check_resize()
		await _report_reference()
		await _check_reference_match()
		await _check_streaks()
		await _report_direct()
	_finish()


func _wait_active() -> void:
	var backends := {}
	var frames := 0
	while frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
		var info := _info()
		var status := String(info.get("status", ""))
		var backend := String(info.get("backend", ""))
		if status == "ACTIVE":
			backends[backend] = int(backends.get(backend, 0)) + 1
			if backend == _expect_backend:
				break
			if _expect_backend == "INLINE" or frames >= HIT_GROUP_WAIT_FRAMES:
				break
	var info := _info()
	_result["startup_frames"] = frames
	_result["startup_backends"] = backends
	_result["tier"] = String(RenderingServer.nextgi_get_tier_info().get("tier", ""))
	_result["backend"] = String(info.get("backend", ""))
	_result["width"] = int(info.get("width", 0))
	_result["height"] = int(info.get("height", 0))
	if String(info.get("status", "")) != "ACTIVE":
		_fail("path tracing did not become ACTIVE in %d frames (status %s)" % [frames, String(info.get("status", ""))])
		return
	if String(info.get("backend", "")) != _expect_backend:
		_fail("backend %s, expected %s" % [String(info.get("backend", "")), _expect_backend])
	# Output resolution: the history is the window's size.
	var size := viewport_size()
	if int(info.get("width", 0)) != size.x or int(info.get("height", 0)) != size.y:
		_fail("history %dx%d, viewport %dx%d" % [int(info.get("width", 0)), int(info.get("height", 0)), size.x, size.y])


func viewport_size() -> Vector2i:
	return Vector2i(get_viewport().get_visible_rect().size)


func _check_idle() -> void:
	# The negative control: nothing changes, so nothing restarts and every drawn frame adds one sample.
	await _frames(10)
	var before := _info()
	await _frames(IDLE_FRAMES)
	var after := _info()
	var added := int(after.get("samples", 0)) - int(before.get("samples", 0))
	var resets := int(after.get("resets", 0)) - int(before.get("resets", 0))
	_result["idle"] = {"frames": IDLE_FRAMES, "samples_added": added, "resets": resets}
	if resets != 0:
		_fail("idle frames restarted the history %d times (last reasons %s)" % [resets, str(after.get("last_reset_reasons", []))])
	if added != IDLE_FRAMES:
		_fail("idle frames added %d samples over %d frames" % [added, IDLE_FRAMES])


func _check_unrelated_materials() -> void:
	# The negative control for materials: a CanvasItem shader parameter and a 3D material no traced surface draws
	# change on every frame, and nothing restarts the history.
	var layer := CanvasLayer.new()
	var rect := ColorRect.new()
	rect.size = Vector2(8.0, 8.0)
	var canvas_shader := Shader.new()
	canvas_shader.code = "shader_type canvas_item;\nuniform float phase = 0.0;\nvoid fragment() {\n\tCOLOR = vec4(phase, 0.0, 0.0, 1.0);\n}\n"
	var canvas_material := ShaderMaterial.new()
	canvas_material.shader = canvas_shader
	rect.material = canvas_material
	layer.add_child(rect)
	add_child(layer)
	var unused := StandardMaterial3D.new()
	unused.albedo_color = Color(0.5, 0.5, 0.5)
	await _frames(10)
	var before := _info()
	for i in IDLE_FRAMES:
		var phase := float(i + 1) / IDLE_FRAMES
		canvas_material.set_shader_parameter("phase", phase)
		unused.albedo_color = Color(phase, 0.5, 0.5)
		await _frame()
	var after := _info()
	var added := int(after.get("samples", 0)) - int(before.get("samples", 0))
	var resets := int(after.get("resets", 0)) - int(before.get("resets", 0))
	_result["unrelated_materials"] = {"frames": IDLE_FRAMES, "samples_added": added, "resets": resets}
	if resets != 0:
		_fail("unrelated materials restarted the history %d times (last reasons %s)" % [resets, str(after.get("last_reset_reasons", []))])
	if added != IDLE_FRAMES:
		_fail("unrelated materials: %d samples over %d frames" % [added, IDLE_FRAMES])
	layer.queue_free()
	await _frames(2)


# Waits until the history has gone 10 frames without a restart (at most p_frames frames).
func _settle(p_frames: int) -> void:
	var last := _info()
	var quiet := 0
	for i in p_frames:
		await _frame()
		var info := _info()
		if int(info.get("resets", 0)) != int(last.get("resets", 0)):
			quiet = 0
		else:
			quiet += 1
			if quiet >= 10:
				return
		last = info


# One change event: p_change runs before a frame is drawn; that frame must restart the history with one of
# p_reasons. p_followups are the reasons later restarts may give before the history settles (none for most events).
func _event(p_name: String, p_reasons: Array, p_change: Callable, p_followups: Array = []) -> void:
	await _frames(3)
	var before := _info()
	p_change.call()
	await _frame()
	var after := _info()
	var reasons: PackedStringArray = after.get("last_reset_reasons", PackedStringArray())
	var row := {
		"resets": int(after.get("resets", 0)) - int(before.get("resets", 0)),
		"reasons": reasons,
		"samples": int(after.get("samples", 0)),
		"status": String(after.get("status", "")),
	}
	var matched := false
	for reason in p_reasons:
		if reasons.has(reason):
			matched = true
	if row.resets < 1:
		_fail("%s: no restart on the frame of the change (status %s)" % [p_name, row.status])
	elif not matched:
		_fail("%s: restarted for %s, expected one of %s" % [p_name, str(reasons), str(p_reasons)])
	if row.samples > 1:
		_fail("%s: the history holds %d samples after the restart" % [p_name, row.samples])
	# Settled: SETTLE_FRAMES frames at most until a run of 10 frames without a restart; follow-ups only with
	# p_followups reasons.
	var followups := PackedStringArray()
	var last := after
	var quiet := 0
	var frames := 0
	while quiet < 10 and frames < SETTLE_FRAMES:
		await _frame()
		frames += 1
		var info := _info()
		if int(info.get("resets", 0)) != int(last.get("resets", 0)):
			quiet = 0
			for reason in PackedStringArray(info.get("last_reset_reasons", PackedStringArray())):
				followups.push_back(reason)
				if not p_followups.has(reason):
					_fail("%s: restarted again %d frames after the change, for %s" % [p_name, frames, reason])
		else:
			quiet += 1
		last = info
	row["followups"] = followups
	row["settle_frames"] = frames
	if quiet < 10:
		_fail("%s: the history kept restarting for %d frames (%s)" % [p_name, frames, str(followups)])
	_result["events"][p_name] = row


func _check_events() -> void:
	_result["events"] = {}
	var block_small: MeshInstance3D = _room.get_node("BlockSmall")
	var block_large: MeshInstance3D = _room.get_node("BlockLarge")
	var light: OmniLight3D = _room.get_node("RoomLight")
	# Its surfaces' materials enter the traced set when its BLAS enters the TLAS, a frame or more later.
	var later_instance := ["scene", "instances", "hit_groups", "materials"]
	await _event("camera_moved", ["camera"], func(): _camera.position += Vector3(0.0, 0.0, 0.01))
	await _event("camera_fov", ["camera"], func(): _camera.fov = 65.0)
	await _event("instance_moved", ["instances"], func(): block_small.position += Vector3(0.05, 0.0, 0.0))
	var material: StandardMaterial3D = block_large.get_active_material(0).duplicate()
	block_large.material_override = material
	await _frames(4)
	await _event("material_changed", ["materials"], func(): material.albedo_color = Color(0.2, 0.3, 0.9))
	await _check_global_parameter(block_large, material)
	await _event("light_energy", ["lights"], func(): light.light_energy = 1.5)
	await _event("light_moved", ["lights"], func(): light.position += Vector3(0.0, -0.1, 0.0))
	var added := MeshInstance3D.new()
	added.mesh = BoxMesh.new()
	added.position = block_small.position + Vector3(0.0, 0.6, 0.0)
	added.scale = Vector3.ONE * 0.3
	# A hidden or freed instance leaves the TLAS: the NextGI event or the TLAS records (scene) restart the history.
	await _event("instance_added", ["instances"], func(): _room.add_child(added), later_instance)
	await _event("instance_hidden", ["instances", "scene"], func(): added.visible = false, later_instance)
	await _event("instance_freed", ["instances", "scene"], func(): added.queue_free(), later_instance)
	var sun := DirectionalLight3D.new()
	await _event("directional_light_added", ["lights"], func(): _room.add_child(sun))
	await _event("directional_light_energy", ["lights"], func(): sun.light_energy = 0.5)
	await _event("background", ["scene"], func(): _environment.background_color = Color(0.05, 0.05, 0.08))
	await _event("max_bounces", ["settings"], func(): get_viewport().path_tracing_max_bounces = 16)
	await _event("firefly_clamp", ["settings"], func(): get_viewport().path_tracing_firefly_clamp = 100.0)
	get_viewport().path_tracing_firefly_clamp = 0.0
	get_viewport().path_tracing_max_bounces = 32
	sun.queue_free()
	await _frames(4)


func _check_global_parameter(p_mesh: MeshInstance3D, p_restore: Material) -> void:
	# A global shader parameter a traced material reads. Its hit group reads it, so with the hit groups a change
	# restarts the history; the D16 table (inline) has no global parameters, so there it does not.
	RenderingServer.global_shader_parameter_add(GLOBAL_TINT, RenderingServer.GLOBAL_VAR_TYPE_COLOR, Color(0.8, 0.8, 0.8))
	var shader := Shader.new()
	shader.code = "shader_type spatial;\nglobal uniform vec4 %s;\nvoid fragment() {\n\tALBEDO = %s.rgb;\n}\n" % [GLOBAL_TINT, GLOBAL_TINT]
	var tinted := ShaderMaterial.new()
	tinted.shader = shader
	p_mesh.material_override = tinted
	await _settle(HIT_GROUP_WAIT_FRAMES)
	if _expect_backend == "HIT_GROUPS":
		# The hit group may link after the change (its shader compiles); the restart comes from the parameter.
		await _event("global_shader_parameter", ["materials"], func(): RenderingServer.global_shader_parameter_set(GLOBAL_TINT, Color(0.2, 0.9, 0.2)), ["hit_groups"])
	else:
		var before := _info()
		RenderingServer.global_shader_parameter_set(GLOBAL_TINT, Color(0.2, 0.9, 0.2))
		await _frames(3)
		var resets := int(_info().get("resets", 0)) - int(before.get("resets", 0))
		_result["events"]["global_shader_parameter"] = {"resets": resets, "backend": _expect_backend}
		if resets != 0:
			_fail("inline: a global shader parameter restarted the history %d times" % resets)
	p_mesh.material_override = p_restore
	# Free the shader that reads the global parameter before removing the parameter.
	tinted = null
	shader = null
	await _frames(4)
	RenderingServer.global_shader_parameter_remove(GLOBAL_TINT)
	await _settle(SETTLE_FRAMES)


func _check_deformation() -> void:
	# A box deformed in place on every frame: its top face moves down and back up inside the box's AABB (the side
	# faces keep the top corners), so NextGI raises no event. The first updates take the mesh out of the TLAS while
	# its BLAS is built again (the records change; while it stays out, nothing it draws changes); after 4 updates it
	# is dynamic and refits in place with the same records (rt_refit_triangles). The first frame and every refit
	# frame must restart the history, the refit frames for the traced scene.
	var arrays := BoxMesh.new().get_mesh_arrays()
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var deformed := MeshInstance3D.new()
	deformed.mesh = mesh
	var block_small: MeshInstance3D = _room.get_node("BlockSmall")
	deformed.position = block_small.position + Vector3(-0.4, 0.6, 0.0)
	deformed.scale = Vector3.ONE * 0.4
	_room.add_child(deformed)
	await _settle(HIT_GROUP_WAIT_FRAMES)
	var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var rows := []
	for i in DEFORM_FRAMES:
		var moved := positions.duplicate()
		var depth := 0.1 if i % 2 == 0 else 0.25
		for v in moved.size():
			if normals[v].y > 0.5:
				var point := moved[v]
				point.y -= depth
				moved[v] = point
		var before := _info()
		RenderingServer.mesh_surface_update_vertex_region(mesh.get_rid(), 0, 0, moved.to_byte_array())
		await _frame()
		var after := _info()
		var refit := int(RenderingServer.nextgi_get_tier_info().get("rt_refit_triangles", 0)) > 0
		var reasons: PackedStringArray = after.get("last_reset_reasons", PackedStringArray())
		var row := {"resets": int(after.get("resets", 0)) - int(before.get("resets", 0)), "reasons": reasons, "samples": int(after.get("samples", 0)), "status": String(after.get("status", "")), "refit": refit}
		rows.push_back(row)
		if refit and (row.resets < 1 or not reasons.has("scene")):
			_fail("deformation frame %d (refit in place): %d restarts for %s, expected one for scene" % [i, row.resets, str(reasons)])
		elif i == 0 and row.resets < 1:
			_fail("deformation frame 0: no restart (status %s)" % row.status)
		if row.samples > 1:
			_fail("deformation frame %d: the history holds %d samples" % [i, row.samples])
	var refits := 0
	for row in rows:
		if row.refit:
			refits += 1
	_result["deformation"] = {"frames": rows, "refit_frames": refits}
	if refits < DEFORM_MIN_REFITS:
		_fail("deformation: the mesh refit in place on %d frames, expected at least %d" % [refits, DEFORM_MIN_REFITS])
	deformed.queue_free()
	await _settle(SETTLE_FRAMES)


func _check_low_processor() -> void:
	# Low-processor mode draws a frame only when something asks for one, as the editor does: while the history
	# accumulates, the path tracer asks for the next frame; once it has converged, it stops asking.
	var viewport := get_viewport()
	viewport.path_tracing_samples = 4096
	_camera.position += Vector3(0.0, 0.0, 0.001)
	await _frames(3)
	OS.low_processor_usage_mode = true
	var before := _info()
	var drawn_before := Engine.get_frames_drawn()
	for i in LOW_PROCESSOR_FRAMES:
		await get_tree().process_frame
	var accumulating := _info()
	var drawn := Engine.get_frames_drawn() - drawn_before
	var added := int(accumulating.get("samples", 0)) - int(before.get("samples", 0))
	# Converge a few samples from here, then count the frames drawn afterwards.
	viewport.path_tracing_samples = int(accumulating.get("samples", 0)) + 3
	for i in 20:
		await get_tree().process_frame
	var converged := _info()
	var drawn_converged_before := Engine.get_frames_drawn()
	for i in 30:
		await get_tree().process_frame
	var drawn_converged := Engine.get_frames_drawn() - drawn_converged_before
	OS.low_processor_usage_mode = false
	viewport.path_tracing_samples = 4096
	await _frames(3)
	var row := {"process_frames": LOW_PROCESSOR_FRAMES, "frames_drawn": drawn, "samples_added": added, "converged_status": String(converged.get("status", "")), "frames_drawn_converged": drawn_converged}
	_result["low_processor"] = row
	if added < LOW_PROCESSOR_FRAMES - 5:
		_fail("low-processor mode: %d samples over %d process frames (%d drawn)" % [added, LOW_PROCESSOR_FRAMES, drawn])
	if row.converged_status != "CONVERGED":
		_fail("low-processor mode: status %s after the target, expected CONVERGED" % row.converged_status)
	if drawn_converged > 2:
		_fail("low-processor mode: %d frames drawn over 30 process frames after converging" % drawn_converged)


func _check_target() -> void:
	# The target stops the history; raising it continues the same history. A fresh history first (a camera nudge),
	# so it holds fewer samples than the target.
	var viewport := get_viewport()
	_camera.position += Vector3(0.0, 0.0, 0.001)
	await _frames(2)
	var before := _info()
	viewport.path_tracing_samples = 8
	await _frames(20)
	var stopped := _info()
	viewport.path_tracing_samples = 16
	await _frames(20)
	var raised := _info()
	var row := {
		"stopped_samples": int(stopped.get("samples", 0)),
		"stopped_status": String(stopped.get("status", "")),
		"raised_samples": int(raised.get("samples", 0)),
		"resets": int(raised.get("resets", 0)) - int(before.get("resets", 0)),
	}
	_result["target"] = row
	if row.stopped_samples != 8 or row.stopped_status != "CONVERGED":
		_fail("target 8: %d samples, status %s" % [row.stopped_samples, row.stopped_status])
	if row.raised_samples != 16:
		_fail("target raised to 16: %d samples" % row.raised_samples)
	if row.resets != 0:
		_fail("changing the target restarted the history %d times" % row.resets)
	viewport.path_tracing_samples = 4096


# The mean of the history's central tenth (radiance, before exposure), after p_samples samples.
func _center_after(p_samples: int) -> float:
	ProjectSettings.set_setting(SETTING_READBACK, false)
	# A change made before this call restarts the history on the next drawn frame.
	await _frames(2)
	var frames := 0
	while int(_info().get("samples", 0)) < p_samples - 1 and frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frame()
	var info := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)
	var bytes: PackedByteArray = info.get("accumulation", PackedByteArray())
	var width := int(info.get("width", 0))
	var height := int(info.get("height", 0))
	if bytes.size() < width * height * 16 or width == 0:
		_fail("no history readback (%d bytes for %dx%d)" % [bytes.size(), width, height])
		return -1.0
	if int(info.get("nonfinite", 0)) != 0:
		_fail("the history holds %d non-finite values" % int(info.get("nonfinite", 0)))
	var floats := bytes.to_float32_array()
	var sum := 0.0
	var count := 0
	for y in range(int(height * 0.45), int(height * 0.55)):
		for x in range(int(width * 0.45), int(width * 0.55)):
			var i := (y * width + x) * 4
			var weight := floats[i + 3]
			if weight > 0.0:
				sum += (floats[i] + floats[i + 1] + floats[i + 2]) / (3.0 * weight)
				count += 1
	return sum / maxf(count, 1)


func _place_quad(p_cull: BaseMaterial3D.CullMode, p_facing_camera: bool, p_mirrored: bool, p_distance := 0.5) -> void:
	if _quad == null:
		_quad = MeshInstance3D.new()
		var mesh := QuadMesh.new()
		mesh.size = Vector2(0.6, 0.6)
		_quad.mesh = mesh
		_quad_material = StandardMaterial3D.new()
		_quad_material.albedo_color = Color(0.0, 0.0, 0.0)
		_quad_material.emission_enabled = true
		_quad_material.emission = Color(1.0, 1.0, 1.0)
		_quad_material.emission_energy_multiplier = EMISSION_ENERGY
		_quad.material_override = _quad_material
		_room.add_child(_quad)
	_quad_material.cull_mode = p_cull
	# p_distance in front of the camera. A QuadMesh faces +Z; the camera looks along its -Z.
	var basis := _camera.global_transform.basis
	if not p_facing_camera:
		basis = basis.rotated(basis.y.normalized(), PI)
	if p_mirrored:
		basis = basis * Basis.from_scale(Vector3(-1.0, 1.0, 1.0))
	_quad.global_transform = Transform3D(basis, _camera.global_position - _camera.global_transform.basis.z * p_distance)


func _check_cull() -> void:
	var cases := [
		["back_front", BaseMaterial3D.CULL_BACK, true, false, true],
		["back_behind", BaseMaterial3D.CULL_BACK, false, false, false],
		["disabled_behind", BaseMaterial3D.CULL_DISABLED, false, false, true],
		["disabled_front", BaseMaterial3D.CULL_DISABLED, true, false, true],
		["front_front", BaseMaterial3D.CULL_FRONT, true, false, false],
		["front_behind", BaseMaterial3D.CULL_FRONT, false, false, true],
		["mirrored_back_front", BaseMaterial3D.CULL_BACK, true, true, true],
		["mirrored_back_behind", BaseMaterial3D.CULL_BACK, false, true, false],
	]
	var rows := {}
	for case in cases:
		_place_quad(case[1], case[2], case[3])
		# Wait for a restart, then for the samples.
		await _frames(2)
		var value := await _center_after(CULL_SAMPLES)
		var seen: bool = value > BRIGHT
		rows[case[0]] = {"center": value, "seen": seen, "expected": case[4]}
		if value < 0.0:
			continue
		if seen != case[4]:
			_fail("cull %s: center %.3f, expected the quad %s" % [case[0], value, "seen" if case[4] else "culled"])
		elif not seen and value > DARK:
			_fail("cull %s: center %.3f is neither the quad nor the room" % [case[0], value])
	_result["cull"] = rows


# The history (with its readback) after p_samples samples; a change made before this call restarts it first.
func _history_after(p_samples: int) -> Dictionary:
	ProjectSettings.set_setting(SETTING_READBACK, false)
	await _frames(2)
	var frames := 0
	while int(_info().get("samples", 0)) < p_samples - 1 and frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frame()
	var info := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)
	return info


func _check_projection() -> void:
	# Camera rays follow the projection: a bright quad PROJECTION_DISTANCE in front of the camera covers the pixels
	# Camera3D.unproject_position() puts its corners on, for a perspective, an orthogonal and an off-axis frustum
	# camera. The D16 camera block is a symmetric perspective: for the last two, the box it would give (d16) must
	# differ from the camera's, or the case would not tell them apart.
	var saved := {"projection": _camera.projection, "size": _camera.size, "offset": _camera.frustum_offset}
	# Direct lighting only: no bounce ray reaches the quad, so only camera rays that hit it see its emission.
	get_viewport().path_tracing_max_bounces = 0
	var cases := [
		["perspective", Camera3D.PROJECTION_PERSPECTIVE, _camera.size, Vector2()],
		["orthogonal", Camera3D.PROJECTION_ORTHOGONAL, 1.5, Vector2()],
		["frustum", Camera3D.PROJECTION_FRUSTUM, 0.06, Vector2(0.012, -0.008)],
	]
	var rows := {}
	for case in cases:
		_camera.projection = case[1]
		_camera.size = case[2]
		_camera.frustum_offset = case[3]
		_place_quad(BaseMaterial3D.CULL_DISABLED, true, false, PROJECTION_DISTANCE)
		var info := await _history_after(CULL_SAMPLES)
		var measured := _bright_box(info)
		var expected := _quad_screen_box()
		var d16 := _quad_d16_box()
		var row := {"measured": [measured.position.x, measured.position.y, measured.end.x, measured.end.y], "expected": [expected.position.x, expected.position.y, expected.end.x, expected.end.y], "d16": [d16.position.x, d16.position.y, d16.end.x, d16.end.y]}
		row["d16_worst_px"] = _box_distance(d16, expected)
		rows[case[0]] = row
		if case[0] != "perspective" and row.d16_worst_px <= 4.0 * PROJECTION_TOLERANCE_PX:
			_fail("projection %s: the D16 camera block would cover %s, too close to %s to tell them apart" % [case[0], str(row.d16), str(row.expected)])
		if measured.size.x <= 0.0:
			_fail("projection %s: the quad is not in the history" % case[0])
			continue
		var worst := _box_distance(measured, expected)
		row["worst_px"] = worst
		if worst > PROJECTION_TOLERANCE_PX:
			_fail("projection %s: the quad covers %s, the camera projects it to %s" % [case[0], str(row.measured), str(row.expected)])
	_result["projection"] = rows
	_camera.projection = saved.projection
	_camera.size = saved.size
	_camera.frustum_offset = saved.offset
	get_viewport().path_tracing_max_bounces = 32
	await _frames(2)


# The pixel box of the quad in the history: the columns and rows where at least a quarter as many pixels as in the
# fullest one see more than half the quad's emission, from the first one's left or top edge to the last one's right
# or bottom edge (stray bright pixels elsewhere do not count).
func _bright_box(p_info: Dictionary) -> Rect2:
	var bytes: PackedByteArray = p_info.get("accumulation", PackedByteArray())
	var width := int(p_info.get("width", 0))
	var height := int(p_info.get("height", 0))
	if bytes.size() < width * height * 16 or width == 0:
		_fail("no history readback (%d bytes for %dx%d)" % [bytes.size(), width, height])
		return Rect2()
	var floats := bytes.to_float32_array()
	var columns := PackedInt32Array()
	columns.resize(width)
	var rows := PackedInt32Array()
	rows.resize(height)
	var threshold := EMISSION_ENERGY * 0.5
	for y in height:
		for x in width:
			var i := (y * width + x) * 4
			var weight := floats[i + 3]
			if weight > 0.0 and (floats[i] + floats[i + 1] + floats[i + 2]) / (3.0 * weight) > threshold:
				columns[x] += 1
				rows[y] += 1
	var span_x := _count_span(columns)
	var span_y := _count_span(rows)
	if span_x.y <= span_x.x or span_y.y <= span_y.x:
		return Rect2()
	return Rect2(Vector2(span_x.x, span_y.x), Vector2(span_x.y - span_x.x, span_y.y - span_y.x))


# The first and one past the last index whose count is at least a quarter of the largest count.
func _count_span(p_counts: PackedInt32Array) -> Vector2:
	var largest := 0
	for count in p_counts:
		largest = maxi(largest, count)
	if largest == 0:
		return Vector2()
	var first := -1
	var last := -1
	for i in p_counts.size():
		if p_counts[i] * 4 >= largest:
			if first < 0:
				first = i
			last = i
	return Vector2(first, last + 1)


func _box_distance(p_a: Rect2, p_b: Rect2) -> float:
	return maxf(maxf(absf(p_a.position.x - p_b.position.x), absf(p_a.position.y - p_b.position.y)), maxf(absf(p_a.end.x - p_b.end.x), absf(p_a.end.y - p_b.end.y)))


# Where the D16 camera block (a symmetric perspective with the vertical field of view taken from the projection,
# path_tracer_camera()) would put the quad's corners, in viewport pixels.
func _quad_d16_box() -> Rect2:
	var size := Vector2(viewport_size())
	var projection := _camera.get_camera_projection()
	var tan_half := 1.0 / absf(projection.y.y)
	var aspect := size.x / size.y
	var to_camera := _camera.global_transform.affine_inverse()
	var half := (_quad.mesh as QuadMesh).size * 0.5
	var low := Vector2(INF, INF)
	var high := Vector2(-INF, -INF)
	for corner: Vector2 in [Vector2(-1, -1), Vector2(1, -1), Vector2(-1, 1), Vector2(1, 1)]:
		var point := to_camera * (_quad.global_transform * Vector3(corner.x * half.x, corner.y * half.y, 0.0))
		var ndc := Vector2(point.x / (-point.z * tan_half * aspect), point.y / (-point.z * tan_half))
		var screen := Vector2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y)
		low = Vector2(minf(low.x, screen.x), minf(low.y, screen.y))
		high = Vector2(maxf(high.x, screen.x), maxf(high.y, screen.y))
	return Rect2(low, high - low)


# Where the camera projects the quad's corners, in viewport pixels.
func _quad_screen_box() -> Rect2:
	var half := (_quad.mesh as QuadMesh).size * 0.5
	var low := Vector2(INF, INF)
	var high := Vector2(-INF, -INF)
	for corner: Vector2 in [Vector2(-1, -1), Vector2(1, -1), Vector2(-1, 1), Vector2(1, 1)]:
		var point := _quad.global_transform * Vector3(corner.x * half.x, corner.y * half.y, 0.0)
		var screen := _camera.unproject_position(point)
		low = Vector2(minf(low.x, screen.x), minf(low.y, screen.y))
		high = Vector2(maxf(high.x, screen.x), maxf(high.y, screen.y))
	return Rect2(low, high - low)


func _check_hit_materials() -> void:
	# A white emitter with a black emission texture: the hit group multiplies them (dark); the D16 table has no
	# textures and keeps the white emission (bright). Multiply, as Add would keep the emission.
	_place_quad(BaseMaterial3D.CULL_BACK, true, false)
	_quad_material.emission_operator = BaseMaterial3D.EMISSION_OP_MULTIPLY
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 1.0))
	_quad_material.emission_texture = ImageTexture.create_from_image(image)
	var expect_dark := _expect_backend == "HIT_GROUPS"
	var value := -1.0
	var frames := 0
	while frames < HIT_GROUP_WAIT_FRAMES:
		value = await _center_after(CULL_SAMPLES)
		frames += CULL_SAMPLES
		if (value < DARK) == expect_dark:
			break
	var info := _info()
	_result["hit_materials"] = {"center": value, "backend": String(info.get("backend", "")), "frames": frames, "textured_materials": int(info.get("textured_materials", 0))}
	if expect_dark and not value < DARK:
		_fail("hit groups: the emission texture did not shade the hit (center %.3f)" % value)
	if not expect_dark and not value > BRIGHT:
		_fail("inline: the D16 table emission did not shade the hit (center %.3f)" % value)
	_quad_material.emission_texture = null
	_quad_material.emission_operator = BaseMaterial3D.EMISSION_OP_ADD


func _check_bounces_and_clamp() -> void:
	var viewport := get_viewport()
	# The firefly clamp caps what one path adds: the emitter's 50 becomes at most the clamp.
	_place_quad(BaseMaterial3D.CULL_BACK, true, false)
	viewport.path_tracing_firefly_clamp = 2.0
	var exposure := float(_info().get("exposure", 1.0))
	var clamped := await _center_after(CULL_SAMPLES)
	viewport.path_tracing_firefly_clamp = 0.0
	var unclamped := await _center_after(CULL_SAMPLES)
	var row := {"clamped": clamped, "unclamped": unclamped, "exposure": exposure}
	if clamped < 0.0 or clamped > 2.0 / maxf(exposure, 1e-6) * 1.001:
		_fail("firefly clamp 2: center %.3f" % clamped)
	if unclamped < EMISSION_ENERGY * 0.9:
		_fail("no clamp: center %.3f, the emitter is %.1f" % [unclamped, EMISSION_ENERGY])
	_quad.queue_free()
	_quad = null
	await _frames(2)
	# The bounce limit: direct lighting only (0 bounces) is darker than 32 bounces in a closed room.
	viewport.path_tracing_max_bounces = 0
	var direct := await _frame_mean(16)
	viewport.path_tracing_max_bounces = 32
	var full := await _frame_mean(16)
	row["mean_bounces_0"] = direct
	row["mean_bounces_32"] = full
	_result["bounces_and_clamp"] = row
	if not (direct >= 0.0 and full > direct * 1.05):
		_fail("bounces: 0 bounces %.4f, 32 bounces %.4f" % [direct, full])


func _frame_mean(p_samples: int) -> float:
	ProjectSettings.set_setting(SETTING_READBACK, false)
	await _frames(2)
	var frames := 0
	while int(_info().get("samples", 0)) < p_samples - 1 and frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frame()
	var info := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)
	if not info.has("mean"):
		_fail("no history readback")
		return -1.0
	return float(info.get("mean", -1.0))


func _check_status() -> void:
	var rows := {}
	_environment.nextgi_enabled = false
	await _frames(3)
	rows["nextgi_off"] = String(_info().get("status", ""))
	_environment.nextgi_enabled = true
	await _wait_status("ACTIVE", 120)
	var back := _info()
	rows["nextgi_on"] = String(back.get("status", ""))
	rows["nextgi_on_reasons"] = back.get("last_reset_reasons", PackedStringArray())
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 1)
	await _frames(3)
	rows["reference_mode"] = String(_info().get("status", ""))
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 0)
	await _wait_status("ACTIVE", 120)
	get_viewport().path_tracing_enabled = false
	await _frames(3)
	rows["disabled"] = String(_info().get("status", ""))
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_PATH_TRACER
	await _wait_status("ACTIVE", 120)
	rows["debug_draw"] = String(_info().get("status", ""))
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	get_viewport().path_tracing_enabled = true
	await _wait_status("ACTIVE", 120)
	_result["status"] = rows
	if rows.nextgi_off != "NO_NEXTGI":
		_fail("NextGI off in the environment: status %s" % rows.nextgi_off)
	if rows.nextgi_on != "ACTIVE" or not PackedStringArray(rows.nextgi_on_reasons).has("history"):
		_fail("NextGI back on: status %s, reasons %s" % [rows.nextgi_on, str(rows.nextgi_on_reasons)])
	if rows.reference_mode != "REFERENCE_MODE":
		_fail("reference_mode on: status %s" % rows.reference_mode)
	if rows.disabled != "OFF":
		_fail("path tracing off: status %s" % rows.disabled)
	if rows.debug_draw != "ACTIVE":
		_fail("the path tracer debug draw: status %s" % rows.debug_draw)


func _check_quick_restart() -> void:
	# Off for 2 frames: the hit groups are freed on the second (their consumer leaves a frame after the last traced
	# frame), and path tracing comes back while the device still holds their tables (NEXTGI_HIT_RELEASING).
	var viewport := get_viewport()
	var hit_groups := PackedStringArray()
	viewport.path_tracing_enabled = false
	for i in 2:
		await _frame()
		hit_groups.push_back(String(RenderingServer.nextgi_get_tier_info().get("hit_groups", "")))
	viewport.path_tracing_enabled = true
	var backends := {}
	var frames := 0
	while frames < HIT_GROUP_WAIT_FRAMES:
		await _frame()
		frames += 1
		hit_groups.push_back(String(RenderingServer.nextgi_get_tier_info().get("hit_groups", "")))
		var info := _info()
		if String(info.get("status", "")) == "ACTIVE":
			var backend := String(info.get("backend", ""))
			backends[backend] = int(backends.get(backend, 0)) + 1
			if backend == _expect_backend:
				break
	# The hit groups' state on each frame from turning path tracing off: NO_CONSUMER once they are freed, then
	# RELEASING while the device still holds their tables, then AVAILABLE.
	var row := {"frames": frames, "backends": backends, "backend": String(_info().get("backend", "")), "hit_groups": hit_groups}
	_result["quick_restart"] = row
	if row.backend != _expect_backend:
		_fail("quick restart: backend %s after %d frames (hit groups %s), expected %s" % [row.backend, frames, str(hit_groups), _expect_backend])
	if _expect_backend == "HIT_GROUPS" and not (hit_groups.has("NO_CONSUMER") and hit_groups.has("RELEASING")):
		_fail("quick restart: the hit groups were not freed and waited for (%s), so the restart was not exercised" % str(hit_groups))


func _check_resize() -> void:
	# A second view, a SubViewport with its own camera, path traced and resized while it accumulates: its history
	# restarts at the new size.
	var sub := SubViewport.new()
	sub.size = Vector2i(320, 180)
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	var camera := Camera3D.new()
	camera.fov = _camera.fov
	sub.add_child(camera)
	add_child(sub)
	camera.global_transform = _camera.global_transform
	camera.current = true
	sub.path_tracing_samples = 4096
	sub.path_tracing_enabled = true
	var first := {}
	for i in 240:
		await _frame()
		first = sub.get_path_tracing_info()
		if String(first.get("status", "")) == "ACTIVE" and int(first.get("samples", 0)) >= 4:
			break
	sub.size = Vector2i(400, 240)
	var resized := {}
	for i in 60:
		await _frame()
		resized = sub.get_path_tracing_info()
		if int(resized.get("width", 0)) == 400 and int(resized.get("samples", 0)) >= 2:
			break
	var counts: Dictionary = resized.get("reset_counts", {})
	var row := {
		"first": {"status": String(first.get("status", "")), "width": int(first.get("width", 0)), "height": int(first.get("height", 0)), "samples": int(first.get("samples", 0))},
		"resized": {"status": String(resized.get("status", "")), "width": int(resized.get("width", 0)), "height": int(resized.get("height", 0)), "samples": int(resized.get("samples", 0)), "resets": int(resized.get("resets", 0)) - int(first.get("resets", 0)), "history_resets": int(counts.get("history", 0))},
	}
	_result["resize"] = row
	if row.first.status != "ACTIVE" or row.first.width != 320 or row.first.height != 180:
		_fail("resize: the SubViewport's path tracer is %s at %dx%d, expected ACTIVE at 320x180" % [row.first.status, row.first.width, row.first.height])
	elif row.resized.width != 400 or row.resized.height != 240 or row.resized.resets < 1 or row.resized.samples < 2:
		_fail("resize: after the resize %dx%d, %d restarts, %d samples (status %s)" % [row.resized.width, row.resized.height, row.resized.resets, row.resized.samples, row.resized.status])
	sub.path_tracing_enabled = false
	sub.queue_free()
	await _settle(SETTLE_FRAMES)


func _wait_status(p_status: String, p_frames: int) -> void:
	for i in p_frames:
		await _frame()
		if String(_info().get("status", "")) == p_status:
			return


func _report_reference() -> void:
	# Report only: the product (63 bounces, no clamp) against the D16 reference at the same sample count.
	var viewport := get_viewport()
	viewport.path_tracing_max_bounces = 63
	viewport.path_tracing_samples = REFERENCE_SAMPLES
	var product := await _frame_mean(REFERENCE_SAMPLES)
	viewport.path_tracing_enabled = false
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, true)
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 1)
	var reference := -1.0
	for i in MAX_WAIT_FRAMES:
		await _frame()
		var tier := RenderingServer.nextgi_get_tier_info()
		if int(tier.get("reference_samples", 0)) >= REFERENCE_SAMPLES:
			reference = float(tier.get("reference_mean", -1.0))
			break
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 0)
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, false)
	_result["reference_ratio"] = {
		"samples": REFERENCE_SAMPLES,
		"product_mean": product,
		"reference_mean": reference,
		"ratio": product / reference if reference > 0.0 else -1.0,
		"note": "report only; the product jitters within the pixel, culls one-sided back faces and shades linear colors, the D16 reference does neither and takes the color parameters' raw values; GP Q1 is measured in PQ (#170)",
	}


func _check_smoke() -> void:
	# The smoke render: the history fills to SMOKE_SAMPLES, finite and not black (and denoised when denoiser is on).
	get_viewport().path_tracing_samples = SMOKE_SAMPLES
	var mean := await _frame_mean(SMOKE_SAMPLES)
	var info := _info()
	var tier := RenderingServer.nextgi_get_tier_info()
	var nextgeo_rt := String(tier.get("nextgeo_ray_tracing_mode", ""))
	_result["smoke"] = {
		"samples": int(info.get("samples", 0)),
		"status": String(info.get("status", "")),
		"mean": mean,
		"nonfinite": int(info.get("nonfinite", -1)),
		"resets": int(info.get("resets", 0)),
		"denoiser": String(info.get("denoiser", "")),
		"denoise_status": String(info.get("denoise_status", "")),
		"denoise_backend": String(info.get("denoise_backend", "")),
		"denoised": bool(info.get("denoised", false)),
		"denoised_mean": float(info.get("denoised_mean", -1.0)),
		"denoised_nonfinite": int(info.get("denoised_nonfinite", -1)),
		"expect_black": _expect_black,
		"nextgeo_ray_tracing_mode": nextgeo_rt,
	}
	# What the native mode put in the TLAS (#156's tier info keys), reported as they are.
	for key in tier:
		if String(key).begins_with("rt_native_"):
			_result["smoke"][key] = tier[key]
	if int(info.get("nonfinite", -1)) != 0:
		_fail("smoke: the history holds %d non-finite values" % int(info.get("nonfinite", -1)))
	if get_viewport().path_tracing_denoiser != Viewport.PATH_TRACING_DENOISER_OFF:
		if not bool(info.get("denoised", false)):
			_fail("smoke: denoiser did not run on convergence (status %s)" % String(info.get("denoise_status", "")))
		elif int(info.get("denoised_nonfinite", -1)) != 0:
			_fail("smoke: denoised image holds %d non-finite values" % int(info.get("denoised_nonfinite", -1)))
	if _expect_black:
		# Radiance is never negative, so a zero mean is an all-zero history.
		if mean != 0.0:
			_fail("smoke: the history's mean is %.6f, expected 0 (--expect-black)" % mean)
	elif not mean > 0.0:
		_fail("smoke: the history's mean is %.6f" % mean)
	if not _nextgeo_rt.is_empty() and nextgeo_rt != _nextgeo_rt:
		_fail("smoke: NextGI reports NextGeo ray tracing mode %s, expected %s" % [nextgeo_rt, _nextgeo_rt])
	if int(info.get("samples", 0)) < SMOKE_SAMPLES - 1:
		_fail("smoke: %d samples, expected %d" % [int(info.get("samples", 0)), SMOKE_SAMPLES])


# Mean high-frequency Laplacian energy in tone-mapped perceptual space (c*e / (1 + c*e)).
func _spatial_noise_metric(p_rgba: PackedFloat32Array, p_width: int, p_height: int, p_exposure: float) -> float:
	if p_width < 4 or p_height < 4 or p_rgba.size() < p_width * p_height * 4:
		return -1.0
	var lum := PackedFloat32Array()
	lum.resize(p_width * p_height)
	var exp_val := maxf(p_exposure, 1e-6)
	for i in p_width * p_height:
		var o := i * 4
		var w := p_rgba[o + 3]
		var mean := 0.0
		if w > 0.0:
			var r := maxf(p_rgba[o + 0] / w, 0.0) * exp_val
			var g := maxf(p_rgba[o + 1] / w, 0.0) * exp_val
			var b := maxf(p_rgba[o + 2] / w, 0.0) * exp_val
			r = r / (1.0 + r)
			g = g / (1.0 + g)
			b = b / (1.0 + b)
			mean = (r + g + b) / 3.0
		lum[i] = mean
	var sum := 0.0
	var count := 0
	for y in range(1, p_height - 1):
		var row := y * p_width
		for x in range(1, p_width - 1):
			var idx := row + x
			var avg := 0.25 * (lum[idx - 1] + lum[idx + 1] + lum[idx - p_width] + lum[idx + p_width])
			sum += absf(lum[idx] - avg)
			count += 1
	return sum / float(maxi(count, 1))


func _check_denoiser() -> void:
	# Path tracer denoising (#168): built-in screen-space JNLM runs on convergence or on demand, preserves
	# history across mode toggles, reduces noise vs noisy accumulation, and OIDN falls back cleanly when disabled.
	var viewport := get_viewport()
	viewport.path_tracing_max_bounces = 32
	viewport.path_tracing_firefly_clamp = 0.0
	viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_BUILTIN
	viewport.path_tracing_samples = 64
	_camera.position += Vector3(0.0, 0.0, 0.001)
	await _frames(6)

	# 1. While accumulating: PENDING, not yet denoised.
	var pending := _info()
	var runs_before := int(pending.get("denoise_runs", 0))
	if String(pending.get("denoise_status", "")) != "PENDING" or bool(pending.get("denoised", false)):
		_fail("denoiser pending: status %s, denoised %s at %d/%d samples" % [String(pending.get("denoise_status", "")), str(pending.get("denoised", false)), int(pending.get("samples", 0)), int(pending.get("target_samples", 0))])

	# 2. On-demand denoise mid-accumulation: runs for one frame, valid & finite.
	ProjectSettings.set_setting(SETTING_READBACK, true)
	viewport.request_path_tracing_denoise()
	await _frame()
	var on_demand := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)
	if not bool(on_demand.get("denoised", false)) or String(on_demand.get("denoise_status", "")) != "DENOISED":
		_fail("denoiser on-demand: status %s, denoised %s" % [String(on_demand.get("denoise_status", "")), str(on_demand.get("denoised", false))])
	elif int(on_demand.get("denoise_runs", 0)) != runs_before + 1:
		_fail("denoiser on-demand: runs %d, expected %d" % [int(on_demand.get("denoise_runs", 0)), runs_before + 1])
	elif int(on_demand.get("denoised_nonfinite", -1)) != 0:
		_fail("denoiser on-demand: %d non-finite values" % int(on_demand.get("denoised_nonfinite", -1)))

	# Next accumulating frame returns to PENDING without extra denoise runs.
	await _frame()
	var after_demand := _info()
	if String(after_demand.get("denoise_status", "")) != "PENDING" or bool(after_demand.get("denoised", false)):
		_fail("denoiser after on-demand: status %s, denoised %s" % [String(after_demand.get("denoise_status", "")), str(after_demand.get("denoised", false))])

	# 3. Converge at 32 samples: automatic one-shot denoise, noise reduction & mean preservation.
	var denoise_target := 32
	viewport.path_tracing_samples = denoise_target
	_camera.position -= Vector3(0.0, 0.0, 0.001)
	await _frames(2)
	var frames := 0
	while int(_info().get("samples", 0)) < denoise_target - 1 and frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frame()
	var converged := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)

	var width := int(converged.get("width", 0))
	var height := int(converged.get("height", 0))
	var exposure := float(converged.get("exposure", 1.0))
	var raw_mean := float(converged.get("mean", -1.0))
	var den_mean := float(converged.get("denoised_mean", -1.0))
	var raw_bytes: PackedByteArray = converged.get("accumulation", PackedByteArray())
	var den_bytes: PackedByteArray = converged.get("denoised_image", PackedByteArray())
	var raw_noise := _spatial_noise_metric(raw_bytes.to_float32_array(), width, height, exposure)
	var den_noise := _spatial_noise_metric(den_bytes.to_float32_array(), width, height, exposure)
	var noise_ratio := den_noise / raw_noise if raw_noise > 0.0 else -1.0

	# Idle converged frames must not re-run the denoiser.
	var runs_at_converge := int(converged.get("denoise_runs", 0))
	await _frames(5)
	var after_idle := _info()
	var runs_after_idle := int(after_idle.get("denoise_runs", 0))

	# 4. Mode toggles while converged preserve the history (0 resets).
	var resets_before_toggle := int(after_idle.get("resets", 0))
	viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_OFF
	await _frames(2)
	var toggled_off := _info()
	viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_BUILTIN
	await _frames(2)
	var toggled_builtin := _info()

	# 5. OIDN mode: uses OIDN if compiled in, else falls back cleanly to BUILTIN.
	viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_OIDN
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frames(2)
	var oidn_info := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)

	var oidn_available := bool(oidn_info.get("oidn_available", false))
	var row := {
		"samples": int(converged.get("samples", 0)),
		"status": String(converged.get("status", "")),
		"denoise_status": String(converged.get("denoise_status", "")),
		"denoise_backend": String(converged.get("denoise_backend", "")),
		"denoised": bool(converged.get("denoised", false)),
		"raw_mean": raw_mean,
		"denoised_mean": den_mean,
		"denoised_nonfinite": int(converged.get("denoised_nonfinite", -1)),
		"raw_noise": raw_noise,
		"denoised_noise": den_noise,
		"noise_ratio": noise_ratio,
		"runs_at_converge": runs_at_converge,
		"runs_after_idle": runs_after_idle,
		"toggle_resets": int(oidn_info.get("resets", 0)) - resets_before_toggle,
		"oidn_available": oidn_available,
		"oidn_backend": String(oidn_info.get("oidn_backend", "")),
		"oidn_status": String(oidn_info.get("denoise_status", "")),
		"oidn_active_backend": String(oidn_info.get("denoise_backend", "")),
		"oidn_nonfinite": int(oidn_info.get("denoised_nonfinite", -1)),
	}
	_result["denoiser"] = row

	if row.status != "CONVERGED" or not row.denoised or row.denoise_status != "DENOISED" or row.denoise_backend != "BUILTIN":
		_fail("denoiser converge: status=%s denoised=%s denoise_status=%s backend=%s" % [row.status, str(row.denoised), row.denoise_status, row.denoise_backend])
	if row.denoised_nonfinite != 0:
		_fail("denoiser converge: %d non-finite values" % row.denoised_nonfinite)
	if not (raw_mean > 0.0 and den_mean > 0.0 and absf(den_mean - raw_mean) / raw_mean <= 0.15):
		_fail("denoiser mean drift: raw %.4f, denoised %.4f" % [raw_mean, den_mean])
	if not (noise_ratio > 0.0 and noise_ratio < 0.70):
		_fail("denoiser noise reduction: ratio %.4f (raw %.5f, denoised %.5f), expected < 0.70" % [noise_ratio, raw_noise, den_noise])
	if runs_after_idle != runs_at_converge:
		_fail("denoiser re-ran on idle converged frames (%d -> %d)" % [runs_at_converge, runs_after_idle])
	if bool(toggled_off.get("denoised", true)) or String(toggled_off.get("denoise_status", "")) != "OFF":
		_fail("denoiser toggle off: denoised=%s status=%s" % [str(toggled_off.get("denoised", true)), String(toggled_off.get("denoise_status", ""))])
	if not bool(toggled_builtin.get("denoised", false)) or String(toggled_builtin.get("denoise_status", "")) != "DENOISED":
		_fail("denoiser toggle builtin: denoised=%s status=%s" % [str(toggled_builtin.get("denoised", false)), String(toggled_builtin.get("denoise_status", ""))])
	if row.toggle_resets != 0:
		_fail("denoiser toggles restarted the history %d times" % row.toggle_resets)
	if row.oidn_nonfinite != 0 or not bool(oidn_info.get("denoised", false)):
		_fail("denoiser OIDN: denoised=%s nonfinite=%d" % [str(oidn_info.get("denoised", false)), row.oidn_nonfinite])
	if oidn_available:
		if row.oidn_status != "DENOISED" or row.oidn_active_backend != "OIDN":
			_fail("denoiser OIDN enabled: status=%s backend=%s, expected DENOISED/OIDN" % [row.oidn_status, row.oidn_active_backend])
	else:
		if row.oidn_status != "FALLBACK_BUILTIN" or row.oidn_active_backend != "BUILTIN":
			_fail("denoiser OIDN fallback: status=%s backend=%s, expected FALLBACK_BUILTIN/BUILTIN" % [row.oidn_status, row.oidn_active_backend])

	viewport.path_tracing_denoiser = Viewport.PATH_TRACING_DENOISER_BUILTIN
	viewport.path_tracing_samples = 4096
	await _frames(2)


func _check_reference_match() -> void:
	# The product with the D16 reference's estimator inputs against the reference itself, per pixel.
	var viewport := get_viewport()
	ProjectSettings.set_setting(SETTING_REFERENCE_SEED, 1)
	ProjectSettings.set_setting(SETTING_REFERENCE_MATCH, true)
	viewport.path_tracing_max_bounces = 63
	viewport.path_tracing_firefly_clamp = 0.0
	viewport.path_tracing_samples = MATCH_SAMPLES
	viewport.path_tracing_enabled = true
	ProjectSettings.set_setting(SETTING_READBACK, false)
	await _frames(3)
	var frames := 0
	while (int(_info().get("samples", 0)) < MATCH_SAMPLES - 1 or not bool(_info().get("reference_match", false))) and frames < MAX_WAIT_FRAMES:
		await _frame()
		frames += 1
	ProjectSettings.set_setting(SETTING_READBACK, true)
	await _frame()
	var product := _info()
	ProjectSettings.set_setting(SETTING_READBACK, false)
	viewport.path_tracing_enabled = false
	ProjectSettings.set_setting(SETTING_REFERENCE_MATCH, false)
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, true)
	# A new epoch: the reference starts from zero samples (it kept _report_reference()'s history).
	ProjectSettings.set_setting(SETTING_REFERENCE_EPOCH, int(ProjectSettings.get_setting(SETTING_REFERENCE_EPOCH, 0)) + 1)
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 1)
	var reference := {}
	for i in MAX_WAIT_FRAMES:
		await _frame()
		var tier := RenderingServer.nextgi_get_tier_info()
		if int(tier.get("reference_samples", 0)) >= MATCH_SAMPLES:
			reference = tier
			break
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 0)
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, false)
	var row := {
		"samples": int(product.get("samples", 0)),
		"backend": String(product.get("backend", "")),
		"reference_samples": int(reference.get("reference_samples", 0)),
	}
	_result["reference_match"] = row
	if row.backend != "INLINE" or row.samples != MATCH_SAMPLES:
		_fail("reference match: the product traced %d samples with %s, expected %d with INLINE" % [row.samples, row.backend, MATCH_SAMPLES])
		return
	if row.reference_samples != MATCH_SAMPLES:
		_fail("reference match: the reference holds %d samples, expected %d" % [row.reference_samples, MATCH_SAMPLES])
		return
	var width := int(product.get("width", 0))
	var height := int(product.get("height", 0))
	if width != int(reference.get("reference_width", 0)) or height != int(reference.get("reference_height", 0)):
		_fail("reference match: product %dx%d, reference %dx%d" % [width, height, int(reference.get("reference_width", 0)), int(reference.get("reference_height", 0))])
		return
	var product_bytes: PackedByteArray = product.get("accumulation", PackedByteArray())
	var reference_bytes: PackedByteArray = reference.get("reference_accumulation", PackedByteArray())
	if product_bytes.size() < width * height * 16 or reference_bytes.size() < width * height * 16:
		_fail("reference match: no readback (%d and %d bytes)" % [product_bytes.size(), reference_bytes.size()])
		return
	var a := product_bytes.to_float32_array()
	var b := reference_bytes.to_float32_array()
	var exact := 0
	var close := 0
	var worst := 0.0
	var product_sum := 0.0
	var reference_sum := 0.0
	var pixels := width * height
	for i in pixels:
		var o := i * 4
		var same := true
		var pixel_close := true
		for c in 3:
			var pa := a[o + c] / maxf(a[o + 3], 1.0)
			var pb := b[o + c] / maxf(b[o + 3], 1.0)
			product_sum += pa
			reference_sum += pb
			if a[o + c] != b[o + c]:
				same = false
			var difference := absf(pa - pb) / maxf(absf(pb), 1e-3)
			worst = maxf(worst, difference)
			if difference > MATCH_TOLERANCE:
				pixel_close = false
		if same:
			exact += 1
		if pixel_close:
			close += 1
	row["pixels"] = pixels
	row["exact_fraction"] = float(exact) / pixels
	row["close_fraction"] = float(close) / pixels
	row["tolerance"] = MATCH_TOLERANCE
	row["worst_relative"] = worst
	row["product_mean"] = product_sum / (3.0 * pixels)
	row["reference_mean"] = reference_sum / (3.0 * pixels)
	# The D16 seeding: the positive control of the streaks line.
	row["streak_ratio"] = _streak_ratio(a, width, height, MATCH_SAMPLES)
	_result["reference_match"] = row
	# The same integrator, the same inputs: every value equal (close_fraction and worst_relative are reported).
	if row.exact_fraction != 1.0:
		_fail("reference match: %.6f of the pixels equal the reference's exactly (%.6f within %.0e), expected all" % [row.exact_fraction, row.close_fraction, MATCH_TOLERANCE])


func _check_streaks() -> void:
	# The product's random numbers: after MATCH_SAMPLES samples, neighbors across a MATCH_SAMPLES-wide block
	# boundary differ as much as neighbors inside a block. With the D16 seeding (seed ^ pixel ^ sample), every
	# pixel of an aligned block draws the same MATCH_SAMPLES streams, so neighbors inside a block differ much less.
	var viewport := get_viewport()
	viewport.path_tracing_max_bounces = 32
	viewport.path_tracing_firefly_clamp = 0.0
	viewport.path_tracing_samples = MATCH_SAMPLES
	viewport.path_tracing_enabled = true
	var info := await _history_after(MATCH_SAMPLES)
	viewport.path_tracing_enabled = false
	var bytes: PackedByteArray = info.get("accumulation", PackedByteArray())
	var width := int(info.get("width", 0))
	var height := int(info.get("height", 0))
	var row := {"samples": int(info.get("samples", 0)), "block": MATCH_SAMPLES, "backend": String(info.get("backend", "")), "max_ratio": STREAK_MAX_RATIO}
	row["reference_match_ratio"] = float(_result.get("reference_match", {}).get("streak_ratio", -1.0))
	if bytes.size() < width * height * 16 or width == 0:
		_fail("streaks: no history readback")
		_result["streaks"] = row
		return
	row["product_ratio"] = _streak_ratio(bytes.to_float32_array(), width, height, MATCH_SAMPLES)
	_result["streaks"] = row
	if row.samples != MATCH_SAMPLES or not (row.product_ratio > 0.0 and row.product_ratio <= STREAK_MAX_RATIO):
		_fail("streaks: product ratio %.3f after %d samples, expected at most %.2f (the reference match gives %.3f)" % [row.product_ratio, row.samples, STREAK_MAX_RATIO, row.reference_match_ratio])


# The mean luminance difference of horizontal neighbors that straddle a p_block-wide pixel block boundary, over
# that of neighbors inside a block.
func _streak_ratio(p_floats: PackedFloat32Array, p_width: int, p_height: int, p_block: int) -> float:
	var across := 0.0
	var across_count := 0
	var inside := 0.0
	var inside_count := 0
	for y in p_height:
		var row_start := y * p_width
		var previous := -1.0
		for x in p_width:
			var o := (row_start + x) * 4
			var value := (p_floats[o] + p_floats[o + 1] + p_floats[o + 2]) / (3.0 * maxf(p_floats[o + 3], 1.0))
			if x > 0:
				var difference := absf(value - previous)
				if x % p_block == 0:
					across += difference
					across_count += 1
				else:
					inside += difference
					inside_count += 1
			previous = value
	if across_count == 0 or inside_count == 0 or inside <= 0.0:
		return -1.0
	return (across / across_count) / (inside / inside_count)


func _report_direct() -> void:
	# Report only: direct lighting (0 bounces) against the reference's Direct Only mode, with the room's
	# materials and with every non-emissive surface white (1, 1, 1), where the hit groups' linear albedo and
	# the D16 table's parameter value are the same number.
	var row := {"samples": DIRECT_SAMPLES}
	row["room_product"] = await _product_mean(0, DIRECT_SAMPLES)
	row["room_reference"] = await _reference_mean(2, DIRECT_SAMPLES)
	var white := StandardMaterial3D.new()
	white.albedo_color = Color(1, 1, 1)
	var whitened: Array[MeshInstance3D] = []
	for node in _room.find_children("*", "MeshInstance3D", true, false):
		var mesh_instance := node as MeshInstance3D
		var emissive := false
		for surface in mesh_instance.get_surface_override_material_count():
			var material := mesh_instance.get_active_material(surface) as BaseMaterial3D
			if material != null and material.emission_enabled:
				emissive = true
		if not emissive and mesh_instance.material_override == null:
			mesh_instance.material_override = white
			whitened.push_back(mesh_instance)
	row["white_surfaces"] = whitened.size()
	row["white_product"] = await _product_mean(0, DIRECT_SAMPLES)
	row["white_reference"] = await _reference_mean(2, DIRECT_SAMPLES)
	for mesh_instance in whitened:
		mesh_instance.material_override = null
	for key in ["room", "white"]:
		var reference := float(row[key + "_reference"])
		row[key + "_ratio"] = float(row[key + "_product"]) / reference if reference > 0.0 else -1.0
	row["note"] = "report only; the product jitters within the pixel, culls one-sided back faces and shades linear colors, the D16 reference does neither and takes the color parameters' raw values"
	_result["direct_ratio"] = row


func _product_mean(p_bounces: int, p_samples: int) -> float:
	var viewport := get_viewport()
	viewport.path_tracing_max_bounces = p_bounces
	viewport.path_tracing_samples = p_samples
	viewport.path_tracing_enabled = true
	var mean := await _frame_mean(p_samples)
	viewport.path_tracing_enabled = false
	return mean


func _reference_mean(p_mode: int, p_samples: int) -> float:
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, true)
	# A new epoch: the reference starts from zero samples.
	ProjectSettings.set_setting(SETTING_REFERENCE_EPOCH, int(ProjectSettings.get_setting(SETTING_REFERENCE_EPOCH, 0)) + 1)
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, p_mode)
	var mean := -1.0
	for i in MAX_WAIT_FRAMES:
		await _frame()
		var tier := RenderingServer.nextgi_get_tier_info()
		if int(tier.get("reference_samples", 0)) >= p_samples:
			mean = float(tier.get("reference_mean", -1.0))
			break
	ProjectSettings.set_setting(SETTING_REFERENCE_MODE, 0)
	ProjectSettings.set_setting(SETTING_REFERENCE_READBACK, false)
	return mean


func _finish() -> void:
	_result["test"] = "path_tracer_check"
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
