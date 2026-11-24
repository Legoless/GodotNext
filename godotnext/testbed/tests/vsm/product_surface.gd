extends Node3D

## Virtual Shadow Map product surface (#221; plan §21.3.2 S1, budget V6).
##
## Loads a scene (ShadowGallery by default; --scene=res://... for another, such as the PV smoke scene
## res://scenes/vsm_flythrough/vsm_flythrough.tscn) and checks, after --warmup frames:
## 1. RenderingServer.virtual_shadow_maps_get_tier_info(): with --expect-active=true the tier is available
##    and the core is active; with --expect-active=false nothing is active. --expect-reason=<NAME> checks
##    the tier reason (NONE on Forward+, UNSUPPORTED_RENDERER on Mobile), and the count of fallback lines is
##    1 for a Virtual project below the tier and 0 otherwise. (Light3D's configuration warning, which names
##    the reason, is not callable from scripts; the [VSM] doctests check it.)
## 2. RENDERING_INFO_VSM_PAGES_* (read back a few frames late) and the matching Performance monitors: pages
##    allocated, requested and re-rendered while active, all 0 while inactive.
## 3. The six Viewport.DEBUG_DRAW_VSM_* draws (Forward+ only), each saved as a PNG with --out=<dir>. While
##    active, each draw is checked by its colors: VSM_MASK holds lit and shadowed pixels; VSM_MIP holds a mip
##    color and at most 0.5% unmapped (magenta) pixels; VSM_PAGES holds at least 16 page colors;
##    VSM_CACHE is mostly cached (green) on a static frame and shows rendered (red) pages on a frame after
##    every shadow was off for one frame, when every page is new; VSM_OVERDRAW shows draws on that frame;
##    VSM_CLIPMAP holds level colors. While inactive, each draw shows the normal image: it changes less than
##    10% of it (the gallery's wave animates; an active draw changes about 80%).
##    While active, VSM_OVERDRAW is then checked on a view without non-Nanite casters (every
##    GeometryInstance3D's cast_shadow off) right after a frame with casters in which every page was new: the
##    view's pages show no draws (at most 1% of the image differs from the normal image without casters and
##    is not the "no draws" color), so the draw does not show an earlier view's caster draws.
## 4. With --presets=true: rendering/lights_and_shadows/virtual_shadows/quality is switched through Low,
##    Medium, High and Epic while running. Each new pool's driver memory equals budget V6 within 1 MiB,
##    the stats count pages in it, and a non-Nanite raster check passes on it.
## 5. With --mixed=true: a shadowed OmniLight3D or SpotLight3D in the camera's view is set to
##    Light3D.SHADOW_METHOD_ATLAS and another one to SHADOW_METHOD_VIRTUAL. The atlas light has no virtual
##    maps: setting it to PROJECT, RAY_TRACED (takes RT when HWRT is active, or falls back to PROJECT),
##    DISTANCE_FIELD (takes DF when mesh SDFs are enabled, or falls back to PROJECT when disabled) and
##    RAY_TRACED again updates its map count accordingly, and ATLAS after each removes it. Zero
##    unimplemented-method warnings are emitted. The non-Nanite raster check
##    (virtual_shadow_maps_get_raster_check_result()) passes with the mixed light set.
## 6. With --expect-headless=true (run with --headless in a Virtual project): the dummy renderer draws no
##    shadows, so after one shadowed light is set to SHADOW_METHOD_VIRTUAL and another to RAY_TRACED, no
##    fallback line and no unimplemented-method warning has been printed, and nothing is active. Only this
##    check runs.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const SceneArgs = preload("res://scenes/scene_args.gd")

const DRAWS := [
	["vsm_mask", Viewport.DEBUG_DRAW_VSM_MASK],
	["vsm_mip", Viewport.DEBUG_DRAW_VSM_MIP],
	["vsm_pages", Viewport.DEBUG_DRAW_VSM_PAGES],
	["vsm_cache", Viewport.DEBUG_DRAW_VSM_CACHE],
	["vsm_overdraw", Viewport.DEBUG_DRAW_VSM_OVERDRAW],
	["vsm_clipmap", Viewport.DEBUG_DRAW_VSM_CLIPMAP],
]
# The colors vsm_debug.glsl draws (alpha 0.85 over the image, so each channel lands within 0.15).
const MIP_COLORS := [Color(0.9, 0.1, 0.1), Color(0.95, 0.5, 0.1), Color(0.95, 0.9, 0.1), Color(0.2, 0.8, 0.2), Color(0.1, 0.8, 0.85), Color(0.15, 0.3, 0.95), Color(0.6, 0.2, 0.85)]
const CACHE_RENDERED := Color(0.95, 0.15, 0.1)
const CACHE_DYNAMIC := Color(0.15, 0.3, 0.95)
const CACHE_CACHED := Color(0.15, 0.85, 0.2)
const OVERDRAW_NONE := Color(0.05, 0.05, 0.2)
const OVERDRAW_NONE_EDGE := Color(0.02, 0.02, 0.08) # OVERDRAW_NONE on a darkened page edge.
const UNMAPPED := Color(1.0, 0.0, 1.0)
const COLOR_TOLERANCE := 0.17
const MAX_UNMAPPED := 0.005
const MAX_INACTIVE_CHANGE := 0.1
# Draws whose palette has no magenta, so a magenta pixel there is an unmapped page.
const UNMAPPED_DRAWS := ["vsm_mask", "vsm_mip", "vsm_cache", "vsm_overdraw"]
const MIN_SHARE := 0.01
const V6_MIB := [64, 128, 256, 512]
const QUALITY_NAMES := ["low", "medium", "high", "epic"]
const QUALITY_SETTING := "rendering/lights_and_shadows/virtual_shadows/quality"
const DRAW_FRAMES := 3
const STATS_FRAMES := 8
const RASTER_WAIT_FRAMES := 10
const TOGGLE_FRAMES := 3
# The atlas light's methods in the mixed check, each followed by ATLAS again.
const MIXED_METHODS := [Light3D.SHADOW_METHOD_PROJECT, Light3D.SHADOW_METHOD_RAY_TRACED, Light3D.SHADOW_METHOD_DISTANCE_FIELD, Light3D.SHADOW_METHOD_RAY_TRACED]
const MIXED_METHOD_NAMES := ["project", "ray_traced", "distance_field", "ray_traced_again"]
const MAX_NO_CASTER_DRAWN := 0.01

var _expect_active := false
var _expect_reason := ""
var _presets := false
var _mixed := false
var _headless := false
var _warmup := 30
var _out := ""
var _scene_path := "res://scenes/shadow_gallery/shadow_gallery.tscn"
var _forward_plus := true

var _frame := 0
var _stage := "warmup"
var _stage_frame := 0
var _draw_index := 0
var _draw_step := 0
var _preset_index := 0
var _preset_check := {}
var _mixed_counts := {}
var _mixed_step := 0
var _mixed_next_frame := 0
var _mixed_warnings_before := -1
var _casters: Array[GeometryInstance3D] = []
var _caster_settings: Array[int] = []
var _no_caster_image: Image
var _baseline: Image
var _atlas_light: Light3D
var _virtual_light: Light3D
var _shadowed_lights := 0
var _shadowed: Array[Light3D] = []
var _failures := PackedStringArray()
var _result := {}


func _ready() -> void:
	_expect_active = SceneArgs.value("--expect-active", "true") == "true"
	_expect_reason = SceneArgs.value("--expect-reason", "")
	_presets = SceneArgs.value("--presets", "false") == "true"
	_mixed = SceneArgs.value("--mixed", "false") == "true"
	_headless = SceneArgs.value("--expect-headless", "false") == "true"
	_warmup = SceneArgs.int_value("--warmup", 30)
	_out = SceneArgs.value("--out", "")
	_scene_path = SceneArgs.value("--scene", _scene_path)
	_forward_plus = RenderingServer.get_current_rendering_method() == "forward_plus"
	_result["expect_active"] = _expect_active
	_result["scene"] = _scene_path
	_result["method"] = RenderingServer.get_current_rendering_method()
	_result["driver"] = RenderingServer.get_current_rendering_driver_name()
	_result["device"] = RenderingServer.get_video_adapter_name()
	_result["shadow_method_setting"] = ProjectSettings.get_setting("rendering/lights_and_shadows/shadow_method", 0)
	var packed := load(_scene_path) as PackedScene
	if packed == null:
		_failures.append("could not load %s" % _scene_path)
		return
	var root := packed.instantiate() as Node3D
	# Static scenes: the gallery's and the flythrough's own scripts stay idle.
	root.set_meta("skip_runtime", true)
	# The gallery's wave lifts its flat plane by up to 0.12 m in VERTEX code, outside its box, and declares
	# no margin, so the non-Nanite raster check would count its vertices outside the cull box; give it the
	# margin tests/vsm/non_nanite.gd gives it.
	var wave := root.get_node_or_null("CasterVertexAnimated") as GeometryInstance3D
	if wave != null:
		wave.extra_cull_margin = 0.15
	add_child(root)
	_collect_casters(root)
	var lights: Array[Light3D] = []
	_collect_lights(root, lights)
	for light in lights:
		if light.shadow_enabled:
			_shadowed_lights += 1
			_shadowed.append(light)
	_result["shadowed_lights"] = _shadowed_lights
	if _headless:
		if _shadowed.size() < 2:
			_failures.append("--expect-headless needs two shadowed lights")
		else:
			_shadowed[0].shadow_method = Light3D.SHADOW_METHOD_VIRTUAL
			_shadowed[1].shadow_method = Light3D.SHADOW_METHOD_RAY_TRACED
		return
	if _mixed:
		# Lights in view, so that each has virtual maps whenever its method resolves to VIRTUAL.
		var camera := get_viewport().get_camera_3d()
		for light in lights:
			if not light.shadow_enabled or light is DirectionalLight3D:
				continue
			if camera == null or not camera.is_position_in_frustum(light.global_position):
				continue
			if _atlas_light == null:
				_atlas_light = light
				light.shadow_method = Light3D.SHADOW_METHOD_ATLAS
			elif _virtual_light == null:
				_virtual_light = light
				light.shadow_method = Light3D.SHADOW_METHOD_VIRTUAL
				break
		if _atlas_light == null or _virtual_light == null:
			_failures.append("--mixed needs two shadowed omni or spot lights in view")
		_result["atlas_light"] = String(_atlas_light.name) if _atlas_light != null else ""
		_result["virtual_light"] = String(_virtual_light.name) if _virtual_light != null else ""


func _set_shadows(p_enabled: bool) -> void:
	for light in _shadowed:
		light.shadow_enabled = p_enabled


func _collect_casters(node: Node) -> void:
	if node is GeometryInstance3D and (node as GeometryInstance3D).cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF:
		_casters.append(node as GeometryInstance3D)
		_caster_settings.append(int((node as GeometryInstance3D).cast_shadow))
	for child in node.get_children():
		_collect_casters(child)


func _set_casters(p_enabled: bool) -> void:
	for i in _casters.size():
		if p_enabled:
			_casters[i].cast_shadow = _caster_settings[i] as GeometryInstance3D.ShadowCastingSetting
		else:
			_casters[i].cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


func _collect_lights(node: Node, lights: Array[Light3D]) -> void:
	if node is Light3D:
		lights.append(node as Light3D)
	for child in node.get_children():
		_collect_lights(child, lights)


func _process(_delta: float) -> void:
	_frame += 1
	_stage_frame += 1
	if not _failures.is_empty() and _stage == "warmup":
		_finish()
		return
	match _stage:
		"warmup":
			if _stage_frame >= _warmup:
				if _headless:
					_check_headless()
					_next_stage("done")
					return
				_check_tier_and_stats()
				_next_stage("baseline" if _forward_plus else "done")
		"baseline":
			if _stage_frame >= DRAW_FRAMES:
				_baseline = get_viewport().get_texture().get_image()
				_save(_baseline, "normal")
				_draw_index = 0
				_draw_step = 0
				get_viewport().debug_draw = DRAWS[0][1]
				_next_stage("draws")
		"draws":
			_step_draws()
		"overdraw_no_casters":
			_step_overdraw_no_casters()
		"mixed_toggle":
			_step_mixed_toggle()
		"mixed_off":
			_set_shadows(true)
			RenderingServer.virtual_shadow_maps_request_raster_check()
			_next_stage("mixed")
		"mixed":
			var check := RenderingServer.virtual_shadow_maps_get_raster_check_result()
			if bool(check.get("captured", false)) or _stage_frame >= RASTER_WAIT_FRAMES:
				_check_mixed(check)
				_after_mixed()
		"presets":
			_step_presets()
		"done":
			_finish()


func _next_stage(p_stage: String) -> void:
	_stage = p_stage
	_stage_frame = 0


# Each draw: DRAW_FRAMES frames, then a capture of a static frame. VSM_CACHE and VSM_OVERDRAW also capture the
# frame after one frame without shadows, in which every page the view requests is new and rendered.
func _step_draws() -> void:
	var draw_name: String = DRAWS[_draw_index][0]
	var dirty_draw := _expect_active and (draw_name == "vsm_cache" or draw_name == "vsm_overdraw")
	match _draw_step:
		0:
			if _stage_frame < DRAW_FRAMES:
				return
			_check_draw(draw_name, get_viewport().get_texture().get_image(), false)
			if dirty_draw:
				_set_shadows(false)
				_draw_step = 1
				_stage_frame = 0
				return
		1:
			_set_shadows(true)
			_draw_step = 2
			_stage_frame = 0
			return
		2:
			# This frame's image is the one rendered right after the shadows came back.
			_check_draw(draw_name, get_viewport().get_texture().get_image(), true)
	_draw_index += 1
	_draw_step = 0
	_stage_frame = 0
	if _draw_index < DRAWS.size():
		get_viewport().debug_draw = DRAWS[_draw_index][1]
		return
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	_check_draw_summary()
	if _expect_active:
		_next_stage("overdraw_no_casters")
	else:
		_after_draws()


func _after_draws() -> void:
	if _mixed and _expect_active and _atlas_light != null:
		_next_stage("mixed_toggle")
	else:
		_after_mixed()


# VSM_OVERDRAW: a frame with casters in which every page is new (its pairs fill the pair list), then a frame
# without casters in which every page is new again, then the normal image without casters to compare with.
func _step_overdraw_no_casters() -> void:
	match _stage_frame:
		1:
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_VSM_OVERDRAW
			_set_shadows(false)
		2:
			_set_shadows(true)
		3:
			# Frame 2's image: casters, every page new.
			_check_draw("vsm_overdraw", get_viewport().get_texture().get_image(), true, "vsm_overdraw_with_casters")
			_set_casters(false)
			_set_shadows(false)
		4:
			_set_shadows(true)
		5:
			# Frame 4's image: no casters, every page new.
			_no_caster_image = get_viewport().get_texture().get_image()
			get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
		6:
			var normal := get_viewport().get_texture().get_image()
			_save(normal, "normal_no_casters")
			_check_overdraw_no_casters(_no_caster_image, normal)
			_set_casters(true)
			_after_draws()


func _check_overdraw_no_casters(p_image: Image, p_normal: Image) -> void:
	_save(p_image, "vsm_overdraw_no_casters")
	if p_image == null or p_normal == null or p_image.get_size() != p_normal.get_size():
		_failures.append("vsm_overdraw_no_casters: no image to compare")
		return
	var total := 0
	var none := 0
	var drawn := 0
	for y in range(0, p_image.get_height(), 2):
		for x in range(0, p_image.get_width(), 2):
			var a := p_image.get_pixel(x, y)
			var b := p_normal.get_pixel(x, y)
			total += 1
			if _near(a, OVERDRAW_NONE) or _near(a, OVERDRAW_NONE_EDGE):
				none += 1
			elif absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > 0.02 and not _near(a, UNMAPPED):
				drawn += 1
	var shares := {"none": float(none) / maxf(float(total), 1.0), "drawn": float(drawn) / maxf(float(total), 1.0), "casters_off": _casters.size()}
	if not _result.has("draws"):
		_result["draws"] = {}
	_result["draws"]["vsm_overdraw_no_casters"] = shares
	if _casters.is_empty():
		_failures.append("vsm_overdraw_no_casters: the scene has no shadow casters to turn off")
	if shares["none"] < MIN_SHARE:
		_failures.append("vsm_overdraw_no_casters shows no page without draws: %s" % str(shares))
	if shares["drawn"] > MAX_NO_CASTER_DRAWN:
		_failures.append("vsm_overdraw_no_casters shows caster draws on a view without casters: %s" % str(shares))


# The atlas light: each of MIXED_METHODS for TOGGLE_FRAMES frames, then ATLAS again for as many.
func _step_mixed_toggle() -> void:
	if _stage_frame == 1:
		_mixed_counts["atlas"] = _light_map_count()
		_mixed_warnings_before = _unimplemented_warnings()
		_mixed_step = 0
	elif _stage_frame < _mixed_next_frame:
		return
	var index: int = _mixed_step >> 1
	if _mixed_step % 2 == 0:
		if _mixed_step > 0:
			_mixed_counts["atlas_after_" + String(MIXED_METHOD_NAMES[index - 1])] = _light_map_count()
		if index >= MIXED_METHODS.size():
			_check_mixed_counts()
			# One frame without shadows frees every light's maps, so the captured frame draws every page.
			_set_shadows(false)
			_next_stage("mixed_off")
			return
		_atlas_light.shadow_method = MIXED_METHODS[index]
	else:
		_mixed_counts[String(MIXED_METHOD_NAMES[index])] = _light_map_count()
		_atlas_light.shadow_method = Light3D.SHADOW_METHOD_ATLAS
	_mixed_step += 1
	_mixed_next_frame = _stage_frame + TOGGLE_FRAMES


func _check_mixed_counts() -> void:
	var warnings := _unimplemented_warnings() - _mixed_warnings_before
	var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
	var df_available := bool(tier.get("df_available", false))
	var rt_available := bool(tier.get("rt_available", false))
	_result["mixed_light_counts"] = _mixed_counts
	_result["mixed_unimplemented_warnings"] = warnings
	var n := int(_mixed_counts["atlas"])
	for method_name: String in MIXED_METHOD_NAMES:
		var skips_vsm := (method_name == "distance_field" and df_available) or (method_name == "ray_traced" and rt_available)
		var expected_active := n if skips_vsm else n + 1
		if int(_mixed_counts.get(method_name, -1)) != expected_active or int(_mixed_counts.get("atlas_after_" + method_name, -1)) != n:
			_failures.append("lights with virtual maps while the atlas light is ATLAS, %s, ATLAS: %s; expected %d, %d, %d" % [method_name.to_upper(), str(_mixed_counts), n, expected_active, n])
	if warnings != 0:
		_failures.append("the unimplemented-method warnings rose by %d; expected 0 (all ShadowMethod values are implemented)" % warnings)


func _unimplemented_warnings() -> int:
	return int(RenderingServer.virtual_shadow_maps_get_tier_info().get("unimplemented_warnings", -1))


func _light_map_count() -> int:
	return int(RenderingServer.virtual_shadow_maps_get_debug_info().get("lights", 0))


func _after_mixed() -> void:
	if _presets and _expect_active:
		_preset_index = 0
		_next_stage("presets")
		_set_quality(0)
	else:
		_next_stage("done")


func _check_tier_and_stats() -> void:
	var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
	_result["tier"] = tier
	var stats := _read_stats()
	_result["stats"] = stats
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	_result["debug_info"] = {
		"active": info.get("active", false),
		"quality": info.get("quality", ""),
		"pool_pages": info.get("pool_pages", 0),
		"pool_driver_bytes": info.get("pool_driver_bytes", 0),
		"lights": info.get("lights", 0),
		"lod_bias": str(info.get("lod_bias", Vector2i())),
		"lod_bias_moving": str(info.get("lod_bias_moving", Vector2i())),
		"tier_reason": info.get("tier_reason", ""),
	}
	var reason := String(tier.get("reason", ""))
	if not _expect_reason.is_empty() and reason != _expect_reason:
		_failures.append("the tier reason is %s; expected %s" % [reason, _expect_reason])
	var project_virtual := String(tier.get("project_shadow_method", "")) == "virtual"
	var expected_lines := 1 if (project_virtual and reason != "NONE") else 0
	if int(tier.get("fallback_lines", -1)) != expected_lines:
		_failures.append("%d fallback lines were printed; expected %d" % [int(tier.get("fallback_lines", -1)), expected_lines])
	if _expect_active:
		if not bool(tier.get("available", false)) or reason != "NONE":
			_failures.append("the VSM tier is not available: %s" % reason)
		if not bool(tier.get("active", false)):
			_failures.append("virtual shadow maps are not active")
		_check_stats_counting(stats, int(info.get("pool_pages", 0)), "warmup")
	else:
		if bool(tier.get("active", false)):
			_failures.append("virtual shadow maps are active with --expect-active=false")
		for key in stats:
			if int(stats[key]) != 0:
				_failures.append("%s is %d while inactive" % [key, int(stats[key])])


func _check_headless() -> void:
	var tier := RenderingServer.virtual_shadow_maps_get_tier_info()
	_result["tier"] = tier
	_result["display_server"] = DisplayServer.get_name()
	if DisplayServer.get_name() != "headless":
		_failures.append("--expect-headless needs --headless (display server %s)" % DisplayServer.get_name())
	if String(tier.get("project_shadow_method", "")) != "virtual":
		_failures.append("--expect-headless needs a Virtual project: %s" % str(tier.get("project_shadow_method", "")))
	if int(tier.get("fallback_lines", -1)) != 0:
		_failures.append("the dummy renderer printed %d fallback lines; expected 0" % int(tier.get("fallback_lines", -1)))
	if int(tier.get("unimplemented_warnings", -1)) != 0:
		_failures.append("the dummy renderer printed %d unimplemented-method warnings; expected 0" % int(tier.get("unimplemented_warnings", -1)))
	if bool(tier.get("active", false)):
		_failures.append("virtual shadow maps are active on the dummy renderer")


func _read_stats() -> Dictionary:
	return {
		"allocated": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VSM_PAGES_ALLOCATED),
		"requested": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VSM_PAGES_REQUESTED),
		"rerendered": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VSM_PAGES_RERENDERED),
		"overflow": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VSM_PAGES_OVERFLOW),
		"monitor_allocated": Performance.get_monitor(Performance.RENDER_VSM_PAGES_ALLOCATED),
		"monitor_requested": Performance.get_monitor(Performance.RENDER_VSM_PAGES_REQUESTED),
		"monitor_rerendered": Performance.get_monitor(Performance.RENDER_VSM_PAGES_RERENDERED),
		"monitor_overflow": Performance.get_monitor(Performance.RENDER_VSM_PAGES_OVERFLOW),
	}


func _check_stats_counting(p_stats: Dictionary, p_pool_pages: int, p_label: String) -> void:
	if int(p_stats["requested"]) <= 0 or int(p_stats["allocated"]) <= 0:
		_failures.append("%s: RENDERING_INFO_VSM_PAGES_* stayed 0 while active: %s" % [p_label, str(p_stats)])
	if int(p_stats["monitor_requested"]) <= 0 or int(p_stats["monitor_allocated"]) <= 0:
		_failures.append("%s: the virtual_shadows monitors stayed 0 while active: %s" % [p_label, str(p_stats)])
	if p_pool_pages > 0 and int(p_stats["allocated"]) > p_pool_pages:
		_failures.append("%s: %d pages allocated in a pool of %d" % [p_label, int(p_stats["allocated"]), p_pool_pages])


func _near(p_a: Color, p_b: Color) -> bool:
	return absf(p_a.r - p_b.r) <= COLOR_TOLERANCE and absf(p_a.g - p_b.g) <= COLOR_TOLERANCE and absf(p_a.b - p_b.b) <= COLOR_TOLERANCE


func _check_draw(p_name: String, p_image: Image, p_dirty: bool, p_label := "") -> void:
	var label := p_label if not p_label.is_empty() else p_name + ("_dirty" if p_dirty else "")
	_save(p_image, label)
	var total := 0
	var changed := 0
	var unmapped := 0
	var counts := {}
	var page_colors := {}
	if _baseline == null or p_image == null or _baseline.get_size() != p_image.get_size():
		_failures.append("%s: no image to compare" % label)
		return
	var width := p_image.get_width()
	var height := p_image.get_height()
	# Every second pixel of every second row.
	for y in range(0, height, 2):
		for x in range(0, width, 2):
			var a := p_image.get_pixel(x, y)
			var b := _baseline.get_pixel(x, y)
			total += 1
			var differs := absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > 0.02
			if differs:
				changed += 1
			if _near(a, UNMAPPED):
				unmapped += 1
			var key := ""
			match p_name:
				"vsm_mask":
					if absf(a.r - a.g) < 0.02 and absf(a.g - a.b) < 0.02:
						key = "lit" if a.r > 0.97 else ("shadowed" if a.r < 0.45 else "")
				"vsm_mip":
					for mip in MIP_COLORS.size():
						if _near(a, MIP_COLORS[mip]):
							key = "mip%d" % mip
				"vsm_pages":
					# A page's color, quantized; the darkened page edges and the plain image fall out by saturation.
					var saturation := maxf(a.r, maxf(a.g, a.b)) - minf(a.r, minf(a.g, a.b))
					if saturation > 0.3:
						page_colors[Vector3i(int(a.r * 15.0), int(a.g * 15.0), int(a.b * 15.0))] = true
				"vsm_cache":
					if _near(a, CACHE_RENDERED):
						key = "rendered"
					elif _near(a, CACHE_DYNAMIC):
						key = "dynamic"
					elif _near(a, CACHE_CACHED):
						key = "cached"
				"vsm_overdraw":
					if _near(a, OVERDRAW_NONE) or _near(a, OVERDRAW_NONE_EDGE):
						key = "none"
					elif differs:
						key = "drawn"
				"vsm_clipmap":
					var level_saturation := maxf(a.r, maxf(a.g, a.b)) - minf(a.r, minf(a.g, a.b))
					if level_saturation > 0.3:
						key = "level"
			if not key.is_empty():
				counts[key] = int(counts.get(key, 0)) + 1
	var shares := {}
	for key in counts:
		shares[key] = float(counts[key]) / maxf(float(total), 1.0)
	var draw := {"changed": float(changed) / maxf(float(total), 1.0), "unmapped": float(unmapped) / maxf(float(total), 1.0), "shares": shares}
	if p_name == "vsm_pages":
		draw["page_colors"] = page_colors.size()
	if not _result.has("draws"):
		_result["draws"] = {}
	_result["draws"][label] = draw
	if not _expect_active:
		# Without virtual shadow maps a VSM draw shows the normal image.
		if draw["changed"] > MAX_INACTIVE_CHANGE:
			_failures.append("%s changed %.4f of the image while VSM is inactive" % [label, draw["changed"]])
		return
	if draw["changed"] < MIN_SHARE:
		_failures.append("%s changed only %.4f of the image while VSM is active" % [label, draw["changed"]])
	if p_name in UNMAPPED_DRAWS and draw["unmapped"] > MAX_UNMAPPED:
		_failures.append("%s: %.4f of the image samples unmapped pages" % [label, draw["unmapped"]])
	var share := func(p_key: String) -> float: return float(shares.get(p_key, 0.0))
	match p_name:
		"vsm_mask":
			if share.call("lit") < MIN_SHARE or share.call("shadowed") < MIN_SHARE * 0.2:
				_failures.append("vsm_mask needs lit and shadowed pixels: %s" % str(shares))
		"vsm_mip":
			var mips := 0.0
			for key in shares:
				mips += float(shares[key])
			if mips < MIN_SHARE:
				_failures.append("vsm_mip shows no mip color: %s" % str(shares))
		"vsm_pages":
			if page_colors.size() < 16:
				_failures.append("vsm_pages shows %d page colors; expected at least 16" % page_colors.size())
		"vsm_cache":
			if p_dirty:
				if share.call("rendered") < MIN_SHARE:
					_failures.append("vsm_cache shows no rendered page after every page was new: %s" % str(shares))
			elif share.call("cached") < MIN_SHARE:
				_failures.append("vsm_cache shows no cached page on a static frame: %s" % str(shares))
		"vsm_overdraw":
			if p_dirty and share.call("drawn") < MIN_SHARE:
				_failures.append("vsm_overdraw shows no draws after every page was new: %s" % str(shares))
		"vsm_clipmap":
			if share.call("level") < MIN_SHARE:
				_failures.append("vsm_clipmap shows no clipmap level: %s" % str(shares))


func _check_draw_summary() -> void:
	if not _result.has("draws"):
		_failures.append("no debug draw was checked")


func _check_mixed(p_check: Dictionary) -> void:
	_result["raster_check"] = _raster_summary(p_check)
	if not bool(p_check.get("captured", false)):
		_failures.append("the mixed light set's raster check did not capture")
	elif not bool(p_check.get("pass", false)):
		_failures.append("the mixed light set's raster check failed: %s" % str(p_check.get("failures", [])))


func _raster_summary(p_check: Dictionary) -> Dictionary:
	return {
		"captured": p_check.get("captured", false),
		"pass": p_check.get("pass", false),
		"lights_without_casters": p_check.get("lights_without_casters", -1),
		"missing_from_vsm": p_check.get("missing_from_vsm", -1),
		"failures": p_check.get("failures", []),
	}


func _set_quality(p_index: int) -> void:
	ProjectSettings.set_setting(QUALITY_SETTING, p_index)


# Each preset: two frames on the new pool, one frame without shadows, then a raster check of the frame that
# draws every page, and the stats STATS_FRAMES frames later.
func _step_presets() -> void:
	match _stage_frame:
		2:
			_set_shadows(false)
		3:
			_set_shadows(true)
			RenderingServer.virtual_shadow_maps_request_raster_check()
			_preset_check = {}
		_:
			if _stage_frame > 3 and _preset_check.is_empty():
				var check := RenderingServer.virtual_shadow_maps_get_raster_check_result()
				if bool(check.get("captured", false)) or _stage_frame >= 3 + RASTER_WAIT_FRAMES:
					_preset_check = check
			if _stage_frame >= 3 + STATS_FRAMES and not _preset_check.is_empty():
				_check_preset(_preset_index, _preset_check)
				_preset_index += 1
				_stage_frame = 0
				if _preset_index < V6_MIB.size():
					_set_quality(_preset_index)
				else:
					_set_quality(2)
					_next_stage("done")


func _check_preset(p_index: int, p_check: Dictionary) -> void:
	var info := RenderingServer.virtual_shadow_maps_get_debug_info()
	var bytes := int(info.get("pool_driver_bytes", 0))
	var stats := _read_stats()
	var entry := {
		"quality": info.get("quality", ""),
		"pool_pages": info.get("pool_pages", 0),
		"pool_bytes": info.get("pool_bytes", 0),
		"pool_driver_bytes": bytes,
		"v6_bytes": V6_MIB[p_index] * 1048576,
		"stats": stats,
		"raster_check": _raster_summary(p_check),
	}
	if not _result.has("presets"):
		_result["presets"] = []
	_result["presets"].append(entry)
	var label := String(QUALITY_NAMES[p_index])
	if String(entry["quality"]) != label:
		_failures.append("quality %s did not switch the pool (now %s)" % [label, entry["quality"]])
	if absi(bytes - V6_MIB[p_index] * 1048576) > 1048576:
		_failures.append("the %s pool takes %d bytes; V6 is %d MiB" % [label, bytes, V6_MIB[p_index]])
	_check_stats_counting(stats, int(info.get("pool_pages", 0)), label)
	if not bool(p_check.get("captured", false)):
		_failures.append("%s: the raster check did not capture" % label)
	elif not bool(p_check.get("pass", false)):
		_failures.append("%s: the raster check failed: %s" % [label, str(p_check.get("failures", []))])


func _save(p_image: Image, p_name: String) -> void:
	if _out.is_empty() or p_image == null:
		return
	DirAccess.make_dir_recursive_absolute(_out)
	p_image.save_png(_out.path_join(p_name + ".png"))


func _finish() -> void:
	_result["frames"] = _frame
	_result["failures"] = _failures
	_result["pass"] = _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
