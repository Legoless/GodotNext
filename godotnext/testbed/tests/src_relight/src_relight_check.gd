extends Node

## NextGI SRC world-space relight check (#31, D14).
##
## Renders --frames frames (600 by default) of --scene and checks the High caps: at most 256k
## relit texels and 256k traced shadow rays per frame, at most 8 shadowed lights per page and at
## most 2 mask evaluations. Extra modes:
##   --probes          compare page radiance with the camera aimed toward and away from the light
##   --shadow-fixture  a 20 m ground plate, a 2 m block on it and a 4 x 1 x 4 m slab floating 1 m
##                     above it at z -7.25, under a downward sun; the ground card's mask matches
##                     both analytic footprints. The block's sits in the sun's shadow map
##                     (directional_shadow_max_distance 4), where relight reuses the map; the
##                     slab's lies beyond it, more than 6 m from the camera, so only relight's own
##                     shadow rays can shadow it (#186 step 5 on the software tier). At least
##                     FAR_INSIDE_MIN traced texels inside the slab's footprint must be checked.
##                     A second slab's footprint lies 4.5 to 6 m away (#258): beyond the shadow
##                     distance but inside its last split's reach, so relight must trace it too.
##                     The edge band (two mask texels around each footprint's edge) is skipped.
##   --ceiling-fixture a 20 m floor under a 20 m ceiling plate whose underside is at y 3, a shadowed
##                     omni light below the ceiling and no positional shadow atlas, so relight traces
##                     every texel. Nothing lies between the floor and the light, so every texel of
##                     the floor card's mask must be lit, near the light and beyond 5 m alike. Three
##                     rows, each running --frames frames:
##                       gap 0.12 m, camera at (0, 1.5, 4)  — the light is in clipmap level 0
##                       gap 0.05 m, camera at (0, 1.5, 12) — level 1's 10 cm band
##                       gap 0.12 m, camera at (0, 1.5, 20) — level 2's 20 cm band
##                       gap 0.12 m, camera at (0, 1.5, 4), a 2 x 1 x 2 m box under the light —
##                       its footprint unlit, the floor around it lit
##                     On the software tier a positional light's shadow rays march the mesh SDFs
##                     of the instances inside its range (#186 step 5), so every floor texel must
##                     be lit on every row, whatever clipmap level holds the light. The hardware tier
##                     must light every texel on the 12 cm rows. Its parallel ray is offset by at
##                     most 5 cm and is not re-aimed, so the 5 cm row is not required on HWRT.
##                     Pass --ceiling-gap and --ceiling-camera-z together to run one row.
##   --light-lists     STATIC / DYNAMIC / DISABLED tracking for visibility, scenario and bake mode
## --expected-tier=HWRT (default) or SWRT: the tier the run must report (run_src_relight_check.sh
## passes it from --hwrt). Under debug/deny_ray_query (a device without ray query) the run also
## asserts NO_RAY_QUERY, no RT arena, no ray-query ShaderRD and SWRT instances. Prints one
## "GODOTNEXT_RESULT <json>" line.

const TEXEL_CAP := 256 * 1024
const RAY_CAP := 256 * 1024
## Traced mask texels inside the floating slab's footprint (the hardware tier checks 81 at High).
const FAR_INSIDE_MIN := 40
## The shadow fixture's floating slab: center and size.
const SLAB_CENTER := Vector3(0, 1.5, -7.25)
const SLAB_SIZE := Vector3(4, 1, 4)
## A second slab whose footprint lies 4.5 to 6 m from the camera (#258): beyond the sun's 4 m
## shadow distance, but inside the reach of its last split's shadow camera, whose shadow pass culls
## occluders outside the split's slice. Relight must trace these texels, not reuse the map.
const MID_SLAB_CENTER := Vector3(2.4, 1.0, -0.4)
const MID_SLAB_SIZE := Vector3(2.4, 0.5, 2.4)
## Traced mask texels inside the mid slab's footprint that must be checked.
const MID_INSIDE_MIN := 8
## The ceiling fixture: the ceiling plate's underside and the level-0 row's gap.
const CEILING_Y := 3.0
const CEILING_LIGHT_GAP := 0.12
## The occluder row: a 2 x 1 x 2 m box hanging under the light (y 1.3 to 2.3). Its shadow on the
## floor is the square max(|x|, |z|) < 1 / (1 - 2.3 / (CEILING_Y - CEILING_LIGHT_GAP)) = 4.97 m
## (its top face's silhouette). Texels inside CEILING_SHADOW_INNER must be unlit, texels between
## CEILING_SHADOW_OUTER and CEILING_LIT_RADIUS lit; the band between is not checked. The box is a
## meter thick because a mesh SDF rounds corners by about a voxel (UE documents the same for its
## mesh distance fields): a 20 cm slab's rounded top edge let steep rays through 0.8 m inside its
## footprint, which measures SDF resolution rather than whether the path occludes.
const CEILING_OCCLUDER_CENTER := Vector3(0, 1.8, 0)
const CEILING_OCCLUDER_SIZE := Vector3(2, 1, 2)
const CEILING_SHADOW_INNER := 4.0
const CEILING_SHADOW_OUTER := 5.8
const CEILING_LIT_RADIUS := 10.0

var _expected_tier := "HWRT"
var _ceiling_gap := -1.0
var _ceiling_camera_z := -1.0
var _ceiling_row_gap := CEILING_LIGHT_GAP
var _ceiling_row_z := 4.0
var _ceiling_row_occluder := false

var _failures := PackedStringArray()


func _ready() -> void:
	var result := await run(OS.get_cmdline_user_args())
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures.append(what)


func run(args: PackedStringArray) -> Dictionary:
	var scene_path := "res://scenes/color_box_room/cbr_a.tscn"
	var frames := 600
	var probes := false
	var shadow_fixture := false
	var ceiling_fixture := false
	var light_lists := false
	for arg in args:
		if arg.begins_with("--scene="):
			scene_path = arg.trim_prefix("--scene=")
		elif arg.begins_with("--frames="):
			frames = int(arg.trim_prefix("--frames="))
		elif arg == "--probes":
			probes = true
		elif arg == "--shadow-fixture":
			shadow_fixture = true
		elif arg == "--ceiling-fixture":
			ceiling_fixture = true
		elif arg.begins_with("--ceiling-gap="):
			_ceiling_gap = float(arg.trim_prefix("--ceiling-gap="))
		elif arg.begins_with("--ceiling-camera-z="):
			_ceiling_camera_z = float(arg.trim_prefix("--ceiling-camera-z="))
		elif arg == "--light-lists":
			light_lists = true
		elif arg.begins_with("--expected-tier="):
			_expected_tier = arg.trim_prefix("--expected-tier=")

	var enabled: bool = ProjectSettings.get_setting("rendering/global_illumination/nextgi/enabled", false)
	var root := Node3D.new()
	add_child(root)
	if shadow_fixture:
		_build_shadow_fixture(root)
		scene_path = "shadow-fixture"
	elif ceiling_fixture:
		scene_path = "ceiling-fixture"
	elif light_lists:
		_build_light_list_scene(root)
		scene_path = "light-lists"
	else:
		var packed: PackedScene = load(scene_path)
		_check(packed != null, "scene loads: " + scene_path)
		if packed == null:
			return _result(scene_path, enabled, {})
		root.add_child(packed.instantiate())
	_prepare(root)

	var max_texels := 0
	var max_rays := 0
	var max_shadowed := 0
	var max_evals := 0
	var info: Dictionary = {}
	if probes:
		return await _run_probes(root, scene_path, enabled, frames)
	if light_lists:
		return await _run_light_lists(root, scene_path, enabled)

	if ceiling_fixture:
		for row in _ceiling_rows():
			_clear_children(root)
			_ceiling_row_gap = float(row["gap"])
			_ceiling_row_z = float(row["z"])
			_ceiling_row_occluder = bool(row.get("occluder", false))
			_build_ceiling_fixture(root, _ceiling_row_gap, _ceiling_row_z, _ceiling_row_occluder)
			_prepare(root)
			info = {}
			for frame in frames:
				await get_tree().process_frame
				info = RenderingServer.nextgi_get_tier_info()
				if info.get("tier", "") != _expected_tier:
					continue
				max_texels = maxi(max_texels, int(info.get("src_relight_texels", 0)))
				max_rays = maxi(max_rays, int(info.get("src_relight_rays", 0)))
				max_shadowed = maxi(max_shadowed, int(info.get("src_max_shadowed_lights", 0)))
				max_evals = maxi(max_evals, int(info.get("src_max_mask_evals", 0)))
			_check(info.get("tier", "") == _expected_tier, "ceiling gap %.3f camera z %.1f: %s tier, got %s" % [_ceiling_row_gap, _ceiling_row_z, _expected_tier, str(info.get("tier", ""))])
			_check_ceiling_mask(info)
	else:
		for frame in frames:
			await get_tree().process_frame
			info = RenderingServer.nextgi_get_tier_info()
			if info.get("tier", "") != _expected_tier:
				continue
			max_texels = maxi(max_texels, int(info.get("src_relight_texels", 0)))
			max_rays = maxi(max_rays, int(info.get("src_relight_rays", 0)))
			max_shadowed = maxi(max_shadowed, int(info.get("src_max_shadowed_lights", 0)))
			max_evals = maxi(max_evals, int(info.get("src_max_mask_evals", 0)))
		_check(info.get("tier", "") == _expected_tier, "%s tier, got %s" % [_expected_tier, str(info.get("tier", ""))])
		if shadow_fixture:
			_check_shadow_mask(info)
	_check(max_texels > 0, "at least one relit texel")
	_check(max_texels <= TEXEL_CAP, "relit texels %d stay within 256k" % max_texels)
	_check(max_rays <= RAY_CAP, "traced rays %d stay within 256k" % max_rays)
	_check(max_shadowed <= 8, "shadowed lights per page %d" % max_shadowed)
	_check(max_evals <= 2, "mask evaluations %d" % max_evals)
	# GodotNext: ps_swrt_trace (#186, #188). debug/deny_ray_query: a device without ray query.
	if bool(ProjectSettings.get_setting("rendering/global_illumination/nextgi/debug/deny_ray_query", false)):
		print("SRC_RELIGHT_DENY reason=%s rt_arena_bytes=%d ray_query_shader_versions=%d swrt_trace_instances=%d" % [str(info.get("reason", "")), int(info.get("rt_arena_bytes", -1)), int(info.get("ray_query_shader_versions", -1)), int(info.get("swrt_trace_instances", 0))])
		_check(str(info.get("reason", "")) == "NO_RAY_QUERY", "deny_ray_query: the tier's reason is %s, expected NO_RAY_QUERY" % str(info.get("reason", "")))
		_check(int(info.get("rt_arena_bytes", -1)) == 0, "deny_ray_query: the RT arena holds %d bytes" % int(info.get("rt_arena_bytes", -1)))
		_check(int(info.get("ray_query_shader_versions", -1)) == 0, "deny_ray_query: %d ray-query ShaderRD versions were created" % int(info.get("ray_query_shader_versions", -1)))
		_check(int(info.get("swrt_trace_instances", 0)) > 0, "deny_ray_query: no SWRT instances (no runtime mesh SDFs from the CPU copies)")
	var result := _result(scene_path, enabled, info)
	result["max_texels"] = max_texels
	result["max_rays"] = max_rays
	result["max_shadowed"] = max_shadowed
	result["max_evals"] = max_evals
	return result


func _run_probes(root: Node3D, scene_path: String, enabled: bool, frames: int) -> Dictionary:
	var toward := root.find_child("LeakCamLit", true, false) as Camera3D
	var away := root.find_child("LeakCamDark", true, false) as Camera3D
	_check(toward != null and away != null, "toward and away cameras")
	if toward == null or away == null:
		return _result(scene_path, enabled, {})
	toward.current = true
	for frame in frames:
		await get_tree().process_frame
	var first: PackedFloat32Array = RenderingServer.nextgi_get_tier_info().get("src_page_probes", PackedFloat32Array())
	away.current = true
	toward.current = false
	for frame in frames:
		await get_tree().process_frame
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	var second: PackedFloat32Array = info.get("src_page_probes", PackedFloat32Array())
	_check(first.size() > 0 and first.size() == second.size(), "probe readback on both cameras")
	var lit := 0
	for i in mini(first.size(), second.size()):
		var scale := maxf(first[i], second[i])
		var delta := absf(first[i] - second[i])
		var ok := delta <= 0.01 if scale < 0.02 else delta <= 0.01 * scale
		if not ok:
			_check(false, "probe %d changed from %.4f to %.4f" % [i, first[i], second[i]])
			break
		if scale >= 0.02:
			lit += 1
	_check(lit > 0, "a lit page was compared")
	return _result(scene_path, enabled, info)


func _run_light_lists(root: Node3D, scene_path: String, enabled: bool) -> Dictionary:
	var static_light := root.get_node("StaticLight") as OmniLight3D
	var dynamic_light := root.get_node("DynamicLight") as OmniLight3D
	var disabled_light := root.get_node("DisabledLight") as OmniLight3D
	for frame in 30:
		await get_tree().process_frame
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("src_light_static_count", 0)) == 1, "one static light, got %s" % str(info.get("src_light_static_count", 0)))
	_check(int(info.get("src_light_dynamic_count", 0)) == 1, "one dynamic light")
	_check(int(info.get("src_light_directional_count", 0)) == 0, "the disabled directional is absent")
	disabled_light.light_bake_mode = Light3D.BAKE_DYNAMIC
	for frame in 8:
		await get_tree().process_frame
	info = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("src_light_dynamic_count", 0)) == 2, "enabling the disabled light adds a dynamic light")
	disabled_light.light_bake_mode = Light3D.BAKE_DISABLED
	static_light.visible = false
	for frame in 8:
		await get_tree().process_frame
	info = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("src_light_static_count", 0)) == 0, "hiding the static light drops it")
	_check(int(info.get("src_light_dynamic_count", 0)) == 1, "DISABLED stays out and the dynamic light remains, count %s" % str(info.get("src_light_count", 0)))
	var holder := Node3D.new()
	root.remove_child(dynamic_light)
	holder.add_child(dynamic_light)
	for frame in 8:
		await get_tree().process_frame
	info = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("src_light_dynamic_count", 0)) == 0, "leaving the scenario drops the dynamic light")
	holder.remove_child(dynamic_light)
	root.add_child(dynamic_light)
	dynamic_light.light_bake_mode = Light3D.BAKE_STATIC
	for frame in 8:
		await get_tree().process_frame
	info = RenderingServer.nextgi_get_tier_info()
	_check(int(info.get("src_light_static_count", 0)) >= 1, "re-entering as STATIC puts the light on the static list")
	_check(int(info.get("src_light_dynamic_count", 0)) == 0, "the bake-mode change left the dynamic list")
	return _result(scene_path, enabled, info)


func _check_shadow_mask(info: Dictionary) -> void:
	var pages: Array = info.get("src_mask_pages", [])
	_check(not pages.is_empty(), "shadow mask readback")
	var page: Dictionary = {}
	var best_y := 1000.0
	for candidate in pages:
		var normal: Vector3 = _world_normal(candidate)
		if normal.y < 0.5:
			continue
		var center := _mask_world(candidate, int(candidate["mask_w"]) / 2, int(candidate["mask_h"]) / 2)
		if center.y < best_y:
			best_y = center.y
			page = candidate
	_check(best_y < 0.5, "the ground card faces up at y %.2f" % best_y)
	if page.is_empty():
		return
	var mask: PackedByteArray = page["mask"]
	var mask_w := int(page["mask_w"])
	var mask_h := int(page["mask_h"])
	var radiance_w := maxi(int(page["radiance_w"]), 1)
	var half_u := float(page["half_u"])
	var half_v := float(page["half_v"])
	var texel_u := (2.0 * half_u) / float(radiance_w) * float(radiance_w) / float(maxi(mask_w, 1))
	var texel_v := (2.0 * float(page["half_v"])) / float(maxi(int(page["radiance_h"]), 1)) * float(maxi(int(page["radiance_h"]), 1)) / float(maxi(mask_h, 1))
	# One mask texel of the shadow edge, plus the texel center, is the allowed band.
	var margin := maxf(absf(texel_u), absf(texel_v)) * 2.0
	var camera := Vector3(0, 3, 3)
	# The footprints on the ground: the block's (center, half extents) and the slab's.
	var footprints := [[Vector2(0, 0), Vector2(1, 1)], [Vector2(SLAB_CENTER.x, SLAB_CENTER.z), Vector2(SLAB_SIZE.x, SLAB_SIZE.z) * 0.5], [Vector2(MID_SLAB_CENTER.x, MID_SLAB_CENTER.z), Vector2(MID_SLAB_SIZE.x, MID_SLAB_SIZE.z) * 0.5]]
	var near_checked := 0
	var far_checked := 0
	var far_inside := 0
	var near_bad := 0
	var far_bad := 0
	var far_bad_inside := 0
	# GodotNext: #258. Beyond the 4 m shadow distance but nearer than the far band.
	var mid_checked := 0
	var mid_inside := 0
	var mid_bad := 0
	var nonzero := 0
	var sample := Vector3.ZERO
	var bad_samples := PackedStringArray()
	for y in mask_h:
		for x in mask_w:
			var world := _mask_world(page, x, y)
			if x == mask_w / 2 and y == mask_h / 2:
				sample = world
			if (mask[y * mask_w + x] & 1) != 0:
				nonzero += 1
			var inside := false
			var edge := false
			for footprint in footprints:
				var offset := Vector2(world.x, world.z) - (footprint[0] as Vector2)
				var half: Vector2 = footprint[1]
				inside = inside or (absf(offset.x) <= half.x and absf(offset.y) <= half.y)
				# The band around this footprint's edge, two mask texels wide on either side.
				var near_x := absf(absf(offset.x) - half.x) <= margin and absf(offset.y) <= half.y + margin
				var near_z := absf(absf(offset.y) - half.y) <= margin and absf(offset.x) <= half.x + margin
				edge = edge or near_x or near_z
			if edge:
				continue
			var lit := (mask[y * mask_w + x] & 1) != 0
			var distance := world.distance_to(camera)
			if distance < 3.0:
				near_checked += 1
				if lit == inside:
					near_bad += 1
			elif distance > 4.2 and distance <= 6.0:
				mid_checked += 1
				if inside:
					mid_inside += 1
				if lit == inside:
					mid_bad += 1
					if bad_samples.size() < 8:
						bad_samples.append("mid (%.2f, %.2f) lit=%s inside=%s" % [world.x, world.z, lit, inside])
			elif distance > 6.0:
				far_checked += 1
				if inside:
					far_inside += 1
				if lit == inside:
					far_bad += 1
					if inside:
						far_bad_inside += 1
					if bad_samples.size() < 8:
						bad_samples.append("(%.2f, %.2f) lit=%s inside=%s" % [world.x, world.z, lit, inside])
	print("SRC_RELIGHT_SHADOW tier=%s mask=%dx%d margin=%.3f near_checked=%d near_bad=%d far_checked=%d far_inside=%d far_bad=%d far_bad_inside=%d mid_checked=%d mid_inside=%d mid_bad=%d lit=%d bad=%s" % [str(info.get("tier", "")), mask_w, mask_h, margin, near_checked, near_bad, far_checked, far_inside, far_bad, far_bad_inside, mid_checked, mid_inside, mid_bad, nonzero, bad_samples])
	_check(near_checked > 0, "shadow-map texels were checked (near %d far %d lit %d sample %s)" % [near_checked, far_checked, nonzero, sample])
	_check(far_checked > 0, "traced texels beyond the shadow distance were checked")
	_check(far_inside >= FAR_INSIDE_MIN, "%d traced texels inside the slab's footprint were checked, expected at least %d" % [far_inside, FAR_INSIDE_MIN])
	_check(near_bad == 0, "%d shadow-map texels disagree with the analytic footprint" % near_bad)
	_check(far_bad == 0, "%d traced texels disagree with the analytic footprints (%d inside one)" % [far_bad, far_bad_inside])
	_check(mid_inside >= MID_INSIDE_MIN, "%d texels inside the mid slab's footprint were checked, expected at least %d" % [mid_inside, MID_INSIDE_MIN])
	_check(mid_bad == 0, "%d texels beyond the shadow distance disagree with the analytic footprints" % mid_bad)


func _check_ceiling_mask(info: Dictionary) -> void:
	var pages: Array = info.get("src_mask_pages", [])
	_check(not pages.is_empty(), "shadow mask readback")
	var page: Dictionary = {}
	var best_y := 1000.0
	for candidate in pages:
		if _world_normal(candidate).y < 0.5:
			continue
		var center := _mask_world(candidate, int(candidate["mask_w"]) / 2, int(candidate["mask_h"]) / 2)
		if center.y < best_y:
			best_y = center.y
			page = candidate
	_check(best_y < 0.5, "the floor card faces up at y %.2f" % best_y)
	if page.is_empty():
		return
	var mask: PackedByteArray = page["mask"]
	var mask_w := int(page["mask_w"])
	var mask_h := int(page["mask_h"])
	if _ceiling_row_occluder:
		_check_ceiling_occluder(info, page, mask, mask_w, mask_h)
		return
	# Texels by their horizontal distance from the light: under 2 m, 2 to 5 m and beyond 5 m.
	var lit := [0, 0, 0]
	var total := [0, 0, 0]
	var bad_samples := PackedStringArray()
	var unlit_near := 1.0e9
	var unlit_far := -1.0
	for y in mask_h:
		for x in mask_w:
			var world := _mask_world(page, x, y)
			var r := Vector2(world.x, world.z).length()
			var bucket := 0 if r < 2.0 else (1 if r < 5.0 else 2)
			total[bucket] += 1
			if (mask[y * mask_w + x] & 1) != 0:
				lit[bucket] += 1
			else:
				unlit_near = minf(unlit_near, r)
				unlit_far = maxf(unlit_far, r)
				if bad_samples.size() < 8:
					bad_samples.append("(%.2f, %.2f) r=%.2f" % [world.x, world.z, r])
	var lit_sum := int(lit[0]) + int(lit[1]) + int(lit[2])
	var total_sum := int(total[0]) + int(total[1]) + int(total[2])
	var unlit_span := ""
	if unlit_far >= 0.0:
		unlit_span = " unlit_r=%.2f..%.2f" % [unlit_near, unlit_far]
	print("SRC_RELIGHT_CEILING tier=%s gap=%.3f camera_z=%.1f mask=%dx%d floor_y=%.2f lit %d/%d (r<2 %d/%d, 2-5 %d/%d, >5 %d/%d)%s unlit=%s" % [str(info.get("tier", "")), _ceiling_row_gap, _ceiling_row_z, mask_w, mask_h, best_y, lit_sum, total_sum, lit[0], total[0], lit[1], total[1], lit[2], total[2], unlit_span, bad_samples])
	# The hardware relight keeps a parallel ray from an origin offset of at most 0.05 m and
	# does not re-aim it, so a 5 cm gap lets that ray enter the ceiling. Full lighting is
	# required on the software tier for every row, and on the hardware tier for the 12 cm rows.
	var require_full := _expected_tier != "HWRT" or _ceiling_row_gap > 0.05
	for bucket in 3:
		_check(total[bucket] > 0, "floor texels in distance band %d were checked (gap %.3f, camera z %.1f)" % [bucket, _ceiling_row_gap, _ceiling_row_z])
		if require_full:
			_check(lit[bucket] == total[bucket], "%d of %d floor texels in distance band %d are lit at gap %.3f m with the camera %.1f m away" % [lit[bucket], total[bucket], bucket, _ceiling_row_gap, _ceiling_row_z])


# The occluder row: the box's footprint is unlit and the floor around it lit, on both tiers. On
# the software tier the omni's rays march the mesh SDFs of the instances inside its range (#186
# step 5), so this row shows that path still occludes.
func _check_ceiling_occluder(info: Dictionary, page: Dictionary, mask: PackedByteArray, mask_w: int, mask_h: int) -> void:
	var inside_unlit := 0
	var inside_total := 0
	var outside_lit := 0
	var outside_total := 0
	var bad_samples := PackedStringArray()
	for y in mask_h:
		for x in mask_w:
			var world := _mask_world(page, x, y)
			var square := maxf(absf(world.x), absf(world.z))
			var lit := (mask[y * mask_w + x] & 1) != 0
			if square < CEILING_SHADOW_INNER:
				inside_total += 1
				if not lit:
					inside_unlit += 1
				elif bad_samples.size() < 8:
					bad_samples.append("lit inside (%.2f, %.2f)" % [world.x, world.z])
			elif square > CEILING_SHADOW_OUTER and Vector2(world.x, world.z).length() < CEILING_LIT_RADIUS:
				outside_total += 1
				if lit:
					outside_lit += 1
				elif bad_samples.size() < 8:
					bad_samples.append("unlit outside (%.2f, %.2f)" % [world.x, world.z])
	print("SRC_RELIGHT_CEILING_OCCLUDER tier=%s footprint unlit %d/%d, around lit %d/%d bad=%s" % [str(info.get("tier", "")), inside_unlit, inside_total, outside_lit, outside_total, bad_samples])
	_check(inside_total > 0 and outside_total > 0, "occluder row checked texels inside (%d) and around (%d) the footprint" % [inside_total, outside_total])
	_check(inside_unlit == inside_total, "%d of %d floor texels in the occluder's footprint are unlit" % [inside_unlit, inside_total])
	_check(outside_lit == outside_total, "%d of %d floor texels around the occluder's footprint are lit" % [outside_lit, outside_total])


func _mask_world(page: Dictionary, x: int, y: int) -> Vector3:
	var radiance_w := float(maxi(int(page["radiance_w"]), 1))
	var radiance_h := float(maxi(int(page["radiance_h"]), 1))
	var mask_w := float(maxi(int(page["mask_w"]), 1))
	var mask_h := float(maxi(int(page["mask_h"]), 1))
	var texel_x := float(x) * radiance_w / mask_w
	var texel_y := float(y) * radiance_h / mask_h
	var xu := ((texel_x + 0.5) / radiance_w * 2.0 - 1.0) * float(page["half_u"])
	var yv := ((texel_y + 0.5) / radiance_h * 2.0 - 1.0) * float(page["half_v"])
	var origin: Vector3 = page["origin"]
	var axis_u: Vector3 = page["axis_u"]
	var axis_v: Vector3 = page["axis_v"]
	var object := origin + axis_u * xu + axis_v * yv
	var rows: PackedFloat32Array = page["world"]
	return Vector3(
			rows[0] * object.x + rows[1] * object.y + rows[2] * object.z + rows[3],
			rows[4] * object.x + rows[5] * object.y + rows[6] * object.z + rows[7],
			rows[8] * object.x + rows[9] * object.y + rows[10] * object.z + rows[11])


func _world_normal(page: Dictionary) -> Vector3:
	var axis_n: Vector3 = page["axis_n"]
	var rows: PackedFloat32Array = page["world"]
	return Vector3(
			rows[0] * axis_n.x + rows[1] * axis_n.y + rows[2] * axis_n.z,
			rows[4] * axis_n.x + rows[5] * axis_n.y + rows[6] * axis_n.z,
			rows[8] * axis_n.x + rows[9] * axis_n.y + rows[10] * axis_n.z).normalized()


func _prepare(node: Node) -> void:
	var stack: Array[Node] = [node]
	while not stack.is_empty():
		var current: Node = stack.pop_back()
		if current is WorldEnvironment:
			var world_environment := current as WorldEnvironment
			if world_environment.environment != null:
				var environment := world_environment.environment.duplicate()
				environment.nextgi_enabled = true
				world_environment.environment = environment
		elif current is OmniLight3D:
			(current as OmniLight3D).shadow_enabled = true
		elif current is DirectionalLight3D:
			(current as DirectionalLight3D).shadow_enabled = true
		elif current is GeometryInstance3D:
			(current as GeometryInstance3D).gi_mode = GeometryInstance3D.GI_MODE_STATIC
		for child in current.get_children():
			stack.append(child)


func _build_shadow_fixture(root: Node3D) -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	root.add_child(world)
	var ground := MeshInstance3D.new()
	var ground_mesh := BoxMesh.new()
	ground_mesh.size = Vector3(20, 0.2, 20)
	ground.mesh = ground_mesh
	ground.position = Vector3(0, -0.1, 0)
	root.add_child(ground)
	var block := MeshInstance3D.new()
	var block_mesh := BoxMesh.new()
	block_mesh.size = Vector3(2, 2, 2)
	block.mesh = block_mesh
	block.position = Vector3(0, 1, 0)
	root.add_child(block)
	# Beyond the shadow map's reach (and its reuse region), so only traced shadow rays shadow its
	# footprint.
	var slab := MeshInstance3D.new()
	var slab_mesh := BoxMesh.new()
	slab_mesh.size = SLAB_SIZE
	slab.mesh = slab_mesh
	slab.position = SLAB_CENTER
	root.add_child(slab)
	var mid_slab := MeshInstance3D.new()
	var mid_mesh := BoxMesh.new()
	mid_mesh.size = MID_SLAB_SIZE
	mid_slab.mesh = mid_mesh
	mid_slab.position = MID_SLAB_CENTER
	root.add_child(mid_slab)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-90, 0, 0)
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 4.0
	sun.shadow_bias = 0.1
	root.add_child(sun)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 3, 3)
	camera.look_at_from_position(camera.position, Vector3(0, 0, 1), Vector3.UP)
	camera.current = true
	root.add_child(camera)


func _ceiling_rows() -> Array:
	if _ceiling_gap >= 0.0 or _ceiling_camera_z >= 0.0:
		return [{
			"gap": _ceiling_gap if _ceiling_gap >= 0.0 else CEILING_LIGHT_GAP,
			"z": _ceiling_camera_z if _ceiling_camera_z >= 0.0 else 4.0,
		}]
	return [
		{"gap": CEILING_LIGHT_GAP, "z": 4.0},
		{"gap": 0.05, "z": 12.0},
		{"gap": 0.12, "z": 20.0},
		{"gap": CEILING_LIGHT_GAP, "z": 4.0, "occluder": true},
	]


func _clear_children(node: Node) -> void:
	for child in node.get_children():
		node.remove_child(child)
		child.free()


func _build_ceiling_fixture(root: Node3D, gap: float, camera_z: float, occluder := false) -> void:
	# No positional shadow atlas: relight cannot reuse the omni light's shadow map and traces.
	get_viewport().positional_shadow_atlas_size = 0
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	root.add_child(world)
	for plate_y in [-0.1, CEILING_Y + 0.1]:
		var plate := MeshInstance3D.new()
		var plate_mesh := BoxMesh.new()
		plate_mesh.size = Vector3(20, 0.2, 20)
		plate.mesh = plate_mesh
		plate.position = Vector3(0, plate_y, 0)
		root.add_child(plate)
	if occluder:
		var box := MeshInstance3D.new()
		var box_mesh := BoxMesh.new()
		box_mesh.size = CEILING_OCCLUDER_SIZE
		box.mesh = box_mesh
		box.position = CEILING_OCCLUDER_CENTER
		root.add_child(box)
	var light := OmniLight3D.new()
	light.position = Vector3(0, CEILING_Y - gap, 0)
	light.omni_range = 12.0
	light.shadow_enabled = true
	light.light_bake_mode = Light3D.BAKE_DYNAMIC
	root.add_child(light)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 1.5, camera_z)
	camera.look_at_from_position(camera.position, Vector3.ZERO, Vector3.UP)
	camera.current = true
	root.add_child(camera)


func _build_light_list_scene(root: Node3D) -> void:
	var environment := Environment.new()
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	root.add_child(world)
	var mesh_instance := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(2, 2, 2)
	mesh_instance.mesh = box
	root.add_child(mesh_instance)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 2, 4)
	camera.look_at_from_position(camera.position, Vector3.ZERO, Vector3.UP)
	camera.current = true
	root.add_child(camera)
	_add_omni(root, "StaticLight", Light3D.BAKE_STATIC, Vector3(3, 2, 0))
	_add_omni(root, "DynamicLight", Light3D.BAKE_DYNAMIC, Vector3(-3, 2, 0))
	_add_omni(root, "DisabledLight", Light3D.BAKE_DISABLED, Vector3(0, 2, 3))


func _add_omni(root: Node3D, node_name: String, bake: Light3D.BakeMode, position: Vector3) -> void:
	var light := OmniLight3D.new()
	light.name = node_name
	light.light_bake_mode = bake
	light.omni_range = 12.0
	light.position = position
	light.shadow_enabled = false
	root.add_child(light)


func _result(scene_path: String, enabled: bool, info: Dictionary) -> Dictionary:
	return {
		"pass": _failures.is_empty(),
		"scene": scene_path,
		"device": RenderingServer.get_video_adapter_name(),
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"nextgi_enabled": enabled,
		"tier": info.get("tier", ""),
		"failures": _failures,
	}
