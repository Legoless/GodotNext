extends Node

## #186 step 4: the Hi-Z screen march of both tiers stops at the first sample
## that is behind the depth buffer by more than the hit thickness (10% of the
## scene depth), or where the ray leaves the screen. The world trace resumes
## at the last sample before that, pulled back toward the origin by 8 cm, or
## by one software-tier voxel when that is larger.
##
## Layout. The camera looks down -Z at a small mirror left of its axis. The
## mirror is tilted so the reflection runs sideways (+X) across the screen and
## leaves at the right edge, inside the 1.5 m screen range. A thin dark wall
## stands across that ray, facing it, close to the camera axis and seen at a
## grazing angle: it is fully on screen, does not cover the mirror, and the
## ray crosses it at about a third of the way to the screen exit. The first
## march sample past the wall is behind the depth buffer by more than the
## thickness, so the march skips the wall's own hit band. A red emissive panel
## and its light sit beyond the screen exit along the ray, off screen.
## Without the stop, the hardware tier keeps marching to the screen exit and
## resumes there, past the wall, and the mirror shows the red. With the stop
## and the pull-back the world trace starts before the wall and hits it, on the
## hardware tier, the software tier and under debug/deny_ray_query. The check
## reads that ray twice at the mirror's center: its color from
## DEBUG_DRAW_NEXTGI_REFLECTION and its hit distance from
## DEBUG_DRAW_NEXTGI_SPECULAR_HIT, which must be the wall's on every tier. On
## the software tier the verification readback (debug/swrt_trace_readback) must
## also stop on the wall.
##
## A control mirror with the same tilt sits above the wall's top edge. Its ray
## runs over the wall, so its hit distance must be past the wall on every tier,
## its reflection must be the red panel on the hardware tier (the software tier
## shades the off-screen panel dark, so its color proves nothing there), and the
## software readback must show its rays passing the wall. That proves a ray the
## wall does not stop is seen to pass, so the wall's mirror cannot pass the
## check for another reason.

const VIEW := Vector2i(960, 540)
const SCREEN_RANGE_M := 1.5
const CAMERA_POSITION := Vector3(0.0, 1.5, 1.7)
## Far enough from the wall that the software tier's 15 cm origin offset leaves
## its rays a clear run to it (a ray that starts within about 5 cm of a thin
## surface skips it).
const MIRROR_POSITION := Vector3(-0.375, 1.5, 0.0)
## Shorter than the wall, so every mirror ray crosses it.
const MIRROR_SIZE := Vector2(0.4, 0.24)
## Offset of the hardware tier's ray origin from the mirror.
const ORIGIN_OFFSET_M := 0.019
## The wall's center. The ray runs across the view, so the 24 march samples are
## close to 6.25 cm apart along it, and the hardware origin offset (0 to 5 cm
## along the mirror normal) shifts them by up to 3.8 cm in X. With the wall's
## front face at 6.25 cm, its hit band (10% of the depth, about 0.7 cm) stays
## clear of every sample position, and the first sample past it is behind the
## depth buffer by more than the band.
const WALL_X := 0.07
const WALL_THICKNESS := 0.015
const WALL_Z_FROM := -0.3
const WALL_Z_TO := 0.9
const WALL_HEIGHT := 0.3
const RED_PAST_EXIT := 0.3
const WARMUP_FRAMES := 180
const SAMPLE_FRAMES := 8
const MAX_FRAMES := 700
const RED_LIMIT := 0.2
## The control mirror: above the wall's top edge (0.15 m over the mirror ray) by
## 0.15 m, and inside the red panel's height.
const CONTROL_RISE := 0.30
const CONTROL_SIZE := Vector2(0.4, 0.08)
## The control reflection is the red panel on the hardware tier: far above RED_LIMIT.
const RED_MIN := 1.0
## Frames after the switch to the hit-distance view before it is sampled.
const HIT_SETTLE_FRAMES := 12
## A hit nearer than this is on the mirror itself.
const MIN_HIT_M := 0.1
## The hit-distance view's value for a miss, and the distance reported for one.
const HIT_VIEW_MISS := 0.999
const MISS_DISTANCE_M := 65504.0
## Software readback rays that must stop on the wall, and control rays that must pass it.
const MIN_WALL_RAYS := 32
const MIN_CONTROL_RAYS := 8
const HIT_COVERAGE := 64
const HIT_SURFACE := 1
const HIT_SCREEN := 4
const HIT_SKIPPED := 128

var _expected_tier := ""
var _viewport: SubViewport
var _camera: Camera3D
var _root: Node3D
var _mirror_point := Vector3.ZERO
var _mirror_normal := Vector3.FORWARD
var _direction := Vector3.ZERO
var _wall_t := 0.0
var _red_t := 0.0
var _wall_center := Vector3.ZERO
var _red_center := Vector3.ZERO
var _frames := 0
var _samples: PackedFloat32Array = PackedFloat32Array()
var _control_samples: PackedFloat32Array = PackedFloat32Array()
var _hit_samples: PackedFloat32Array = PackedFloat32Array()
var _control_hit_samples: PackedFloat32Array = PackedFloat32Array()
var _hit_view_frame := -1
var _control_point := Vector3.ZERO
var _done := false
var _placed := false


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if str(arg).begins_with("--expected-tier="):
			_expected_tier = str(arg).trim_prefix("--expected-tier=")
	_viewport = SubViewport.new()
	_viewport.size = VIEW
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.use_hdr_2d = true
	_viewport.msaa_3d = Viewport.MSAA_DISABLED
	_viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	_viewport.use_taa = false
	_viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_REFLECTION
	add_child(_viewport)
	_root = Node3D.new()
	_viewport.add_child(_root)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.05, 0.2)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.BLACK
	environment.ambient_light_energy = 0.0
	environment.ssil_enabled = false
	environment.ssao_enabled = false
	environment.ssr_enabled = false
	environment.nextgi_enabled = true
	environment.nextgi_max_roughness_to_trace = 0.4
	var world := WorldEnvironment.new()
	world.environment = environment
	_root.add_child(world)
	_camera = Camera3D.new()
	_camera.fov = 28.0
	_root.add_child(_camera)
	_camera.current = true


func _process(_delta: float) -> void:
	if _done:
		return
	_frames += 1
	if not _placed:
		if _frames < 2:
			return
		var reason := _place()
		_placed = true
		if not reason.is_empty():
			_finish([reason])
		return
	if _frames < WARMUP_FRAMES:
		return
	if _frames >= MAX_FRAMES:
		_finish(_checks())
		return
	if _samples.size() < SAMPLE_FRAMES:
		_samples.append(_red_at(_mirror_point))
		_control_samples.append(_red_at(_control_point))
		if _samples.size() == SAMPLE_FRAMES:
			# The same rays' hit distances.
			_viewport.debug_draw = Viewport.DEBUG_DRAW_NEXTGI_SPECULAR_HIT
			_hit_view_frame = _frames
		return
	if _frames < _hit_view_frame + HIT_SETTLE_FRAMES:
		return
	_hit_samples.append(_hit_distance_at(_mirror_point))
	_control_hit_samples.append(_hit_distance_at(_control_point))
	if _hit_samples.size() < SAMPLE_FRAMES:
		return
	_finish(_checks())


func _place() -> String:
	_camera.look_at_from_position(CAMERA_POSITION, Vector3(CAMERA_POSITION.x, CAMERA_POSITION.y, 0.0), Vector3.UP)
	_mirror_point = MIRROR_POSITION
	_control_point = MIRROR_POSITION + Vector3(0.0, CONTROL_RISE, 0.0)
	var view := (CAMERA_POSITION - MIRROR_POSITION).normalized()
	# The reflection runs along +X; the mirror normal bisects it and the view.
	_direction = Vector3.RIGHT
	_mirror_normal = (_direction + view).normalized()
	var origin := _mirror_point + _mirror_normal * ORIGIN_OFFSET_M
	var exit_t := _exit_distance(origin, _direction, SCREEN_RANGE_M)
	if exit_t < 0.5 or exit_t > SCREEN_RANGE_M - 0.3:
		return "the reflection ray does not leave the screen inside the screen range (exit %.3f m)" % exit_t
	_wall_t = (WALL_X - origin.x) / _direction.x
	_red_t = exit_t + RED_PAST_EXIT
	if _wall_t < 0.25 * exit_t or _wall_t > 0.55 * exit_t:
		return "the wall is not 25-55%% of the way to the screen exit (wall %.3f m, exit %.3f m)" % [_wall_t, exit_t]
	_wall_center = Vector3(WALL_X, origin.y, (WALL_Z_FROM + WALL_Z_TO) * 0.5)
	_red_center = origin + _direction * _red_t
	var red_edge := _edge_margin(_red_center - _direction * 0.03 + Vector3(0.0, 0.0, -0.3))
	if red_edge > -20.0:
		return "the red panel is on screen (margin %.1f px)" % red_edge
	for corner in [Vector3(WALL_X, origin.y - WALL_HEIGHT * 0.5, WALL_Z_FROM), Vector3(WALL_X, origin.y + WALL_HEIGHT * 0.5, WALL_Z_FROM), Vector3(WALL_X, origin.y - WALL_HEIGHT * 0.5, WALL_Z_TO), Vector3(WALL_X, origin.y + WALL_HEIGHT * 0.5, WALL_Z_TO)]:
		if _edge_margin(corner) < 8.0:
			return "the wall is not fully on screen"
	if _edge_margin(_control_point + Vector3(0.0, CONTROL_SIZE.y * 0.5, 0.0)) < 8.0:
		return "the control mirror is not on screen"
	_build_meshes()
	print("PULLBACK_PLACE wall_t=%.4f red_t=%.4f exit_t=%.4f dir=%s normal=%s" % [_wall_t, _red_t, exit_t, _direction, _mirror_normal])
	return ""


## Distance along the ray at which it leaves the screen, or -1.0.
func _exit_distance(p_origin: Vector3, p_direction: Vector3, p_max: float) -> float:
	if not _on_screen(p_origin):
		return -1.0
	var steps := 600
	for step in steps + 1:
		var t := float(step) / float(steps) * p_max
		if not _on_screen(p_origin + p_direction * t):
			return t
	return -1.0


func _on_screen(p_world: Vector3) -> bool:
	return _edge_margin(p_world) >= 1.0


## Pixels inside the viewport, or negative once the point is outside.
func _edge_margin(p_world: Vector3) -> float:
	if _camera.is_position_behind(p_world):
		return -1000.0
	var screen := _camera.unproject_position(p_world)
	var left := screen.x
	var right := float(VIEW.x) - screen.x
	var top := screen.y
	var bottom := float(VIEW.y) - screen.y
	return minf(minf(left, right), minf(top, bottom))


func _build_meshes() -> void:
	var mirror := _sheet(MIRROR_SIZE, _mirror_material(), false)
	mirror.position = _mirror_point
	mirror.basis = _basis_from_normal(_mirror_normal)
	_root.add_child(mirror)
	var control := _sheet(CONTROL_SIZE, _mirror_material(), false)
	control.position = _control_point
	control.basis = _basis_from_normal(_mirror_normal)
	_root.add_child(control)
	var wall_size := Vector3(WALL_Z_TO - WALL_Z_FROM, WALL_HEIGHT, WALL_THICKNESS)
	var wall := _box(wall_size, _dark_material(), _signed_box_field(wall_size))
	wall.position = _wall_center
	wall.basis = _basis_from_normal(-_direction)
	_root.add_child(wall)
	# Tall enough for the control mirror's ray, which rises with the view ray it reflects.
	var red_size := Vector3(0.6, 1.6, 0.06)
	var red := _box(red_size, _red_material(), _signed_box_field(red_size))
	red.position = _red_center
	red.basis = _basis_from_normal(-_direction)
	_root.add_child(red)
	var light := OmniLight3D.new()
	light.position = _red_center - _direction * 0.35
	light.omni_range = 4.0
	light.light_energy = 8.0
	light.light_color = Color(1, 0.15, 0.1)
	light.shadow_enabled = false
	_root.add_child(light)


func _basis_from_normal(p_normal: Vector3) -> Basis:
	var normal := p_normal.normalized()
	var helper := Vector3.UP if absf(normal.y) < 0.9 else Vector3.RIGHT
	var tangent := helper.cross(normal).normalized()
	var bitangent := normal.cross(tangent).normalized()
	var basis := Basis()
	basis.x = tangent
	basis.y = bitangent
	basis.z = normal
	return basis


func _sheet(p_size: Vector2, p_material: Material, _p_two_sided: bool) -> MeshInstance3D:
	var quad := QuadMesh.new()
	quad.size = p_size
	quad.material = p_material
	var instance := MeshInstance3D.new()
	instance.mesh = quad
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	return instance


func _box(p_size: Vector3, p_material: Material, p_field: MeshDistanceField) -> MeshInstance3D:
	var box := BoxMesh.new()
	box.size = p_size
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, box.get_mesh_arrays())
	mesh.surface_set_material(0, p_material)
	mesh.distance_field = p_field
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.gi_mode = GeometryInstance3D.GI_MODE_STATIC
	return instance


## Signed box, one mip, the MeshSDFBuilder brick layout. The zero crossing is the
## box face, so the global SDF trace can stop on a wall thinner than its voxel.
func _signed_box_field(p_size: Vector3) -> MeshDistanceField:
	var voxel := 0.05
	var pad := 4
	var half := p_size * 0.5
	var dims := Vector3i(ceili(p_size.x / voxel) + pad * 2, ceili(p_size.y / voxel) + pad * 2, ceili(p_size.z / voxel) + pad * 2)
	var origin := -half - Vector3.ONE * voxel * float(pad)
	var band := voxel * float(pad)
	var grid := Vector3i((dims.x + 7) / 8, (dims.y + 7) / 8, (dims.z + 7) / 8)
	var pages := PackedInt32Array()
	pages.resize(grid.x * grid.y * grid.z)
	var bricks := PackedByteArray()
	for bz in grid.z:
		for by in grid.y:
			for bx in grid.x:
				var values := PackedFloat32Array()
				values.resize(512)
				var nearest := INF
				for lz in 8:
					for ly in 8:
						for lx in 8:
							var v := Vector3i(mini(bx * 8 + lx, dims.x - 1), mini(by * 8 + ly, dims.y - 1), mini(bz * 8 + lz, dims.z - 1))
							var p := origin + (Vector3(v) + Vector3.ONE * 0.5) * voxel
							var q := p.abs() - half
							var outside := Vector3(maxf(q.x, 0.0), maxf(q.y, 0.0), maxf(q.z, 0.0)).length()
							var distance := outside + minf(maxf(q.x, maxf(q.y, q.z)), 0.0)
							values[lx + 8 * (ly + 8 * lz)] = distance
							nearest = minf(nearest, absf(distance))
				var page := bx + grid.x * (by + grid.y * bz)
				if nearest > voxel:
					pages[page] = -1
					continue
				pages[page] = bricks.size() / 512
				for value in values:
					bricks.append(clampi(roundi((clampf(value / band, -1.0, 1.0) * 0.5 + 0.5) * 255.0), 0, 255))
	var field := MeshDistanceField.new()
	field.bounds = AABB(origin, Vector3(dims) * voxel)
	field.voxel_size = voxel
	field.mip_dimensions = PackedInt32Array([dims.x, dims.y, dims.z])
	field.mip_brick_grid_dimensions = PackedInt32Array([grid.x, grid.y, grid.z])
	field.mip_bands = PackedFloat32Array([band])
	field.page_tables = [pages]
	field.brick_data = [bricks]
	return field


func _mirror_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.85, 0.85, 0.88)
	material.metallic = 1.0
	material.roughness = 0.0
	return material


func _dark_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.02, 0.02, 0.02)
	material.roughness = 1.0
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	return material


func _red_material() -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(1, 0.05, 0.02)
	material.emission_enabled = true
	material.emission = Color(1, 0.05, 0.02)
	material.emission_energy_multiplier = 12.0
	material.roughness = 1.0
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	return material


## Mean red of the 5x5 pixels around a world point.
func _red_at(p_point: Vector3) -> float:
	var image := _viewport.get_texture().get_image()
	if image == null or image.is_empty():
		return -1.0
	var projected := _camera.unproject_position(p_point)
	var sum := 0.0
	var count := 0
	var cx := int(projected.x * float(image.get_width()) / float(VIEW.x))
	var cy := int(projected.y * float(image.get_height()) / float(VIEW.y))
	for y in range(cy - 2, cy + 3):
		for x in range(cx - 2, cx + 3):
			if x < 0 or y < 0 or x >= image.get_width() or y >= image.get_height():
				continue
			sum += image.get_pixel(x, y).r
			count += 1
	return sum / float(maxi(count, 1))


## The reflection ray's hit distance in meters around a world point, from the
## hit-distance view (log2(1 + meters) / 8, 1 for a miss), or -1.0.
func _hit_distance_at(p_point: Vector3) -> float:
	var scaled := _red_at(p_point)
	if scaled < 0.0:
		return -1.0
	if scaled >= HIT_VIEW_MISS:
		return MISS_DISTANCE_M
	return pow(2.0, 8.0 * scaled) - 1.0


func _mean(p_samples: PackedFloat32Array) -> float:
	var sum := 0.0
	for value in p_samples:
		sum += value
	return sum / float(maxi(p_samples.size(), 1))


func _mean_red() -> float:
	return _mean(_samples)


func _checks() -> PackedStringArray:
	var reasons := PackedStringArray()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var tier := str(info.get("tier", ""))
	var red := _mean_red()
	var control_red := _mean(_control_samples)
	print("PULLBACK_COLOR tier=%s red=%.4f control_red=%.4f samples=%d" % [tier, red, control_red, _samples.size()])
	if _expected_tier != "" and tier != _expected_tier:
		reasons.append("tier is %s, expected %s" % [tier, _expected_tier])
	if _samples.size() < SAMPLE_FRAMES:
		reasons.append("only %d reflection samples" % _samples.size())
	if red < 0.0:
		reasons.append("the reflection view was empty")
	elif red > RED_LIMIT:
		reasons.append("the mirror reflection is red (%.3f); the thin wall did not occlude the emissive past the screen exit" % red)
	if tier == "HWRT" and control_red < RED_MIN:
		reasons.append("the control mirror's reflection is not red (%.3f, at least %.1f): a ray the wall does not stop must show the panel" % [control_red, RED_MIN])
	var past_wall := 0.5 * (_wall_t + _red_t)
	var hit := _mean(_hit_samples)
	var control_hit := _mean(_control_hit_samples)
	print("PULLBACK_HIT tier=%s mirror=%.3f control=%.3f wall_t=%.3f red_t=%.3f samples=%d" % [tier, hit, control_hit, _wall_t, _red_t, _hit_samples.size()])
	if _hit_samples.size() < SAMPLE_FRAMES:
		reasons.append("only %d hit-distance samples" % _hit_samples.size())
	else:
		if hit < MIN_HIT_M or hit >= past_wall:
			reasons.append("the mirror's reflection ray ends %.3f m out; the wall is %.3f m out and the panel %.3f m" % [hit, _wall_t, _red_t])
		if control_hit < past_wall:
			reasons.append("the control mirror's ray ends %.3f m out; it runs over the wall and must pass it (%.3f m)" % [control_hit, _wall_t])
	if tier == "SWRT":
		reasons.append_array(_check_readback(info))
	return reasons


func _check_readback(p_info: Dictionary) -> PackedStringArray:
	var reasons := PackedStringArray()
	var readback: Dictionary = p_info.get("swrt_trace_readback", {})
	if not readback.has("reflect"):
		reasons.append("the software tier did not read the reflection rays back")
		return reasons
	if not bool(readback.get("screen_trace", false)) or not bool(readback.get("normal_roughness", false)):
		reasons.append("the software readback ran without the screen trace or the normal-roughness prepass")
	var bytes: PackedByteArray = readback["reflect"]
	var on_wall := 0
	var past := 0
	var mirror_rays := 0
	var control_past := 0
	var control_stopped := 0
	var best := -1.0
	var count := bytes.size() / 80
	for index in count:
		var base := index * 80
		var flags := bytes.decode_u32(base + 76)
		if flags & HIT_SKIPPED:
			continue
		var surface := Vector3(bytes.decode_float(base), bytes.decode_float(base + 4), bytes.decode_float(base + 8))
		if surface.distance_to(_mirror_point) > 0.7:
			continue
		# The control mirror's rays are counted apart: they must pass the wall.
		var control := absf(surface.y - _control_point.y) <= CONTROL_SIZE.y * 0.5 + 0.02
		if not control and absf(surface.y - _mirror_point.y) > MIRROR_SIZE.y * 0.5 + 0.02:
			continue
		var direction := Vector3(bytes.decode_float(base + 32), bytes.decode_float(base + 36), bytes.decode_float(base + 40))
		if not control:
			mirror_rays += 1
			best = maxf(best, direction.dot(_direction))
		if direction.dot(_direction) < 0.5:
			continue
		var offset := bytes.decode_float(base + 12)
		var surface_normal := Vector3(bytes.decode_float(base + 16), bytes.decode_float(base + 20), bytes.decode_float(base + 24))
		var t := bytes.decode_float(base + 60)
		var hit := surface + surface_normal * offset + direction * maxf(t, 0.0)
		# A ray that hit nothing passed the wall.
		var stopped := (flags & HIT_SURFACE) != 0 and (hit - _mirror_point).dot(_direction) < 0.5 * (_wall_t + _red_t)
		if control:
			if stopped:
				control_stopped += 1
			else:
				control_past += 1
		elif stopped:
			on_wall += 1
		else:
			past += 1
	print("PULLBACK_RAYS mirror=%d on_wall=%d past=%d best_dot=%.3f control_past=%d control_stopped=%d" % [mirror_rays, on_wall, past, best, control_past, control_stopped])
	if past > 0:
		reasons.append("software reflection rays along the exit: %d hit the wall, %d pass it (best alignment %.2f, %d mirror rays)" % [on_wall, past, best, mirror_rays])
	if on_wall < MIN_WALL_RAYS:
		reasons.append("only %d software reflection rays were read back on the wall (at least %d)" % [on_wall, MIN_WALL_RAYS])
	if control_past < MIN_CONTROL_RAYS:
		reasons.append("only %d of the control mirror's software rays pass the wall (at least %d; %d stopped)" % [control_past, MIN_CONTROL_RAYS, control_stopped])
	return reasons


func _finish(p_reasons: PackedStringArray) -> void:
	_done = true
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	print("GODOTNEXT_RESULT " + JSON.stringify({
		"test": "screen_pullback",
		"pass": p_reasons.is_empty(),
		"tier": info.get("tier", ""),
		"expected_tier": _expected_tier,
		"red": _mean_red() if not _samples.is_empty() else -1.0,
		"control_red": _mean(_control_samples) if not _control_samples.is_empty() else -1.0,
		"hit_m": _mean(_hit_samples) if not _hit_samples.is_empty() else -1.0,
		"control_hit_m": _mean(_control_hit_samples) if not _control_hit_samples.is_empty() else -1.0,
		"failures": p_reasons,
	}))
	for reason in p_reasons:
		push_error("SCREEN_PULLBACK_FAIL %s" % reason)
	get_tree().quit(0 if p_reasons.is_empty() else 1)
