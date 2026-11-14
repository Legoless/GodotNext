extends Node3D
## #186: the software tracer on analytic geometry, checked ray by ray.
##
## The scene is a few boxes (a mirror floor, a rough cube, a pillar, a wall)
## with matching physics shapes, a sun and a two-sided panel whose mesh SDF is
## built here. With debug/swrt_trace_readback on (override.cfg), the trace
## reads back every ray of its 64x64 verification grid; each one is compared
## with physics ray casts against the same boxes:
## - the depth-reconstructed surface sits on the box the camera ray hits,
## - gather and reflection rays hit where physics hits, within the voxel size
##   of the clipmap level, with the hit normal of the box face,
## - reflection rays on the mirror floor follow the mirror direction and the
##   rough cube (above max_roughness_to_trace) is skipped,
## - SDF shadow rays toward the sun agree with physics,
## - the #182 coverage words of the two-sided panel match the CPU rule,
## - rays whose physics hit lies within the 2 m detail range hit it in time,
## - few rays run out of trace steps (a ray out of steps reports a hit),
## - some trace steps step by the global SDF's coarse bound (#257); the summary's counters report the
##   steps and the coarse ones, and on every resident level the bound matches its CPU twin and reads
##   past no instance. Then debug/swrt_coarse_levels masks the bound off (no coarse step, more steps)
##   and the climb through coarser empty bricks too (more steps again),
## - no ray-query ShaderRD was created in the process (override.cfg starts it with
##   hardware_ray_tracing Off, which skips the startup self-test) (#186); under
##   debug/deny_ray_query (a device without ray query) also the reason NO_RAY_QUERY, no RT
##   arena and SWRT instances.
## Then the sun drops to LOW_SUN_DEGREES above the horizon, so the shadow rays of the open
## floor graze it for tens of meters through its resident bricks: no open-floor receiver
## that physics sees lit reads shadowed, and no grazing gather ray over the open floor
## reports a hit where physics finds nothing (the brick cap without its clearance made these
## rays run out of steps: 248 of 875 lit open-floor receivers read shadowed).
## Pass --detail to run with software_ray_tracing_mode = detail (deprecated).
## Pass --dense to add a forest of thin poles, so the detail froxels find more
## instances than they can list (the trace must still see every pole).
## Pass --scaled to build every box as a unit BoxMesh scaled to its size (the
## physics shapes stay the same), so the mesh SDFs are non-uniformly scaled.
## Pass --sheared to turn every box but the floor 30 degrees about Y under a parent scaled
## (1.5, 1, 1), so the boxes are parallelepipeds (physics gets the same corners). With
## --scaled their mesh SDFs are unit boxes under a sheared basis.
## Pass --reduced to give the tracer a global SDF pool (debug/swrt_trace_pool_bricks)
## that holds only some of its six levels: it drops levels from the top until the
## rest fit (#187 SWRT_REDUCED), reports it, and still passes every check above.
## Pass --starved for a pool too small even for three levels: the tracer stops at
## three, builds the levels coarsest first so only level 0 runs out of slots, keeps it
## resident with the bricks nearest the camera, and builds no level in full again. The
## trace reads level 0's missing bricks from level 1 and still passes every check above
## but the low-sun pass.
## Pass --stale (#264) for a coarser level that marches past a change a finer level holds. Level L
## updates on every 2^L-th frame, so after an instance moves, the coarser levels keep its old place
## for a frame or more (a dirty volume pending). After the first readback's ray checks (the
## coarse-mask, low-sun and step-budget passes do not run), the sun moves high behind the camera, so
## the shadow rays of the wall's face and of the floor in front of it climb toward the camera through
## level 1 and cross into level 0. A slab jumps into those rays just past level 0's edge
## (STALE_SLAB_INSET inside the region the trace marches level 0 in), and back under the floor, once
## every STALE_CYCLE frames: an odd cycle, so it lands on the frames level 1 skips and on those it
## updates alike. On every frame the slab is in place, every shadow, gather and reflection ray that
## physics sees cross it (STALE_EDGE_MARGIN inside its edges) must hit it, also on the frames level 1
## still holds it under the floor (bit 9 clear in the readback's coarse_level_mask). A stale level's
## empty-brick skip or distance step used to carry those rays up to one level-1 brick past the slab.
## --reduced and --starved are sized for the plain scene: do not combine them with
## --dense, --scaled or --sheared. Nor --dense with --sheared: the poles keep clear of the
## upright boxes only. --stale runs on the plain scene alone.
## Every run also compares the GPU composite of the global SDF with its CPU twin
## (NextGIGlobalSDF::composite_distance), checks that no resident voxel reads
## past the nearest instance, and checks that the #187 microbenchmark's trace
## agrees with its own fixture.
## Except with --reduced and --starved, the camera starts CAMERA_TRAVEL away and moves into
## place first, so the clipmaps scroll and every check runs on bricks a scroll kept (shifted,
## never rebuilt): a brick's composite must not depend on its place in its level.
## Run it through harness/runners/macos_local.sh --fail-on-validation, which fails the run on any
## "ERROR:", "Invalid Metal usage" or "Error detected in" line.

const WAIT_FRAMES := 300
const MAX_FRAMES := 900
const EXHAUST_STEPS := 6
const EXHAUST_FRAMES := 30
## Gather or reflection rays that may run out of the 64-step budget, as a fraction of the
## traced rays. Every mode here measures at most 9 of 2738 (0.33%, --scaled); the brick cap
## without its clearance made it 2.2-4.4%.
const MAX_EXHAUSTED_FRACTION := 0.005
## The GPU composite against its CPU twin: half-float storage rounds about 0.001 m at the
## level-0 cap. A whole voxel (0.1 m) hid a 0.06 m mismatch.
const COMPOSITE_TOLERANCE := 0.005
## The low-sun pass: the sun's elevation, and the frames it gets to reach the readback.
const LOW_SUN_DEGREES := 8.0
const LOW_SUN_FRAMES := 30
## An open-floor ray stays this far from every box but the floor (a swept sphere), so a
## surface band or a trilinear blend of a coarse level cannot reach one.
const LOW_SUN_CLEARANCE := 0.4
## Gather rays that graze the open floor: elevations in this range, in degrees.
const GRAZING_DEGREES := Vector2(4.0, 16.0)
## cast_motion() resolves a long motion coarsely (one 200 m cast missed a wall 0.5 m from its
## start), so open paths are swept in steps this long.
const OPEN_PATH_STEP := 1.0
## The camera's start offset and the frames it moves in (more than a 6.4 m level-3 brick on x).
const CAMERA_TRAVEL := Vector3(-7.0, 0.0, 3.0)
const CAMERA_MOVE_FRAMES := Vector2i(30, 150)
## #257: the coarse-level mask stages, and the frames each waits once the readback shows its mask
## (the step counters arrive a frame or more late).
const COARSE_LEVELS_SETTING := "rendering/global_illumination/nextgi/debug/swrt_coarse_levels"
const COARSE_MASK_NO_BOUND := 0x3F00
const COARSE_MASK_FRAMES := 8
const CAMERA_POSITION := Vector3(0.3, 3.2, 4.2)
## The detail range of software_ray_tracing_mode = detail (SWRT_DETAIL_METERS in nextgi_sdf_trace_inc.glsl).
const DETAIL_METERS := 2.0
const FROXEL_CAP := 8
## Pool sizes in bricks, for the plain scene (no --dense, no --scaled). Its six levels
## hold PLAIN_LEVEL_BRICKS (level_bricks in the result). Levels 0 to 3 take 2223 bricks and
## levels 0 to 4 take 2263, so REDUCED_POOL_BRICKS keeps four levels. STARVED_POOL_BRICKS
## holds levels 1 and 2 and 427 of level 0's bricks. If the scene or the mesh SDF builder
## changes these counts, pick new sizes from level_bricks.
const PLAIN_LEVEL_BRICKS := [1086, 700, 309, 128, 40, 12]
const REDUCED_POOL_BRICKS := 2243
const REDUCED_LEVELS := 4
const STARVED_POOL_BRICKS := 1436
const MIN_LEVELS := 3
const STARVED_SETTLE_FRAMES := 60
## #264 --stale. Level 0's voxel (NextGIGlobalSDF::level_voxel_size(0)); the trace marches level 0
## from two voxels inside its clipmap's edge (swrt_level_at()).
const LEVEL0_VOXEL := 0.1
const LEVEL_MARGIN_VOXELS := 2
const LEVEL_SIZE_VOXELS := 128
## The sun of the stale stage: 60 degrees up, behind the camera (to the light: +z and up).
const STALE_SUN_DEGREES := Vector3(-60.0, 0.0, 0.0)
## The slab: wider than the wall, where the wall's shadow rays cross it, one level-1 voxel thick.
## Its near face lies STALE_SLAB_INSET past level 0's edge.
const STALE_SLAB_SIZE := Vector3(6.6, 2.0, 0.2)
const STALE_SLAB_Y := 3.7
const STALE_SLAB_INSET := 0.05
## Under the floor slab, out of every ray's way.
const STALE_PARKED := Vector3(0.0, -1.5, -2.0)
## The slab is in place on one frame of every STALE_CYCLE (odd: both parities of the frame number).
const STALE_CYCLE := 3
## Frames verified once the stale sun reached the readback.
const STALE_FRAMES := 60
## A ray counts when physics sees it reach the slab first, through the slab's box shrunk by
## STALE_EDGE_MARGIN across its face and for at least STALE_MIN_PATH (a corner graze is the
## SDF's surface band, not a crossing). A gather or reflection hit counts up to two level-0 voxels
## past where physics meets the slab.
const STALE_EDGE_MARGIN := 0.15
const STALE_MIN_PATH := 0.1
const STALE_HIT_TOLERANCE := 2.0 * LEVEL0_VOXEL + 0.05
## Coverage: frames with the slab in place while level 1 was stale, and the shadow rays that
## crossed into level 0 on them.
const STALE_MIN_HAZARD_FRAMES := 4
const STALE_MIN_HAZARD_RAYS := 200

var _frames := 0
var _stage := 0
var _coarse_steps_full := -1
var _coarse_mask_full := -1
var _coarse_steps_no_bound := -1
var _stage_frame := 0
var _reasons := PackedStringArray()
var _summary := {}
var _space: PhysicsDirectSpaceState3D
var _camera: Camera3D
var _sun: DirectionalLight3D
var _floor_body: StaticBody3D
var _boxes_top := 0.0
var _camera_moves := false
var _roughness_of := {}
var _panel: MeshInstance3D
var _detail := false
var _dense := false
var _scaled := false
var _sheared := false
var _reduced := false
var _starved := false
var _starved_full_builds := -1
var _stale := false
var _slab: MeshInstance3D
var _slab_body: StaticBody3D
var _stale_region := AABB()
var _stale_place := Vector3()
var _stale_step := 0
var _stale_shown_in_place := false
var _stale_last_frame := -1
var _stale_stats := {}


func _ready() -> void:
	_detail = OS.get_cmdline_user_args().has("--detail")
	_dense = OS.get_cmdline_user_args().has("--dense")
	_scaled = OS.get_cmdline_user_args().has("--scaled")
	_sheared = OS.get_cmdline_user_args().has("--sheared")
	_reduced = OS.get_cmdline_user_args().has("--reduced")
	_starved = OS.get_cmdline_user_args().has("--starved")
	_stale = OS.get_cmdline_user_args().has("--stale")
	if _reduced or _starved:
		# Read when the tracer is created, on the first software-tier frame.
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/swrt_trace_pool_bricks", STARVED_POOL_BRICKS if _starved else REDUCED_POOL_BRICKS)
	if _detail:
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/software_ray_tracing_mode", 1)
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.3, 0.4, 0.6)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.2, 0.2, 0.25)
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	# The floor reaches past 12.8 m from the camera, into clipmap level 2.
	_add_box("Floor", Vector3(40.0, 0.2, 40.0), Vector3(0.0, -0.1, -10.0), 0.0)
	_add_box("Cube", Vector3(1.0, 1.0, 1.0), Vector3(0.6, 0.5, -1.6), 1.0)
	_add_box("Pillar", Vector3(0.6, 2.4, 0.6), Vector3(-1.6, 1.2, -1.2), 0.3)
	_add_box("Wall", Vector3(7.0, 1.5, 0.3), Vector3(0.0, 0.75, -4.2), 0.3)
	_add_box("LeftWall", Vector3(0.3, 2.0, 4.0), Vector3(-3.6, 1.0, -2.0), 0.6)
	_add_box("RightWall", Vector3(0.3, 2.0, 4.0), Vector3(3.6, 1.0, -2.0), 0.6)
	_panel = _add_two_sided_panel(Vector3(1.4, 1.0, 0.06), Vector3(2.4, 0.9, -2.6))
	if _dense:
		_add_poles()
	if _stale:
		_add_slab()

	# Low behind the wall, so the wall, the pillar and the cube cast shadows toward the camera.
	_sun = DirectionalLight3D.new()
	_sun.rotation_degrees = Vector3(-35.0, 165.0, 0.0)
	add_child(_sun)

	_camera = Camera3D.new()
	add_child(_camera)
	_camera_moves = not (_reduced or _starved)
	var travel := CAMERA_TRAVEL if _camera_moves else Vector3.ZERO
	_camera.position = CAMERA_POSITION + travel
	_camera.look_at(Vector3(0.0, 0.3, -3.0) + travel)
	_camera.current = true


func _add_box(p_name: String, p_size: Vector3, p_position: Vector3, p_roughness: float) -> void:
	var mesh := BoxMesh.new()
	mesh.size = Vector3.ONE if _scaled else p_size
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.7, 0.7)
	material.roughness = p_roughness
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = mesh
	# The global transform a node turned 30 degrees about Y gets under a parent scaled (1.5, 1, 1).
	var basis := Basis.from_scale(Vector3(1.5, 1.0, 1.0)) * Basis.from_euler(Vector3(0.0, PI / 6.0, 0.0)) if _sheared and p_name != "Floor" else Basis()
	instance.transform = Transform3D(basis * Basis.from_scale(p_size) if _scaled else basis, p_position)
	add_child(instance)
	var body := StaticBody3D.new()
	body.name = p_name + "Body"
	body.position = p_position
	var shape := CollisionShape3D.new()
	if basis == Basis():
		var box := BoxShape3D.new()
		box.size = p_size
		shape.shape = box
	else:
		var convex := ConvexPolygonShape3D.new()
		var points := PackedVector3Array()
		for corner in 8:
			points.append(basis * (Vector3(0.5 if corner & 1 else -0.5, 0.5 if corner & 2 else -0.5, 0.5 if corner & 4 else -0.5) * p_size))
		convex.points = points
		shape.shape = convex
	body.add_child(shape)
	add_child(body)
	_roughness_of[body.get_instance_id()] = p_roughness
	if p_name == "Floor":
		_floor_body = body
	else:
		_boxes_top = maxf(_boxes_top, p_position.y + 0.5 * p_size.y)


## Poles 1.2 m apart over the visible floor, clear of the other boxes. They are
## 0.4 m thick, so the 10 cm and 20 cm global-SDF levels resolve them.
func _add_poles() -> void:
	# x/z footprints of the boxes above, grown by a margin so no pole touches them.
	var keep_out := [
		Rect2(0.1, -2.1, 1.0, 1.0), Rect2(-1.9, -1.5, 0.6, 0.6), Rect2(-3.5, -4.35, 7.0, 0.3),
		Rect2(-3.75, -4.0, 0.3, 4.0), Rect2(3.45, -4.0, 0.3, 4.0), Rect2(1.7, -2.63, 1.4, 0.06),
	]
	var count := 0
	for iz in 6:
		for ix in 6:
			var center := Vector2(-3.0 + 1.2 * ix, -3.6 + 1.2 * iz)
			var footprint := Rect2(center - Vector2(0.2, 0.2), Vector2(0.4, 0.4)).grow(0.3)
			var clear := true
			for rect in keep_out:
				if footprint.intersects(rect):
					clear = false
			if clear:
				_add_box("Pole%d" % count, Vector3(0.4, 1.0, 0.4), Vector3(center.x, 0.5, center.y), 0.3)
				count += 1


## #264 --stale: the slab that jumps across level 0's edge, parked under the floor until the stale
## stage. Created here so its mesh SDF is built before the checks start.
func _add_slab() -> void:
	var mesh := BoxMesh.new()
	mesh.size = STALE_SLAB_SIZE
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.7, 0.7)
	mesh.material = material
	_slab = MeshInstance3D.new()
	_slab.name = "StaleSlab"
	_slab.mesh = mesh
	_slab.position = STALE_PARKED
	add_child(_slab)
	_slab_body = StaticBody3D.new()
	_slab_body.name = "StaleSlabBody"
	_slab_body.position = STALE_PARKED
	var shape := CollisionShape3D.new()
	var box := BoxShape3D.new()
	box.size = STALE_SLAB_SIZE
	shape.shape = box
	_slab_body.add_child(shape)
	add_child(_slab_body)
	_roughness_of[_slab_body.get_instance_id()] = 0.5


## A thin two-sided box whose unsigned mesh SDF is encoded here (one mip, the
## MeshSDFBuilder brick layout), so the global SDF has two-sided coverage.
func _add_two_sided_panel(p_size: Vector3, p_position: Vector3) -> MeshInstance3D:
	var box := BoxMesh.new()
	box.size = p_size
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, box.get_mesh_arrays())
	var material := StandardMaterial3D.new()
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_set_material(0, material)
	mesh.distance_field = _unsigned_box_field(p_size)
	var instance := MeshInstance3D.new()
	instance.name = "TwoSidedPanel"
	instance.mesh = mesh
	instance.position = p_position
	add_child(instance)
	var body := StaticBody3D.new()
	body.name = "TwoSidedPanelBody"
	body.position = p_position
	var shape := CollisionShape3D.new()
	var box_shape := BoxShape3D.new()
	box_shape.size = p_size
	shape.shape = box_shape
	body.add_child(shape)
	add_child(body)
	_roughness_of[body.get_instance_id()] = 0.5
	_boxes_top = maxf(_boxes_top, p_position.y + 0.5 * p_size.y)
	return instance


func _unsigned_box_field(p_size: Vector3) -> MeshDistanceField:
	var voxel := 0.05
	var pad := 4
	var half := p_size * 0.5
	var dims := Vector3i(ceili(p_size.x / voxel) + pad * 2, ceili(p_size.y / voxel) + pad * 2, ceili(p_size.z / voxel) + pad * 2)
	var origin := -half - Vector3.ONE * voxel * pad
	var band := voxel * pad
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
							var distance := absf(outside + minf(maxf(q.x, maxf(q.y, q.z)), 0.0))
							values[lx + 8 * (ly + 8 * lz)] = distance
							nearest = minf(nearest, distance)
				var page := bx + grid.x * (by + grid.y * bz)
				if nearest > voxel:
					pages[page] = -1
					continue
				pages[page] = bricks.size() / 512
				for value in values:
					bricks.push_back(clampi(roundi((clampf(value / band, -1.0, 1.0) * 0.5 + 0.5) * 255.0), 0, 255))
	var field := MeshDistanceField.new()
	field.bounds = AABB(origin, Vector3(dims) * voxel)
	field.voxel_size = voxel
	field.flags = MeshDistanceField.FLAG_TWO_SIDED | MeshDistanceField.FLAG_UNSIGNED
	field.mip_dimensions = PackedInt32Array([dims.x, dims.y, dims.z])
	field.mip_brick_grid_dimensions = PackedInt32Array([grid.x, grid.y, grid.z])
	field.mip_bands = PackedFloat32Array([band])
	field.page_tables = [pages]
	field.brick_data = [bricks]
	return field


func _write_manifest(p_summary: Dictionary) -> void:
	var out_dir := ""
	for arg in OS.get_cmdline_user_args():
		if str(arg).begins_with("--out="):
			out_dir = str(arg).substr("--out=".length())
	if out_dir.is_empty():
		return
	DirAccess.make_dir_recursive_absolute(out_dir)
	var file := FileAccess.open(out_dir.path_join("manifest.json"), FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify({ "source": "swrt_trace_check", "summary": p_summary }, "\t"))
	if not _dump.is_empty():
		var dump := FileAccess.open(out_dir.path_join("rays.txt"), FileAccess.WRITE)
		if dump != null:
			dump.store_string("\n".join(_dump))


func _finish(p_pass: bool, p_reasons: PackedStringArray, p_summary: Dictionary) -> void:
	p_summary["pass"] = p_pass
	p_summary["reasons"] = p_reasons
	print("GODOTNEXT_RESULT %s" % JSON.stringify(p_summary))
	if not p_pass:
		for reason in p_reasons:
			push_error("SWRT_TRACE_FAIL %s" % reason)
	_write_manifest(p_summary)
	get_tree().quit(0 if p_pass else 1)


func _process(_delta: float) -> void:
	_frames += 1
	if _camera_moves and _frames <= CAMERA_MOVE_FRAMES.y:
		var weight := clampf(float(_frames - CAMERA_MOVE_FRAMES.x) / float(CAMERA_MOVE_FRAMES.y - CAMERA_MOVE_FRAMES.x), 0.0, 1.0)
		_camera.position = CAMERA_POSITION + CAMERA_TRAVEL * (1.0 - weight)
	if _frames < WAIT_FRAMES:
		return
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var readback: Dictionary = info.get("swrt_trace_readback", {})
	if readback.is_empty() or not readback.has("gather"):
		if _frames >= MAX_FRAMES:
			_finish(false, PackedStringArray(["no SWRT readback (tier %s)" % str(info.get("tier", "?"))]), {})
		return
	_space = get_world_3d().direct_space_state
	if _stage == 0 and _starved:
		_verify(info, readback)
		_verify_starved(info)
		_starved_full_builds = int(info.get("swrt_trace_full_builds", -1))
		_stage = 2
		_stage_frame = _frames
		return
	if _stage == 2:
		# A level that ran out of slots at the level floor stays resident: no full rebuild
		# on every update (it would fail the same way).
		if _frames - _stage_frame < STARVED_SETTLE_FRAMES:
			return
		var full_builds := int(info.get("swrt_trace_full_builds", -1))
		_summary["starved_full_builds"] = [_starved_full_builds, full_builds]
		if full_builds != _starved_full_builds:
			_reasons.append("starved pool: %d full level builds in %d frames" % [full_builds - _starved_full_builds, STARVED_SETTLE_FRAMES])
		_finish(_reasons.is_empty(), _reasons, _summary)
		return
	if _stage == 6:
		_process_stale(info, readback)
		return
	if _stage == 0:
		_verify(info, readback)
		if _reduced:
			_verify_reduced(info, REDUCED_POOL_BRICKS, REDUCED_LEVELS)
		if _stale:
			# Stage 6 (#264): the slab across level 0's edge.
			_start_stale(readback)
			return
		# Stage 4 (#257): without the coarse bound, then also without the climb through coarser
		# empty bricks, through debug/swrt_coarse_levels. The trace must follow the mask it is given.
		_coarse_steps_full = int(info.get("swrt_steps", -1))
		# The tracer's own mask covers its resident levels only (four under --reduced).
		_coarse_mask_full = int(readback.get("coarse_level_mask", -1))
		ProjectSettings.set_setting(COARSE_LEVELS_SETTING, COARSE_MASK_NO_BOUND)
		_stage = 4
		_stage_frame = _frames
		return
	if _stage == 4 or _stage == 5:
		var expected_mask := (_coarse_mask_full & COARSE_MASK_NO_BOUND) if _stage == 4 else 0
		if int(readback.get("coarse_level_mask", -1)) != expected_mask or _frames - _stage_frame < COARSE_MASK_FRAMES:
			if _frames >= MAX_FRAMES:
				_reasons.append("debug/swrt_coarse_levels %d never reached the SWRT readback" % expected_mask)
				_finish(false, _reasons, _summary)
			return
		var steps := int(info.get("swrt_steps", -1))
		var coarse := int(info.get("swrt_coarse_steps", -1))
		if coarse != 0:
			_reasons.append("coarse mask %d: the trace took %d coarse-bound steps" % [expected_mask, coarse])
		if _stage == 4:
			_coarse_steps_no_bound = steps
			if steps <= _coarse_steps_full:
				_reasons.append("without the coarse bound the trace took %d steps, not more than the %d with it" % [steps, _coarse_steps_full])
			ProjectSettings.set_setting(COARSE_LEVELS_SETTING, 0)
			_stage = 5
			_stage_frame = _frames
			return
		_summary["coarse_mask_steps"] = {"full": _coarse_steps_full, "no_bound": _coarse_steps_no_bound, "no_bound_no_climb": steps}
		if steps <= _coarse_steps_no_bound:
			_reasons.append("without the climb through coarser empty bricks the trace took %d steps, not more than the %d with it" % [steps, _coarse_steps_no_bound])
		ProjectSettings.set_setting(COARSE_LEVELS_SETTING, -1)
		# Stage 3: the low sun.
		_sun.rotation_degrees = Vector3(-LOW_SUN_DEGREES, 165.0, 0.0)
		_stage = 3
		_stage_frame = _frames
		return
	if _stage == 3:
		var travel: Vector3 = readback.get("light_direction", Vector3())
		var expected_travel := -_sun.global_transform.basis.z.normalized()
		if _frames - _stage_frame < LOW_SUN_FRAMES or travel.normalized().dot(expected_travel) < 0.9999:
			if _frames >= MAX_FRAMES:
				_reasons.append("the low sun never reached the SWRT readback")
				_finish(false, _reasons, _summary)
			return
		_verify_low_sun(readback)
		# Stage 1: a tiny step budget. Rays that run out report a hit at the current t (#186 step 2).
		ProjectSettings.set_setting("rendering/global_illumination/nextgi/debug/swrt_trace_max_steps", EXHAUST_STEPS)
		_stage = 1
		_stage_frame = _frames
		return
	if _frames - _stage_frame < EXHAUST_FRAMES or int(readback.get("max_steps", 0)) != EXHAUST_STEPS:
		if _frames >= MAX_FRAMES:
			_reasons.append("the step budget never reached %d" % EXHAUST_STEPS)
			_finish(false, _reasons, _summary)
		return
	_verify_exhausted(readback)
	_finish(_reasons.is_empty(), _reasons, _summary)


## #187 SWRT_REDUCED: the pool the check asked for, the levels it holds, and the flag.
func _verify_reduced(p_info: Dictionary, p_pool: int, p_levels: int) -> void:
	_summary["pool_bricks"] = int(p_info.get("swrt_trace_pool_bricks", 0))
	_summary["used_bricks"] = int(p_info.get("swrt_trace_used_bricks", 0))
	_summary["swrt_reduced"] = bool(p_info.get("swrt_reduced", false))
	_summary["levels"] = int(p_info.get("swrt_trace_levels", 0))
	var readback: Dictionary = p_info.get("swrt_trace_readback", {})
	_summary["level_bricks"] = Array(readback.get("level_bricks", PackedInt32Array()))
	if _summary["pool_bricks"] != p_pool:
		_reasons.append("the pool holds %d bricks, not the %d asked for" % [_summary["pool_bricks"], p_pool])
	if not _summary["swrt_reduced"]:
		_reasons.append("a pool of %d bricks did not report SWRT_REDUCED" % p_pool)
	if _summary["levels"] != p_levels:
		_reasons.append("a pool of %d bricks keeps %d levels, not %d" % [p_pool, _summary["levels"], p_levels])


## --starved: three levels, the pool full, and only level 0 partial (_verify checks the trace).
func _verify_starved(p_info: Dictionary) -> void:
	_summary["starved"] = true
	if str(p_info.get("tier", "")) != "SWRT":
		_reasons.append("tier is %s, not SWRT" % str(p_info.get("tier", "")))
	_verify_reduced(p_info, STARVED_POOL_BRICKS, MIN_LEVELS)
	if _summary["used_bricks"] != STARVED_POOL_BRICKS:
		_reasons.append("starved pool: %d of %d bricks used" % [_summary["used_bricks"], STARVED_POOL_BRICKS])
	var level_bricks: Array = _summary["level_bricks"]
	var resident := 0
	for bricks in level_bricks:
		resident += int(bricks)
	if level_bricks.size() != MIN_LEVELS or resident != STARVED_POOL_BRICKS or int(level_bricks[0]) <= 0:
		_reasons.append("starved pool: the levels hold %s bricks of %d" % [str(level_bricks), STARVED_POOL_BRICKS])
	elif int(level_bricks[1]) != PLAIN_LEVEL_BRICKS[1] or int(level_bricks[2]) != PLAIN_LEVEL_BRICKS[2]:
		# The levels are built coarsest first: the trace reads level 0's missing bricks from level 1.
		_reasons.append("starved pool: levels 1 and 2 hold %s bricks, not all of %s" % [str(level_bricks.slice(1)), str(PLAIN_LEVEL_BRICKS.slice(1, 3))])


## #264 --stale (stage 6): the region the trace marches level 0 in, the slab's place inside its
## edge that faces the wall, and the sun behind the camera.
func _start_stale(p_readback: Dictionary) -> void:
	var origins: Array = p_readback.get("level_origins", [])
	if origins.is_empty():
		_reasons.append("stale: the SWRT readback has no level origins")
		_finish(false, _reasons, _summary)
		return
	var origin: Vector3i = origins[0]
	_stale_region = AABB((Vector3(origin) + Vector3.ONE * LEVEL_MARGIN_VOXELS) * LEVEL0_VOXEL, Vector3.ONE * (LEVEL_SIZE_VOXELS - 2 * LEVEL_MARGIN_VOXELS) * LEVEL0_VOXEL)
	# The wall lies past level 0's -z edge, its shadow rays climb across it toward the camera.
	_stale_place = Vector3(0.0, STALE_SLAB_Y, _stale_region.position.z + STALE_SLAB_INSET + 0.5 * STALE_SLAB_SIZE.z)
	_stale_stats = {
		"frames": 0,
		"in_place_frames": 0,
		"hazard_frames": 0,
		"frames_with_misses": 0,
		"skipped_frames": 0,
		"level0_edge_z": _stale_region.position.z,
		"slab_place": str(_stale_place),
		"steps_sum": 0,
		"rays": {},
		"misses": [],
	}
	for kind in ["shadow", "gather", "reflect"]:
		_stale_stats["rays"][kind] = { "slab": 0, "crossing": 0, "crossing_on_stale": 0, "missed": 0, "missed_crossing": 0 }
	_sun.rotation_degrees = STALE_SUN_DEGREES
	_stage = 6
	_stage_frame = _frames
	_stale_step = 0
	_move_slab(false)


## Puts the slab (its mesh and its body) in place or under the floor. The next _process() reads the
## readback of the frame drawn now, with the slab here.
func _move_slab(p_in_place: bool) -> void:
	var place := _stale_place if p_in_place else STALE_PARKED
	_slab.position = place
	_slab_body.position = place
	_stale_shown_in_place = p_in_place


func _process_stale(p_info: Dictionary, p_readback: Dictionary) -> void:
	var travel: Vector3 = p_readback.get("light_direction", Vector3())
	var expected_travel := -_sun.global_transform.basis.z.normalized()
	if travel.normalized().dot(expected_travel) >= 0.9999:
		# Each readback must be the next frame's, or the slab's place would not be the one traced.
		var frame := int(p_readback.get("frame", -1))
		if frame < 0 or (_stale_last_frame >= 0 and frame != _stale_last_frame + 1):
			_stale_stats["skipped_frames"] += 1
		_stale_last_frame = frame
		_verify_stale(p_info, p_readback)
	if _stale_stats["frames"] >= STALE_FRAMES:
		_finish_stale()
		return
	if _frames >= MAX_FRAMES:
		_reasons.append("stale: %d of %d frames verified by frame %d (the stale sun reached the readback: %s)" % [_stale_stats["frames"], STALE_FRAMES, _frames, str(travel.normalized().dot(expected_travel) >= 0.9999)])
		_finish(false, _reasons, _summary)
		return
	_stale_step += 1
	_move_slab(_stale_step % STALE_CYCLE == 0)


## One frame of the stale stage: with the slab in place, every ray physics sees cross it must hit it.
func _verify_stale(p_info: Dictionary, p_readback: Dictionary) -> void:
	_stale_stats["frames"] += 1
	# The step counters arrive a frame or more late: a report-only mean over the stage.
	_stale_stats["steps_sum"] += int(p_info.get("swrt_steps", 0))
	if not _stale_shown_in_place:
		return
	_stale_stats["in_place_frames"] += 1
	# Bit 8 + 1 clear: level 1 still has the slab's move pending, so it holds the slab under the floor.
	var mask := int(p_readback.get("coarse_level_mask", -1))
	var level1_stale := mask >= 0 and (mask & (256 << 1)) == 0
	if level1_stale:
		_stale_stats["hazard_frames"] += 1
	var slab := AABB(_slab_body.global_position - 0.5 * STALE_SLAB_SIZE, STALE_SLAB_SIZE)
	var missed_before := _stale_missed()
	var gather := _decode_rays(p_readback["gather"])
	# Shadow rays toward the stale sun (result 0: occluded, 2: no request).
	var results: PackedByteArray = p_readback.get("shadow", PackedByteArray())
	var to_light := -(p_readback["light_direction"] as Vector3).normalized()
	for index in mini(gather.size(), results.size() / 4):
		var ray: Dictionary = gather[index]
		if int(ray["flags"]) & 128:
			continue
		var result := results.decode_u32(index * 4)
		if result == 2:
			continue
		var origin: Vector3 = ray["surface"] + ray["surface_normal"] * ray["offset"]
		if _slab_t(origin, to_light, 200.0, slab) < 0.0:
			continue
		_stale_count("shadow", origin, to_light, level1_stale, result != 0)
	# Gather and reflection rays the SDF traced (a screen-trace hit is the Hi-Z march's): a hit no
	# farther than STALE_HIT_TOLERANCE past the slab.
	var kinds := { "gather": gather }
	if p_readback.has("reflect"):
		kinds["reflect"] = _decode_rays(p_readback["reflect"])
	for kind in kinds:
		for ray in kinds[kind]:
			var flags: int = ray["flags"]
			if flags & (128 | 4):
				continue
			var origin: Vector3 = ray["surface"] + ray["surface_normal"] * ray["offset"]
			var direction: Vector3 = ray["direction"]
			var slab_t := _slab_t(origin, direction, float(ray["tmax"]), slab)
			if slab_t < 0.0:
				continue
			var hit := (flags & 1) != 0 and float(ray["t"]) <= slab_t + STALE_HIT_TOLERANCE
			_stale_count(kind, origin, direction, level1_stale, not hit)
	if _stale_missed() > missed_before:
		_stale_stats["frames_with_misses"] += 1


## Where physics sees the ray meet the slab first, when it crosses the slab's core (its box shrunk by
## STALE_EDGE_MARGIN across its face, the thin z axis kept) for at least STALE_MIN_PATH. -1 otherwise.
func _slab_t(p_origin: Vector3, p_direction: Vector3, p_length: float, p_slab: AABB) -> float:
	var hit := _ray(p_origin, p_origin + p_direction * p_length)
	if hit.is_empty() or hit.get("collider") != _slab_body:
		return -1.0
	var core_min := p_slab.position + Vector3(STALE_EDGE_MARGIN, STALE_EDGE_MARGIN, 0.0)
	var core_max := p_slab.end - Vector3(STALE_EDGE_MARGIN, STALE_EDGE_MARGIN, 0.0)
	var enter := 0.0
	var leave := p_length
	for axis in 3:
		var d := p_direction[axis]
		if absf(d) < 1e-9:
			if p_origin[axis] < core_min[axis] or p_origin[axis] > core_max[axis]:
				return -1.0
			continue
		var t0 := (core_min[axis] - p_origin[axis]) / d
		var t1 := (core_max[axis] - p_origin[axis]) / d
		enter = maxf(enter, minf(t0, t1))
		leave = minf(leave, maxf(t0, t1))
	if leave - enter < STALE_MIN_PATH:
		return -1.0
	return p_origin.distance_to(hit["position"])


## Counts a ray physics sees cross the slab. A crossing ray starts outside level 0's region, where a
## coarser level marches it up to level 0's edge.
func _stale_count(p_kind: String, p_origin: Vector3, p_direction: Vector3, p_level1_stale: bool, p_missed: bool) -> void:
	var stats: Dictionary = _stale_stats["rays"][p_kind]
	var crossing := not _stale_region.has_point(p_origin)
	stats["slab"] += 1
	if crossing:
		stats["crossing"] += 1
		if p_level1_stale:
			stats["crossing_on_stale"] += 1
	if not p_missed:
		return
	stats["missed"] += 1
	if crossing:
		stats["missed_crossing"] += 1
	if _stale_stats["misses"].size() < 8:
		_stale_stats["misses"].append({ "kind": p_kind, "frame": _stale_last_frame, "level1_stale": p_level1_stale, "origin": str(p_origin), "direction": str(p_direction) })


func _stale_missed() -> int:
	var missed := 0
	for kind in _stale_stats["rays"]:
		missed += int(_stale_stats["rays"][kind]["missed"])
	return missed


func _finish_stale() -> void:
	_stale_stats["steps_mean"] = float(_stale_stats["steps_sum"]) / maxf(float(_stale_stats["frames"]), 1.0)
	_summary["stale"] = _stale_stats
	var missed := _stale_missed()
	var slab_rays := 0
	for kind in _stale_stats["rays"]:
		slab_rays += int(_stale_stats["rays"][kind]["slab"])
	if missed != 0:
		_reasons.append("stale: %d of %d rays that physics sees cross the slab passed it, on %d of %d frames with the slab in place" % [missed, slab_rays, _stale_stats["frames_with_misses"], _stale_stats["in_place_frames"]])
	if _stale_stats["skipped_frames"] != 0:
		_reasons.append("stale: %d readbacks were not the next frame's" % _stale_stats["skipped_frames"])
	if _stale_stats["hazard_frames"] < STALE_MIN_HAZARD_FRAMES:
		_reasons.append("stale: the slab was in place on %d frames level 1 skipped, not %d" % [_stale_stats["hazard_frames"], STALE_MIN_HAZARD_FRAMES])
	var crossing_on_stale := int(_stale_stats["rays"]["shadow"]["crossing_on_stale"])
	if crossing_on_stale < STALE_MIN_HAZARD_RAYS:
		_reasons.append("stale: %d shadow rays crossed into level 0 through the slab while level 1 was stale, not %d" % [crossing_on_stale, STALE_MIN_HAZARD_RAYS])
	_finish(_reasons.is_empty(), _reasons, _summary)


func _verify_exhausted(p_readback: Dictionary) -> void:
	var rays := _decode_rays(p_readback["gather"])
	var exhausted := 0
	var as_hits := 0
	for ray in rays:
		var flags: int = ray["flags"]
		if flags & 2:
			exhausted += 1
			if (flags & 1) and float(ray["t"]) >= 0.0 and float(ray["t"]) < float(ray["tmax"]):
				as_hits += 1
	_summary["exhausted_at_%d_steps" % EXHAUST_STEPS] = exhausted
	_summary["exhausted_as_hits"] = as_hits
	if exhausted < 100:
		_reasons.append("only %d rays ran out of %d steps" % [exhausted, EXHAUST_STEPS])
	if as_hits != exhausted:
		_reasons.append("%d of %d rays that ran out of steps were not reported as hits at their t" % [exhausted - as_hits, exhausted])


## The low-sun pass (stage 3): shadow rays toward a sun LOW_SUN_DEGREES above the horizon, and
## the gather rays that graze the open floor.
func _verify_low_sun(p_readback: Dictionary) -> void:
	var gather := _decode_rays(p_readback["gather"])
	var results: PackedByteArray = p_readback["shadow"]
	var to_light := -(p_readback["light_direction"] as Vector3).normalized()
	var compared := 0
	var agree := 0
	var open_lit := 0
	var false_shadows := 0
	var grazing := 0
	var false_hits := 0
	var grazing_exhausted := 0
	for index in gather.size():
		var ray: Dictionary = gather[index]
		var flags: int = ray["flags"]
		if flags & 128:
			continue
		var origin: Vector3 = ray["surface"] + ray["surface_normal"] * ray["offset"]
		var on_floor := (ray["surface_normal"] as Vector3).y > 0.99 and absf((ray["surface"] as Vector3).y) < 0.02
		var result := results.decode_u32(index * 4)
		if result != 2:
			var occluded := not _ray(origin, origin + to_light * 200.0).is_empty()
			compared += 1
			if (result == 0) == occluded:
				agree += 1
			# Lit open floor: nothing but the floor within LOW_SUN_CLEARANCE of the path to the sun.
			if on_floor and _open_path(origin, to_light, 200.0):
				open_lit += 1
				if result == 0:
					false_shadows += 1
		# Gather rays that graze the open floor: no hit (surface or out of steps) where physics
		# finds nothing near the path.
		var direction: Vector3 = ray["direction"]
		var elevation := rad_to_deg(asin(clampf(direction.y, -1.0, 1.0)))
		if on_floor and not (flags & 4) and elevation >= GRAZING_DEGREES.x and elevation <= GRAZING_DEGREES.y and _open_path(origin, direction, float(ray["tmax"])):
			grazing += 1
			if flags & 1:
				false_hits += 1
			if flags & 2:
				grazing_exhausted += 1
	_summary["low_sun"] = {
		"degrees": LOW_SUN_DEGREES,
		"shadow_compared": compared,
		"shadow_agree": agree,
		"open_floor_lit": open_lit,
		"false_shadows": false_shadows,
		"grazing_rays": grazing,
		"grazing_false_hits": false_hits,
		"grazing_exhausted": grazing_exhausted,
	}
	if compared < 1000 or agree < int(0.95 * compared):
		_reasons.append("low sun: %d of %d shadow rays agree with physics" % [agree, compared])
	if open_lit < 200 or false_shadows != 0:
		_reasons.append("low sun: %d of %d lit open-floor receivers read shadowed" % [false_shadows, open_lit])
	if grazing < 30 or false_hits != 0:
		_reasons.append("low sun: %d of %d gather rays grazing the open floor report a hit physics does not have" % [false_hits, grazing])


## True when no box but the floor lies within LOW_SUN_CLEARANCE of the segment, up to where
## it rises past every box.
func _open_path(p_from: Vector3, p_direction: Vector3, p_length: float) -> bool:
	var sphere := SphereShape3D.new()
	sphere.radius = LOW_SUN_CLEARANCE
	var query := PhysicsShapeQueryParameters3D.new()
	query.shape = sphere
	query.transform = Transform3D(Basis(), p_from)
	query.exclude = [_floor_body.get_rid()]
	# cast_motion() ignores the shapes the sphere starts in.
	if not _space.intersect_shape(query, 1).is_empty():
		return false
	var travelled := 0.0
	while travelled < p_length:
		var start := p_from + p_direction * travelled
		if p_direction.y > 0.0 and start.y > _boxes_top + LOW_SUN_CLEARANCE:
			return true
		var step := minf(OPEN_PATH_STEP, p_length - travelled)
		query.transform = Transform3D(Basis(), start)
		query.motion = p_direction * step
		var fractions := _space.cast_motion(query)
		if fractions.size() != 2 or fractions[0] < 1.0:
			return false
		travelled += step
	return true


## True when geometry lies within p_radius of p_point.
func _near_geometry(p_point: Vector3, p_radius: float) -> bool:
	var sphere := SphereShape3D.new()
	sphere.radius = p_radius
	var query := PhysicsShapeQueryParameters3D.new()
	query.shape = sphere
	query.transform = Transform3D(Basis(), p_point)
	return not _space.intersect_shape(query, 1).is_empty()


func _ray(p_from: Vector3, p_to: Vector3) -> Dictionary:
	var query := PhysicsRayQueryParameters3D.create(p_from, p_to)
	query.hit_back_faces = false
	return _space.intersect_ray(query)


func _level_voxel(p_distance_from_camera: float) -> float:
	var level := 0
	var half_extent := 6.4
	while level < 5 and p_distance_from_camera > half_extent * pow(2.0, level) - 0.4 * pow(2.0, level):
		level += 1
	return 0.1 * pow(2.0, level)


func _decode_rays(p_bytes: PackedByteArray) -> Array:
	var rays := []
	var count := p_bytes.size() / 80
	for index in count:
		var base := index * 80
		var ray := {}
		ray["surface"] = Vector3(p_bytes.decode_float(base), p_bytes.decode_float(base + 4), p_bytes.decode_float(base + 8))
		ray["offset"] = p_bytes.decode_float(base + 12)
		ray["surface_normal"] = Vector3(p_bytes.decode_float(base + 16), p_bytes.decode_float(base + 20), p_bytes.decode_float(base + 24))
		ray["roughness"] = p_bytes.decode_float(base + 28)
		ray["direction"] = Vector3(p_bytes.decode_float(base + 32), p_bytes.decode_float(base + 36), p_bytes.decode_float(base + 40))
		ray["tmax"] = p_bytes.decode_float(base + 44)
		ray["hit_normal"] = Vector3(p_bytes.decode_float(base + 48), p_bytes.decode_float(base + 52), p_bytes.decode_float(base + 56))
		ray["t"] = p_bytes.decode_float(base + 60)
		ray["flags"] = p_bytes.decode_u32(base + 76)
		rays.append(ray)
	return rays


var _dump := PackedStringArray()


## Compares one kind of traced ray with physics. Returns its statistics.
func _check_hits(p_rays: Array, p_camera: Vector3) -> Dictionary:
	# Screen-trace hits come from the hardware tier's Hi-Z march (24 samples, a 10% thickness
	# test): they count for hit or miss, not for distance and normal.
	var stats := { "traced": 0, "agree": 0, "sdf_hits": 0, "on_geometry": 0, "both_hit": 0, "distance_ok": 0, "normal_ok": 0, "exhausted": 0, "screen": 0, "screen_on_geometry": 0, "near": 0, "near_ok": 0 }
	for ray in p_rays:
		var flags: int = ray["flags"]
		if flags & 128:
			continue # Skipped: background or above the roughness gate.
		var direction: Vector3 = ray["direction"]
		var origin: Vector3 = ray["surface"] + ray["surface_normal"] * ray["offset"]
		var tmax: float = ray["tmax"]
		var expected := _ray(origin, origin + direction * tmax)
		if not expected.is_empty() and expected.get("collider") == null:
			expected = {}
		var sdf_hit := (flags & 1) != 0
		stats["traced"] += 1
		if flags & 2:
			stats["exhausted"] += 1
		var expected_hit := not expected.is_empty()
		var hit_point: Vector3 = origin + direction * float(ray["t"])
		var voxel_tolerance := 2.0 * _level_voxel(p_camera.distance_to(hit_point)) + 0.05
		if flags & 4:
			stats["screen"] += 1
			if _near_geometry(hit_point, 0.1 * float(ray["t"]) + 0.25):
				stats["screen_on_geometry"] += 1
		elif sdf_hit and not (flags & 2):
			# Every SDF hit lies on geometry, within the surface band of its level, and
			# past the origin offset (a hit at the start is the ray's own surface).
			stats["sdf_hits"] += 1
			if float(ray["t"]) > float(ray["offset"]) and _near_geometry(hit_point, voxel_tolerance):
				stats["on_geometry"] += 1
		if sdf_hit == expected_hit:
			stats["agree"] += 1
		# The detail range: a ray whose physics hit lies within 2 m of its origin hits it in
		# time (a hit within two voxels of its level, or a graze of an edge before it).
		if expected_hit and not (flags & 4) and origin.distance_to(expected["position"]) <= DETAIL_METERS:
			stats["near"] += 1
			if sdf_hit:
				var near_t: float = origin.distance_to(expected["position"])
				var near_tolerance := 2.0 * _level_voxel(p_camera.distance_to(expected["position"])) + 0.05
				var near_grazed := float(ray["t"]) > float(ray["offset"]) and float(ray["t"]) < near_t and _near_geometry(hit_point, voxel_tolerance)
				if absf(float(ray["t"]) - near_t) <= near_tolerance or near_grazed:
					stats["near_ok"] += 1
		if OS.get_cmdline_user_args().has("--dump"):
			_dump.append("%s flags=%d t=%.3f expected=%s n=%s en=%s o=%s d=%s tmax=%.2f" % [str(sdf_hit == expected_hit), flags, float(ray["t"]), str(origin.distance_to(expected["position"])) if expected_hit else "miss", str(ray["hit_normal"]), str(expected.get("normal", Vector3())), str(origin), str(direction), tmax])
		if sdf_hit and expected_hit and not (flags & 2) and not (flags & 4):
			stats["both_hit"] += 1
			var expected_t: float = origin.distance_to(expected["position"])
			var tolerance := 2.0 * _level_voxel(p_camera.distance_to(expected["position"])) + 0.05
			var on_time := absf(float(ray["t"]) - expected_t) <= tolerance
			# A ray that grazes an edge within the surface band stops there, earlier than physics.
			# A hit within the origin offset is a self-intersection, never a graze.
			var grazed := float(ray["t"]) > float(ray["offset"]) and float(ray["t"]) < expected_t and _near_geometry(hit_point, voxel_tolerance)
			if on_time or grazed:
				stats["distance_ok"] += 1
			var normal: Vector3 = ray["hit_normal"]
			if not on_time or normal.dot(expected["normal"]) >= 0.7:
				stats["normal_ok"] += 1
	return stats


func _verify(p_info: Dictionary, p_readback: Dictionary) -> void:
	var reasons := PackedStringArray()
	var summary := {}
	var tier := str(p_info.get("tier", ""))
	summary["tier"] = tier
	summary["detail"] = _detail
	summary["levels"] = int(p_readback.get("levels", 0))
	summary["level_bricks"] = Array(p_readback.get("level_bricks", PackedInt32Array()))
	summary["instances"] = int(p_info.get("swrt_trace_instances", 0))
	if tier != "SWRT":
		reasons.append("tier is %s, not SWRT" % tier)
	summary["ray_query_shader_versions"] = int(p_info.get("ray_query_shader_versions", -1))
	if int(ProjectSettings.get_setting("rendering/global_illumination/nextgi/hardware_ray_tracing", 0)) == 1 and int(p_info.get("ray_query_shader_versions", -1)) != 0:
		reasons.append("%d ray-query ShaderRD versions were created in a process started with hardware_ray_tracing Off" % int(p_info.get("ray_query_shader_versions", -1)))
	# GodotNext: ps_swrt_trace (#186, #188). debug/deny_ray_query: a device without ray query.
	if bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false)):
		summary["reason"] = str(p_info.get("reason", ""))
		summary["rt_arena_bytes"] = int(p_info.get("rt_arena_bytes", -1))
		if str(p_info.get("reason", "")) != "NO_RAY_QUERY":
			reasons.append("deny_ray_query: the tier's reason is %s, expected NO_RAY_QUERY" % str(p_info.get("reason", "")))
		if int(p_info.get("rt_arena_bytes", -1)) != 0:
			reasons.append("deny_ray_query: the RT arena holds %d bytes" % int(p_info.get("rt_arena_bytes", -1)))
		if int(p_info.get("ray_query_shader_versions", -1)) != 0:
			reasons.append("deny_ray_query: %d ray-query ShaderRD versions were created" % int(p_info.get("ray_query_shader_versions", -1)))
		if int(p_info.get("swrt_trace_instances", 0)) <= 0:
			reasons.append("deny_ray_query: no SWRT instances (no runtime mesh SDFs from the CPU copies)")
	var grid: Vector2i = p_readback["grid"]
	var screen: Vector2i = p_readback["screen"]
	var camera: Transform3D = p_readback["camera"]
	var viewport_size := Vector2(get_viewport().get_visible_rect().size)
	var scale := viewport_size / Vector2(screen)
	var gather := _decode_rays(p_readback["gather"])
	if gather.size() != grid.x * grid.y:
		reasons.append("gather readback has %d rays for a %s grid" % [gather.size(), str(grid)])
		_reasons.append_array(reasons)
		_summary.merge(summary, true)
		return

	# 1. World reconstruction: the depth-reconstructed surface is where the camera ray hits.
	var surfaces := 0
	var surface_ok := 0
	var worst_surface := 0.0
	for index in gather.size():
		var ray: Dictionary = gather[index]
		if int(ray["flags"]) & 128:
			continue
		var cell := Vector2i(index % grid.x, index / grid.x)
		var pixel := Vector2i(clampi(int((cell.x + 0.5) * float(screen.x) / float(grid.x)), 0, screen.x - 1), clampi(int((cell.y + 0.5) * float(screen.y) / float(grid.y)), 0, screen.y - 1))
		var screen_point := (Vector2(pixel) + Vector2(0.5, 0.5)) * scale
		var from := _camera.project_ray_origin(screen_point)
		var hit := _ray(from, from + _camera.project_ray_normal(screen_point) * 200.0)
		if hit.is_empty():
			continue
		surfaces += 1
		var error: float = (ray["surface"] as Vector3).distance_to(hit["position"])
		worst_surface = maxf(worst_surface, error)
		if error <= 0.02 + 0.004 * camera.origin.distance_to(hit["position"]):
			surface_ok += 1
	summary["surfaces"] = surfaces
	summary["surfaces_ok"] = surface_ok
	summary["worst_surface_m"] = worst_surface
	if surfaces < gather.size() / 2:
		reasons.append("only %d of %d grid cells reconstruct a surface" % [surfaces, gather.size()])
	if surface_ok < int(0.97 * surfaces):
		reasons.append("%d of %d reconstructed surfaces are off the geometry (worst %.3f m)" % [surfaces - surface_ok, surfaces, worst_surface])

	# 2. Gather rays against physics.
	var gather_stats := _check_hits(gather, camera.origin)
	summary["gather"] = gather_stats
	_require_hits("gather", gather_stats, reasons)

	# 3. Reflection rays: the roughness gate, the mirror direction, the hits.
	if p_readback.has("reflect"):
		var reflect := _decode_rays(p_readback["reflect"])
		var mirror := 0
		var mirror_ok := 0
		var rough := 0
		var rough_skipped := 0
		for ray in reflect:
			var surface: Vector3 = ray["surface"]
			if surface == Vector3.ZERO:
				continue
			var to_surface := surface - camera.origin
			var hit := _ray(camera.origin, camera.origin + to_surface * 1.01)
			if hit.is_empty():
				continue
			var roughness: float = _roughness_of.get(hit["collider_id"], -1.0)
			if roughness >= 0.9:
				rough += 1
				if int(ray["flags"]) & 128:
					rough_skipped += 1
			elif roughness <= 0.0 and not (int(ray["flags"]) & 128):
				mirror += 1
				var view := (camera.origin - surface).normalized()
				# Godot's Vector3.reflect() is GLSL's reflect(-view, normal).
				var expected_direction := view.reflect(hit["normal"])
				if (ray["direction"] as Vector3).dot(expected_direction) >= 0.98:
					mirror_ok += 1
		summary["reflect_mirror"] = mirror
		summary["reflect_mirror_ok"] = mirror_ok
		summary["reflect_rough"] = rough
		summary["reflect_rough_skipped"] = rough_skipped
		if mirror < 50 or mirror_ok < int(0.98 * mirror):
			reasons.append("mirror-floor reflections: %d of %d follow the mirror direction" % [mirror_ok, mirror])
		if rough < 10 or rough_skipped != rough:
			reasons.append("rough cube: %d of %d reflection rays skipped by the roughness gate" % [rough_skipped, rough])
		var reflect_stats := _check_hits(reflect, camera.origin)
		summary["reflect"] = reflect_stats
		_require_hits("reflect", reflect_stats, reasons)
	else:
		reasons.append("no reflection readback (reflections or the normal-roughness prepass missing)")

	# 4. SDF shadow rays toward the sun.
	if p_readback.has("shadow"):
		var results: PackedByteArray = p_readback["shadow"]
		var light_travel: Vector3 = p_readback["light_direction"]
		var to_light := -light_travel.normalized()
		var compared := 0
		var agree := 0
		var expected_lit := 0
		var expected_shadow := 0
		for index in gather.size():
			var ray: Dictionary = gather[index]
			if int(ray["flags"]) & 128:
				continue
			var result := results.decode_u32(index * 4)
			if result == 2:
				continue
			var origin: Vector3 = ray["surface"] + ray["surface_normal"] * ray["offset"]
			var occluded := not _ray(origin, origin + to_light * 200.0).is_empty()
			if OS.get_cmdline_user_args().has("--dump"):
				_dump.append("shadow result=%d occluded=%s o=%s n=%s" % [result, str(occluded), str(origin), str(ray["surface_normal"])])
			compared += 1
			if occluded:
				expected_shadow += 1
			else:
				expected_lit += 1
			if (result == 0) == occluded:
				agree += 1
		summary["shadow_compared"] = compared
		summary["shadow_agree"] = agree
		summary["shadow_expected_lit"] = expected_lit
		summary["shadow_expected_occluded"] = expected_shadow
		if compared < 1000 or expected_shadow < 50 or expected_lit < 50:
			reasons.append("shadow rays: %d compared, %d lit and %d occluded expected" % [compared, expected_lit, expected_shadow])
		if agree < int(0.95 * compared):
			reasons.append("shadow rays: %d of %d agree with physics" % [agree, compared])
	else:
		reasons.append("no shadow readback (no directional light reached the trace)")

	# 5. #182 coverage words of the two-sided panel.
	summary["coverage_checked"] = int(p_readback.get("coverage_checked", 0))
	summary["coverage_expected_set"] = int(p_readback.get("coverage_expected_set", 0))
	summary["coverage_mismatches"] = int(p_readback.get("coverage_mismatches", -1))
	if summary["coverage_expected_set"] <= 0 or summary["coverage_mismatches"] != 0:
		reasons.append("coverage words: %d expected bits, %d mismatches" % [summary["coverage_expected_set"], summary["coverage_mismatches"]])
	if int(p_info.get("swrt_trace_two_sided", 0)) < 1:
		reasons.append("the two-sided panel never entered the SWRT scene")

	# 6. #187 auto quality: a GPU trace of the fixture (one CPU ray took about 1 us) and,
	# on Metal, the GPU-core count from the IORegistry.
	summary["benchmark_usec"] = int(p_readback.get("benchmark_usec", 0))
	summary["compute_units"] = int(p_readback.get("compute_units", 0))
	if summary["benchmark_usec"] < 50:
		reasons.append("the SWRT microbenchmark took %d us; it does not trace on the GPU" % summary["benchmark_usec"])
	if str(p_readback.get("driver", "")) == "Metal" and summary["compute_units"] <= 0:
		reasons.append("Metal reported no GPU-core count")
	# The benchmark times a trace that sees its room: its fixture segments (lit or crossing a
	# box) agree with the fixture's boxes. Its room is built from non-uniformly scaled boxes.
	summary["benchmark_fixture_checked"] = int(p_readback.get("benchmark_fixture_checked", 0))
	summary["benchmark_fixture_agreed"] = int(p_readback.get("benchmark_fixture_agreed", 0))
	if summary["benchmark_fixture_checked"] < 200 or summary["benchmark_fixture_agreed"] < int(0.95 * summary["benchmark_fixture_checked"]):
		reasons.append("microbenchmark fixture: %d of %d segments agree with its boxes" % [summary["benchmark_fixture_agreed"], summary["benchmark_fixture_checked"]])

	# 8. The global SDF composite (GPU) against the CPU reference, at random points of level 0.
	summary["scaled"] = _scaled
	summary["sheared"] = _sheared
	summary["composite_checked"] = int(p_readback.get("composite_checked", 0))
	summary["composite_max_error"] = float(p_readback.get("composite_max_error", 1e30))
	summary["composite_sign_mismatches"] = int(p_readback.get("composite_sign_mismatches", -1))
	if summary["composite_checked"] < 1000 or summary["composite_max_error"] > COMPOSITE_TOLERANCE or summary["composite_sign_mismatches"] != 0:
		reasons.append("global SDF composite vs CPU: %d checked, max error %.3f m, %d sign mismatches" % [summary["composite_checked"], summary["composite_max_error"], summary["composite_sign_mismatches"]])
	# Every mesh here is a box, whose CPU distance is exact: a resident voxel that reads more
	# than a quarter voxel past the nearest instance (listed in its brick or not) lets rays
	# step through that instance.
	summary["composite_overestimates"] = int(p_readback.get("composite_overestimates", -1))
	if summary["composite_overestimates"] != 0:
		reasons.append("global SDF composite: %d voxels read farther than the nearest instance" % summary["composite_overestimates"])

	summary["dense"] = _dense
	if _detail and int(p_info.get("swrt_detail_hits", 0)) <= 0:
		reasons.append("detail mode made no mesh-SDF hits")
	summary["detail_hits"] = int(p_info.get("swrt_detail_hits", 0))
	# 7. Detail froxels: the cull lists instances, and in the dense scene finds more than it can list.
	summary["froxel_marked"] = int(p_readback.get("froxel_marked", 0))
	summary["froxel_listed"] = int(p_readback.get("froxel_listed", 0))
	summary["froxel_overflow"] = int(p_readback.get("froxel_overflow", 0))
	summary["froxel_most_found"] = int(p_readback.get("froxel_most_found", 0))
	if _detail and (summary["froxel_marked"] <= 0 or summary["froxel_listed"] <= 0):
		reasons.append("detail froxels: %d marked, %d with instances" % [summary["froxel_marked"], summary["froxel_listed"]])
	if _detail and _dense and summary["froxel_most_found"] <= FROXEL_CAP:
		reasons.append("dense scene: no detail froxel found more than %d instances (most %d)" % [FROXEL_CAP, summary["froxel_most_found"]])
	summary["counters"] = {
		"gather_rays": int(p_info.get("swrt_gather_rays", 0)),
		"gather_hits": int(p_info.get("swrt_gather_hits", 0)),
		"reflect_rays": int(p_info.get("swrt_reflect_rays", 0)),
		"shadow_traced": int(p_info.get("swrt_shadow_traced", 0)),
		"exhausted": int(p_info.get("swrt_exhausted", 0)),
		# #257: the verification rays' steps, and the empty-space steps the global SDF's coarse
		# bound took without a fine sample.
		"steps": int(p_info.get("swrt_steps", -1)),
		"coarse_steps": int(p_info.get("swrt_coarse_steps", -1)),
	}
	if int(ProjectSettings.get_setting(COARSE_LEVELS_SETTING, -1)) == -1 and summary["counters"]["coarse_steps"] <= 0:
		reasons.append("the trace took no coarse-bound step (%d of %d steps)" % [summary["counters"]["coarse_steps"], summary["counters"]["steps"]])
	# #257: the tracer's own coarse bound on every resident level, against its CPU twin and the
	# nearest instance.
	summary["coarse_levels_checked"] = int(p_readback.get("coarse_levels_checked", -1))
	summary["coarse_checked"] = int(p_readback.get("coarse_checked", -1))
	summary["coarse_overestimates"] = int(p_readback.get("coarse_overestimates", -1))
	summary["coarse_twin_mismatches"] = int(p_readback.get("coarse_twin_mismatches", -1))
	summary["coarse_levels_fresh"] = int(p_readback.get("coarse_levels_fresh", -1))
	if summary["coarse_levels_checked"] != summary["coarse_levels_fresh"] or summary["coarse_levels_checked"] < int(p_info.get("swrt_trace_levels", 0)) or summary["coarse_levels_checked"] <= 0:
		reasons.append("coarse bound: %d of %d levels checked (%d fresh)" % [summary["coarse_levels_checked"], int(p_info.get("swrt_trace_levels", 0)), summary["coarse_levels_fresh"]])
	if summary["coarse_overestimates"] != 0 or summary["coarse_twin_mismatches"] != 0:
		reasons.append("coarse bound: %d of %d points read past the nearest instance, %d cells differ from the CPU twin" % [summary["coarse_overestimates"], summary["coarse_checked"], summary["coarse_twin_mismatches"]])
	_reasons.append_array(reasons)
	_summary.merge(summary, true)


func _require_hits(p_kind: String, p_stats: Dictionary, r_reasons: PackedStringArray) -> void:
	var traced: int = p_stats["traced"]
	if traced < 1000:
		r_reasons.append("%s: only %d rays traced" % [p_kind, traced])
		return
	if p_stats["agree"] < int(0.9 * traced):
		r_reasons.append("%s: %d of %d rays agree with physics on hit or miss" % [p_kind, p_stats["agree"], traced])
	if p_stats["both_hit"] < 150:
		r_reasons.append("%s: only %d rays hit where physics hits" % [p_kind, p_stats["both_hit"]])
	if p_stats["on_geometry"] < int(0.97 * p_stats["sdf_hits"]):
		r_reasons.append("%s: %d of %d SDF hits lie on geometry" % [p_kind, p_stats["on_geometry"], p_stats["sdf_hits"]])
	if p_stats["distance_ok"] < int(0.95 * p_stats["both_hit"]):
		r_reasons.append("%s: %d of %d hit distances within two voxels" % [p_kind, p_stats["distance_ok"], p_stats["both_hit"]])
	if p_stats["normal_ok"] < int(0.9 * p_stats["both_hit"]):
		r_reasons.append("%s: %d of %d hit normals within 45 degrees" % [p_kind, p_stats["normal_ok"], p_stats["both_hit"]])
	# The Hi-Z pyramid is sampled nearest, as the hardware tier does: filtering it blends
	# near and far depths at silhouettes into phantom surfaces (76% on geometry here).
	if p_stats["screen"] > 0 and p_stats["screen_on_geometry"] < int(0.85 * p_stats["screen"]):
		r_reasons.append("%s: %d of %d screen-trace hits are on geometry" % [p_kind, p_stats["screen_on_geometry"], p_stats["screen"]])
	if p_stats["exhausted"] > int(MAX_EXHAUSTED_FRACTION * traced):
		r_reasons.append("%s: %d of %d rays ran out of steps" % [p_kind, p_stats["exhausted"], traced])
	if p_stats["near"] < 30 or p_stats["near_ok"] < int(0.9 * p_stats["near"]):
		r_reasons.append("%s: %d of %d rays with a hit within %.0f m hit it in time" % [p_kind, p_stats["near_ok"], p_stats["near"], DETAIL_METERS])
