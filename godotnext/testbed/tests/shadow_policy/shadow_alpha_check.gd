extends Node3D

## RT visibility policy: alpha-tested and translucent casters (#145, plan §21.3 contract M4).
##
## A 20 m ground plate under a downward sun whose shadow map reaches 0.5 m, so the SRC relight traces every ground
## texel through the shared shadow-ray include (nextgi_shadow_ray_inc.glsl), and six 4.8 m casters 1.5 m above it:
##   texture hole   BaseMaterial3D alpha scissor whose albedo texture is transparent in its middle half (the
##                  bindless-texture path); a mesh material, so the surface is a shadow-only part (D17)
##   shader hole    a ShaderMaterial whose alpha (from shader code) cuts the same middle half (the RT-pipeline
##                  material retrace); an instance override, so the instance is non-opaque for shadow rays only
##   constant cut   BaseMaterial3D alpha scissor without a texture, albedo alpha below the threshold: casts nothing
##   opaque         the control
##   stack          five constant-cut plates 25 cm apart: four alpha samples cut, the fifth candidate is past the
##                  cap and shadows as opaque (the policy's rule, not the raster's)
##   glass          BaseMaterial3D alpha blend: shadows as opaque, as UE 5.9's hardware ray tracing does (LQ6)
## GodotNext: nextgi_hit_discard (#304). A seventh caster, the discard hole, is a ShaderMaterial that cuts the same
## middle half with discard in fragment(), on a ground of its own beside the main one: the any-hit stage ignores what
## fragment() discards, so the retrace leaves its hole lit, and its ground card's mask is checked the same way. An
## eighth plate, off the ground, discards in a helper function: the hit groups still refuse such a shader (#130), so it
## must shadow as opaque and leave no retrace entry once its verdict is known.
## With rendering/global_illumination/nextgi/debug/read_shadow_mask on, the ground card's shadow mask must match the
## expected footprint of every caster, away from the footprint and hole edges (one mask texel: every ground texel is
## traced straight up from the texel the mask records), in the holes and in the rings around them, for every
## shadowed light's bit. Counters prove the alpha paths ran: alpha instances, sampled, constant and retrace entries
## (exactly the fixture's), the relight's alpha variant, retraced rays with both answers.
## With one sun the run then checks NextGI's hold (60 TLAS phases): hiding the three hole plates for one frame keeps
## the hit groups; hiding them for 200 frames frees the hit groups (the refused plate's kept verdict does not bring
## them back) and returns the relight to its baked variant; showing them again rebuilds both, and the masks must
## match again.
##
## Run it through run_shadow_policy_check.sh (NextGI on at High, hardware tier on Auto):
##   godot --path godotnext/testbed --rendering-driver vulkan res://tests/shadow_policy/shadow_alpha_check.tscn
## "-- --suns=8": eight suns with distinct energies (so page slot i is sun i) and caster masks 3, 5, 9, 17, 33, 65,
## 129 and 3. Mask 3 is the most frequent and 129 the highest of the rest, so 129 holds no TLAS mask bit: sun 6
## takes the fallback, which never retraces (the shader and discard holes shadow as opaque for it). The other seven
## share the retrace records, one per mask texel per frame, in turn; every light's bit must be right.
## Pass "-- --no-retrace" on a device without RT pipelines (Metal): the shader and discard holes then shadow as opaque.
## With rendering/global_illumination/nextgi/hardware_ray_tracing_mode (#286) at Simplified Material or Inline
## (run_shadow_policy_check.sh's SHADOW_POLICY_HWRT_MODE=1 or 2), pass "-- --expect=simplified-material" or
## "-- --expect=inline": the texture hole is still sampled (an inline ray query, which UE keeps in every mode), the
## shader hole, the discard hole and the refused plate shadow as opaque (no material hit group exists to retrace them),
## the material retrace reports the mode as its reason and the relight leaves it no record.
## "-- --gpu-written": the six casters are dynamic MultiMeshes (one element each, five for the stack), so the GPU
## writes their TLAS instances (#57). The texture hole and the glass keep their mesh materials (the elements cast
## through the mesh's shadow-only parts); the constant cut, the stack and the shader hole take an instance
## override (the GI part's instances turn non-opaque for shadow rays). A GPU-written instance has no hit record,
## so the shader hole shadows as opaque; there is no discard hole, no refused plate and no hold check.
## Prints one "GODOTNEXT_RESULT" line and exits 0 when every check passes, 1 otherwise.

const MASK_SETTING := "rendering/global_illumination/nextgi/debug/read_shadow_mask"
const PLATE := 4.8
const HOLE := 1.2 ## Half the hole's width: the middle half of the plate.
const PLATE_Y := 1.5
const MAX_WAIT_FRAMES := 1500
const RETRACE_FRAMES := 60
const MIN_CHECKED := 4
const SUN_MASKS := [3, 5, 9, 17, 33, 65, 129, 3]
const FALLBACK_SUN := 6
const HOLD_FRAMES := 60
const LONG_HIDE_FRAMES := 200
const REFUSED_POSITION := Vector3(0.0, 1.5, -16.0) ## Off the ground: it shadows nothing the mask check reads.
## GodotNext: nextgi_hit_discard (#304). The discard hole's ground (center x, z, and size): beside the main ground, and
## near enough to the camera that its card keeps mask texels for the hole and the ring.
const MAIN_GROUND_HALF := 10.0
const DISCARD_GROUND_CENTER := Vector2(15.5, 3.0)
const DISCARD_GROUND_SIZE := 9.0
const GPU_WRITTEN_ELEMENTS := 10 ## --gpu-written: five single-plate MultiMeshes and the five-plate stack.
const SETTLE_FRAMES := 120 ## Without the retrace: frames the mask gets once the alpha paths are ready.

## Name, center (x, z) and what the ground under it must be: "hole" (lit in the hole, shadowed around it), "lit"
## or "shadow".
const CASTERS := [
	["texture_hole", Vector2(-6.4, -4.0), "hole"],
	["shader_hole", Vector2(0.0, -4.0), "hole"],
	["constant_cut", Vector2(6.4, -4.0), "lit"],
	["opaque", Vector2(-6.4, 4.0), "shadow"],
	["stack", Vector2(0.0, 4.0), "shadow"],
	["glass", Vector2(6.4, 4.0), "shadow"],
]
## GodotNext: nextgi_hit_discard (#304). The caster on the discard hole's ground.
const DISCARD_CASTERS := [
	["discard_hole", Vector2(15.5, 3.0), "hole"],
]
## Casters whose alpha comes from shader code: without the retrace they shadow as opaque.
const SHADER_ALPHA_HOLES := ["shader_hole", "discard_hole"]

var _failures := PackedStringArray()
var _result := {}
var _expect_retrace := true
var _expect_mode := "FULL" ## GodotNext: nextgi_hwrt_mode (#286). The Hardware Ray Tracing Mode the run expects.
var _suns := 1
var _fallback_slot := -1
var _plates := {}
var _gpu_written := false
var _multimeshes: Array[MultiMesh] = []
var _first_transforms: Array[Transform3D] = []


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg == "--no-retrace":
			_expect_retrace = false
		elif arg.begins_with("--suns="):
			_suns = clampi(int(arg.trim_prefix("--suns=")), 1, SUN_MASKS.size())
		elif arg == "--gpu-written":
			_gpu_written = true
		elif arg == "--expect=inline":
			_expect_mode = "INLINE"
			_expect_retrace = false
		elif arg == "--expect=simplified-material":
			_expect_mode = "SIMPLIFIED_MATERIAL"
			_expect_retrace = false
	_result["gpu_written"] = _gpu_written
	ProjectSettings.set_setting(MASK_SETTING, true)
	_build()
	_run()


func _build() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.nextgi_enabled = true
	var world := WorldEnvironment.new()
	world.environment = environment
	add_child(world)

	var ground := MeshInstance3D.new()
	var ground_mesh := BoxMesh.new()
	ground_mesh.size = Vector3(20, 0.2, 20)
	ground.mesh = ground_mesh
	ground.position = Vector3(0, -0.1, 0)
	add_child(ground)

	# The texture hole: alpha 0 in the middle half of the texture, 1 around it.
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var hole := absf((x + 0.5) / 64.0 - 0.5) < 0.25 and absf((y + 0.5) / 64.0 - 0.5) < 0.25
			image.set_pixel(x, y, Color(0.7, 0.7, 0.7, 0.0 if hole else 1.0))
	var texture_material := StandardMaterial3D.new()
	texture_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	texture_material.alpha_scissor_threshold = 0.5
	texture_material.albedo_texture = ImageTexture.create_from_image(image)
	_add_plate("texture_hole", texture_material, false, [PLATE_Y])

	var shader := Shader.new()
	shader.code = """shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.6);
	ALPHA = (abs(UV.x - 0.5) < 0.25 && abs(UV.y - 0.5) < 0.25) ? 0.0 : 1.0;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
"""
	var shader_material := ShaderMaterial.new()
	shader_material.shader = shader
	_add_plate("shader_hole", shader_material, true, [PLATE_Y])

	var cut := StandardMaterial3D.new()
	cut.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	cut.alpha_scissor_threshold = 0.5
	cut.albedo_color = Color(0.7, 0.7, 0.7, 0.2)
	# GPU-written: an instance override, so the GI part's instances carry the constant alpha.
	_add_plate("constant_cut", cut, _gpu_written, [PLATE_Y])

	_add_plate("opaque", StandardMaterial3D.new(), false, [PLATE_Y])

	_add_plate("stack", cut, true, [1.0, 1.25, 1.5, 1.75, 2.0])

	var glass := StandardMaterial3D.new()
	glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	glass.albedo_color = Color(0.4, 0.6, 0.9, 0.3)
	_add_plate("glass", glass, false, [PLATE_Y])

	# GodotNext: nextgi_hit_discard (#304). The discard hole cuts the middle half with discard in fragment(), which the
	# any-hit stage turns into ignoreIntersectionEXT; an instance override, as the shader hole.
	var discard_shader := Shader.new()
	discard_shader.code = """shader_type spatial;
void fragment() {
	ALBEDO = vec3(0.5);
	if (abs(UV.x - 0.5) < 0.25 && abs(UV.y - 0.5) < 0.25) {
		discard;
	}
}
"""
	var discard_material := ShaderMaterial.new()
	discard_material.shader = discard_shader
	# The refused plate discards in a helper function, which the hit groups refuse.
	var refused_shader := Shader.new()
	refused_shader.code = """shader_type spatial;
void cut(vec2 uv) {
	if (abs(uv.x - 0.5) < 0.25 && abs(uv.y - 0.5) < 0.25) {
		discard;
	}
}
void fragment() {
	ALBEDO = vec3(0.5);
	cut(UV);
}
"""
	var refused_material := ShaderMaterial.new()
	refused_material.shader = refused_shader
	if not _gpu_written:
		var discard_ground := MeshInstance3D.new()
		discard_ground.name = "discard_ground"
		var discard_ground_mesh := BoxMesh.new()
		discard_ground_mesh.size = Vector3(DISCARD_GROUND_SIZE, 0.2, DISCARD_GROUND_SIZE)
		discard_ground.mesh = discard_ground_mesh
		discard_ground.position = Vector3(DISCARD_GROUND_CENTER.x, -0.1, DISCARD_GROUND_CENTER.y)
		add_child(discard_ground, true)
		_add_plate("discard_hole", discard_material, true, [PLATE_Y])

		var refused_plate := MeshInstance3D.new()
		refused_plate.name = "refused_plate"
		var refused_mesh := PlaneMesh.new()
		refused_mesh.size = Vector2(PLATE, PLATE)
		refused_plate.mesh = refused_mesh
		refused_plate.material_override = refused_material
		refused_plate.position = REFUSED_POSITION
		add_child(refused_plate, true)

	for i in _suns:
		var sun := DirectionalLight3D.new()
		sun.name = "sun_%d" % i
		sun.rotation_degrees = Vector3(-90, 0, 0)
		sun.shadow_enabled = true
		# The map reaches 0.5 m from the camera: the relight traces every ground texel (#258).
		sun.directional_shadow_max_distance = 0.5
		if _suns > 1:
			# Distinct energies order the page's shadowed lights: slot i is sun i.
			sun.light_energy = 1.0 - 0.05 * i
			sun.shadow_caster_mask = SUN_MASKS[i]
		add_child(sun, true)

	var camera := Camera3D.new()
	camera.look_at_from_position(Vector3(0, 9, 14), Vector3(0, 0, 0), Vector3.UP)
	camera.current = true
	add_child(camera)

	for child in get_children():
		if child is GeometryInstance3D:
			(child as GeometryInstance3D).gi_mode = GeometryInstance3D.GI_MODE_STATIC


## Plates at their caster's center, one per height in p_heights. p_override puts the material on the instance (GI
## keeps the surface as opaque, #61); otherwise it is the mesh's material (the surface becomes a shadow-only part).
## With --gpu-written they are the elements of one MultiMesh at the origin (its elements' transforms are the TLAS
## instances' transforms), which _process() keeps dynamic.
func _add_plate(p_name: String, p_material: Material, p_override: bool, p_heights: Array) -> void:
	var center := Vector2.ZERO
	for caster in CASTERS + DISCARD_CASTERS:
		if caster[0] == p_name:
			center = caster[1]
	var mesh := PlaneMesh.new()
	mesh.size = Vector2(PLATE, PLATE)
	if not p_override:
		mesh.material = p_material
	if _gpu_written:
		var multimesh := MultiMesh.new()
		multimesh.transform_format = MultiMesh.TRANSFORM_3D
		multimesh.mesh = mesh
		multimesh.instance_count = p_heights.size()
		for i in p_heights.size():
			multimesh.set_instance_transform(i, Transform3D(Basis(), Vector3(center.x, float(p_heights[i]), center.y)))
		var instance := MultiMeshInstance3D.new()
		instance.name = p_name
		instance.multimesh = multimesh
		if p_override:
			instance.material_override = p_material
		add_child(instance, true)
		_multimeshes.append(multimesh)
		_first_transforms.append(Transform3D(Basis(), Vector3(center.x, float(p_heights[0]), center.y)))
		_plates[p_name] = instance
		return
	for height in p_heights:
		var plate := MeshInstance3D.new()
		plate.name = p_name
		plate.mesh = mesh
		if p_override:
			plate.material_override = p_material
		plate.position = Vector3(center.x, float(height), center.y)
		add_child(plate, true)
		if not _plates.has(p_name):
			_plates[p_name] = plate


## One updated element per frame keeps a MultiMesh in the RT dynamic class, so the GPU writes its TLAS instances
## (as tests/nextgi_dynamic_rt does).
func _process(_delta: float) -> void:
	for i in _multimeshes.size():
		_multimeshes[i].set_instance_transform(0, _first_transforms[i])


func _run() -> void:
	var info := await _wait_for_retrace()
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	_result["tier"] = String(info.get("tier", ""))
	_check(_result["tier"] == "HWRT", "the hardware tier is active (%s)" % _result["tier"])
	# GodotNext: nextgi_hwrt_mode (#286)
	_result["hardware_ray_tracing_mode"] = String(info.get("hardware_ray_tracing_mode", ""))
	_result["material_retrace"] = String((info.get("hit_groups_by_consumer", {}) as Dictionary).get("material_retrace", ""))
	_check(_result["hardware_ray_tracing_mode"] == _expect_mode, "the Hardware Ray Tracing Mode is %s (%s)" % [_expect_mode, _result["hardware_ray_tracing_mode"]])
	for key in ["shadow_alpha_instances", "shadow_only_instances", "shadow_texture_entries", "shadow_cut_entries", "shadow_retrace_entries", "shadow_opaque_alpha_entries", "shadow_word_misaligned", "shadow_allocation", "rt_gpu_written_instances", "hit_groups_linked", "hit_groups_pending", "hit_fallback_geometries", "hit_shadow_retrace_dispatches", "hit_materials_created"]:
		_result[key] = counters.get(key, -1)
	for key in ["src_shadow_alpha_variant", "src_shadow_relight_variant", "src_shadow_retrace_capacity", "src_shadow_retrace_frames", "src_shadow_retrace_appended", "src_shadow_retrace_occluded", "src_shadow_retrace_clear", "src_shadow_rays", "src_max_shadowed_lights"]:
		_result[key] = info.get(key, -1)
	_check(int(counters.get("shadow_alpha_instances", 0)) > 0, "non-opaque (alpha-tested) TLAS instances")
	_check(int(counters.get("shadow_only_instances", 0)) > 0, "shadow-only TLAS instances (surfaces GI does not see)")
	# Exactly the fixture's alpha entries: the texture hole is sampled (a detector that missed BaseMaterial3D's
	# texture alpha would send it to the retrace), the constant cut and the five stack plates are constants (the
	# stack's GPU-written elements share their MultiMesh's one entry).
	var cut_entries := 2 if _gpu_written else 6
	_check(int(counters.get("shadow_texture_entries", 0)) == 1, "one sampled-texture alpha entry, the texture hole (%d)" % int(counters.get("shadow_texture_entries", 0)))
	_check(int(counters.get("shadow_cut_entries", 0)) == cut_entries, "%d constant-alpha entries, the constant cut and the stack (%d)" % [cut_entries, int(counters.get("shadow_cut_entries", 0))])
	if _gpu_written:
		_check(int(counters.get("rt_gpu_written_instances", 0)) == GPU_WRITTEN_ELEMENTS, "the GPU wrote the casters' %d TLAS instances (%d)" % [GPU_WRITTEN_ELEMENTS, int(counters.get("rt_gpu_written_instances", 0))])
		# No hit record: the shader hole cannot be retraced and shadows as opaque.
		_check(int(counters.get("shadow_retrace_entries", -1)) == 0, "no retrace entry for GPU-written instances (%d)" % int(counters.get("shadow_retrace_entries", -1)))
		_check(int(counters.get("shadow_opaque_alpha_entries", 0)) == 1, "one opaque alpha entry, the GPU-written shader hole (%d)" % int(counters.get("shadow_opaque_alpha_entries", 0)))
	_check(int(counters.get("shadow_word_misaligned", -1)) == 0, "the shadow instance words stay in step with the TLAS custom indices (%d)" % int(counters.get("shadow_word_misaligned", -1)))
	_check(int(info.get("src_shadow_relight_variant", -1)) == 1, "the relight dispatched its alpha variant (%d)" % int(info.get("src_shadow_relight_variant", -1)))
	_check(_suns <= 1 or int(info.get("src_max_shadowed_lights", 0)) == _suns, "the page shadows all %d suns (%d)" % [_suns, int(info.get("src_max_shadowed_lights", 0))])
	if _gpu_written:
		pass
	elif _expect_retrace:
		# The shader hole and the discard hole go to the retrace (#304: the any-hit stage ignores what fragment()
		# discards); the refused plate's shader is refused by the hit groups, so once its verdict is known it shadows
		# as opaque and asks for nothing.
		_check(int(counters.get("shadow_retrace_entries", 0)) == 2, "two retrace entries, the shader hole and the discard hole (%d)" % int(counters.get("shadow_retrace_entries", 0)))
		_check(int(counters.get("shadow_opaque_alpha_entries", 0)) == 1, "one opaque alpha entry, the refused plate (%d)" % int(counters.get("shadow_opaque_alpha_entries", 0)))
		_check(int(info.get("src_shadow_retrace_appended", 0)) > 0, "the relight appended retrace records")
		_check(int(info.get("src_shadow_retrace_occluded", 0)) > 0, "the retrace found occluded rays")
		_check(int(info.get("src_shadow_retrace_clear", 0)) > 0, "the retrace found clear rays")
	elif _expect_mode != "FULL":
		# GodotNext: nextgi_hwrt_mode (#286). No material hit group exists in this mode: the shader hole, the discard
		# hole and the refused plate are opaque entries, the material retrace reports the mode and the relight leaves
		# it nothing.
		_check(_result["material_retrace"] == "HWRT_MODE_" + _expect_mode, "the material retrace reports HWRT_MODE_%s (%s)" % [_expect_mode, _result["material_retrace"]])
		_check(int(counters.get("shadow_retrace_entries", -1)) == 0, "no retrace entry in %s (%d)" % [_expect_mode, int(counters.get("shadow_retrace_entries", -1))])
		_check(int(counters.get("shadow_opaque_alpha_entries", 0)) == 3, "three opaque alpha entries, the shader hole, the discard hole and the refused plate (%d)" % int(counters.get("shadow_opaque_alpha_entries", 0)))
		_check(int(info.get("src_shadow_retrace_capacity", -1)) == 0, "the relight leaves the retrace no record (capacity %d)" % int(info.get("src_shadow_retrace_capacity", -1)))
		_check(not bool(counters.get("shadow_wants_retrace", true)), "nothing asks for the retrace")
	else:
		_check(int(counters.get("shadow_retrace_entries", 0)) >= 1, "alpha entries left to the retrace")
	if _suns > 1:
		_find_fallback_slot(counters)
	_check_mask(info, "")
	if not _gpu_written:
		_check_mask(info, "", "discard")
	if _suns == 1 and _expect_retrace and not _gpu_written:
		await _check_hold()
	_finish()


## Waits until the relight runs its alpha variant (and with --gpu-written, the GPU writes every caster's instance),
## then until the hit groups linked and the retrace ran RETRACE_FRAMES times, or without the retrace for
## SETTLE_FRAMES more frames.
func _wait_for_retrace() -> Dictionary:
	# The hit groups compile off the render thread: wait until none is pending, then for the retrace to run.
	var info: Dictionary = {}
	var ready_frame := -1
	var start_retrace := 0
	var waited := 0
	var frames := RETRACE_FRAMES * maxi(_suns / 2, 1)
	var retrace := _expect_retrace and not _gpu_written
	while waited < MAX_WAIT_FRAMES:
		await RenderingServer.frame_post_draw
		waited += 1
		if waited < 30:
			continue
		info = RenderingServer.nextgi_get_tier_info()
		var counters: Dictionary = RenderingServer.nextgi_get_counters()
		if String(info.get("tier", "")) != "HWRT":
			continue
		var paths_ready := int(info.get("src_shadow_relight_variant", -1)) == 1
		if _gpu_written:
			paths_ready = paths_ready and int(counters.get("rt_gpu_written_instances", 0)) == GPU_WRITTEN_ELEMENTS
		if not retrace:
			if ready_frame < 0:
				if paths_ready:
					ready_frame = waited
				continue
			if not paths_ready:
				ready_frame = -1 # Start the settling again.
				continue
			if waited - ready_frame >= SETTLE_FRAMES:
				break
			continue
		var linked := int(counters.get("hit_groups_linked", 0))
		var pending := int(counters.get("hit_groups_pending", 1))
		if ready_frame < 0:
			if paths_ready and linked > 0 and pending == 0 and int(info.get("src_shadow_retrace_capacity", 0)) > 0:
				ready_frame = waited
				start_retrace = int(info.get("src_shadow_retrace_frames", 0))
			continue
		if int(info.get("src_shadow_retrace_frames", 0)) - start_retrace >= frames:
			break
	_result["frames"] = int(_result.get("frames", 0)) + waited
	if retrace:
		_check(ready_frame >= 0, "the hit groups linked and the retrace ran within %d frames" % MAX_WAIT_FRAMES)
	else:
		_check(ready_frame >= 0 and waited < MAX_WAIT_FRAMES, "the alpha paths were ready for %d frames within %d frames" % [SETTLE_FRAMES, MAX_WAIT_FRAMES])
	return info


## Sun 6's caster mask (129) holds no TLAS mask bit: it is the fallback light.
func _find_fallback_slot(p_counters: Dictionary) -> void:
	var allocation = p_counters.get("shadow_allocation", PackedInt64Array())
	var unallocated := PackedInt32Array()
	for i in _suns:
		if not allocation.has(SUN_MASKS[i]):
			unallocated.append(i)
	_result["fallback_suns"] = unallocated
	_check(unallocated.size() == 1 and unallocated[0] == FALLBACK_SUN, "exactly sun %d falls back (%s, allocation %s)" % [FALLBACK_SUN, str(unallocated), str(allocation)])
	_fallback_slot = FALLBACK_SUN


func _set_holes_visible(p_visible: bool) -> void:
	for plate_name in ["texture_hole", "shader_hole", "discard_hole"]:
		if _plates.has(plate_name):
			(_plates[plate_name] as Node3D).visible = p_visible


## NextGI's hold: a one-frame absence keeps the hit groups; a long one frees them and the relight's alpha variant,
## and the refused plate's kept verdict does not bring the hit groups back; showing the holes again rebuilds both.
func _check_hold() -> void:
	var created := int(RenderingServer.nextgi_get_counters().get("hit_materials_created", -1))
	_result["hold_created"] = created
	_check(created >= 1, "the hit groups were made (%d)" % created)

	_set_holes_visible(false)
	await RenderingServer.frame_post_draw
	_set_holes_visible(true)
	for i in HOLD_FRAMES + 30:
		await RenderingServer.frame_post_draw
	var counters: Dictionary = RenderingServer.nextgi_get_counters()
	_result["one_frame_created"] = int(counters.get("hit_materials_created", -1))
	_check(int(counters.get("hit_materials_created", -1)) == created and bool(counters.get("hit_materials_live", false)), "a one-frame absence of the alpha casters keeps the hit groups (made %d times, now %d)" % [created, int(counters.get("hit_materials_created", -1))])

	_set_holes_visible(false)
	var relinked := false
	for i in LONG_HIDE_FRAMES:
		await RenderingServer.frame_post_draw
		if int(RenderingServer.nextgi_get_counters().get("hit_materials_created", -1)) != created:
			relinked = true
	counters = RenderingServer.nextgi_get_counters()
	var info: Dictionary = RenderingServer.nextgi_get_tier_info()
	_result["hidden"] = {
		"hit_materials_live": counters.get("hit_materials_live", null),
		"hit_materials_created": counters.get("hit_materials_created", -1),
		"shadow_retrace_entries": counters.get("shadow_retrace_entries", -1),
		"shadow_opaque_alpha_entries": counters.get("shadow_opaque_alpha_entries", -1),
		"src_shadow_relight_variant": info.get("src_shadow_relight_variant", -1),
	}
	_check(not bool(counters.get("hit_materials_live", true)), "%d frames without a shader-alpha caster free the hit groups" % LONG_HIDE_FRAMES)
	_check(not relinked, "the refused plate's kept verdict never brings the hit groups back")
	_check(int(counters.get("shadow_retrace_entries", -1)) == 0, "no retrace entry while the shader and discard holes are hidden (%d)" % int(counters.get("shadow_retrace_entries", -1)))
	_check(int(counters.get("shadow_opaque_alpha_entries", -1)) == 1, "the refused plate stays opaque (%d)" % int(counters.get("shadow_opaque_alpha_entries", -1)))
	_check(int(info.get("src_shadow_relight_variant", -1)) == 0, "the relight runs its baked variant again (%d)" % int(info.get("src_shadow_relight_variant", -1)))

	_set_holes_visible(true)
	info = await _wait_for_retrace()
	counters = RenderingServer.nextgi_get_counters()
	_result["shown_created"] = int(counters.get("hit_materials_created", -1))
	_check(int(counters.get("hit_materials_created", -1)) == created + 1, "showing the holes again makes the hit groups once more (%d after %d)" % [int(counters.get("hit_materials_created", -1)), created])
	_check(int(info.get("src_shadow_relight_variant", -1)) == 1, "the relight runs its alpha variant again (%d)" % int(info.get("src_shadow_relight_variant", -1)))
	_check(int(counters.get("shadow_retrace_entries", 0)) == 2 and int(counters.get("shadow_texture_entries", 0)) == 1, "the alpha entries are back (retrace %d, texture %d)" % [int(counters.get("shadow_retrace_entries", 0)), int(counters.get("shadow_texture_entries", 0))])
	_check(int(counters.get("shadow_word_misaligned", -1)) == 0, "the shadow instance words stay in step (%d)" % int(counters.get("shadow_word_misaligned", -1)))
	_check_mask(info, "shown again: ")
	_check_mask(info, "shown again: ", "discard")


func _expected_lit(p_world: Vector3, p_margin: float, p_slot: int, p_casters: Array = CASTERS) -> int:
	# 1 lit, 0 shadowed, -1 too close to an edge to judge.
	var point := Vector2(p_world.x, p_world.z)
	for caster in p_casters:
		var offset: Vector2 = (point - (caster[1] as Vector2)).abs()
		var half := PLATE * 0.5
		if offset.x > half + p_margin or offset.y > half + p_margin:
			continue
		if absf(offset.x - half) <= p_margin or absf(offset.y - half) <= p_margin:
			return -1
		var kind: String = caster[2]
		# Without the retrace, for the fallback light (its layer check needs the candidate loop, which the retrace
		# cannot do) and for GPU-written instances (no hit record), the shader and discard holes shadow as opaque.
		if kind == "hole" and caster[0] in SHADER_ALPHA_HOLES and (not _expect_retrace or _gpu_written or p_slot == _fallback_slot):
			kind = "shadow"
		if kind == "lit":
			return 1
		if kind == "shadow":
			return 0
		var near_x := absf(offset.x - HOLE) <= p_margin and offset.y <= HOLE + p_margin
		var near_z := absf(offset.y - HOLE) <= p_margin and offset.x <= HOLE + p_margin
		if near_x or near_z:
			return -1
		return 1 if offset.x < HOLE and offset.y < HOLE else 0
	return 1


## Every shadowed light's bit of the ground card's mask against the expected footprints. p_prefix labels the pass.
## GodotNext: nextgi_hit_discard (#304). p_ground "discard" checks the discard hole's ground (its own card, casters and
## result keys) instead of the main ground.
func _check_mask(p_info: Dictionary, p_prefix: String, p_ground := "") -> void:
	var on_discard_ground := p_ground == "discard"
	var ground_center := DISCARD_GROUND_CENTER if on_discard_ground else Vector2.ZERO
	var ground_half := DISCARD_GROUND_SIZE * 0.5 if on_discard_ground else MAIN_GROUND_HALF
	var casters: Array = DISCARD_CASTERS if on_discard_ground else CASTERS
	var key := "discard_" if on_discard_ground else ""
	var prefix := p_prefix + ("discard ground: " if on_discard_ground else "")
	var pages: Array = p_info.get("src_mask_pages", [])
	_check(not pages.is_empty(), prefix + "shadow mask readback")
	var page: Dictionary = {}
	var best_y := 1000.0
	for candidate in pages:
		if _world_normal(candidate).y < 0.5:
			continue
		var center := _mask_world(candidate, int(candidate["mask_w"]) / 2, int(candidate["mask_h"]) / 2)
		# The two grounds' cards face up at the same height: take the one over this ground.
		if absf(center.x - ground_center.x) > ground_half or absf(center.z - ground_center.y) > ground_half:
			continue
		if center.y < best_y:
			best_y = center.y
			page = candidate
	_check(best_y < 0.5, prefix + "the ground card faces up at y %.2f" % best_y)
	if page.is_empty():
		return
	var mask: PackedByteArray = page["mask"]
	var mask_w := int(page["mask_w"])
	var mask_h := int(page["mask_h"])
	var texel_u := 2.0 * float(page["half_u"]) / float(maxi(mask_w, 1))
	var texel_v := 2.0 * float(page["half_v"]) / float(maxi(mask_h, 1))
	var margin := maxf(absf(texel_u), absf(texel_v))
	_result[key + "mask"] = "%dx%d" % [mask_w, mask_h]
	_result[key + "margin"] = margin
	var slot_results := []
	for slot in _suns:
		# Per caster: [checked, bad, checked in the hole, bad in the hole]; the hole casters' other texels are the ring.
		var regions := {}
		for caster in casters:
			regions[caster[0]] = [0, 0, 0, 0]
		regions["outside"] = [0, 0, 0, 0]
		var bad_samples := PackedStringArray()
		for y in mask_h:
			for x in mask_w:
				var world := _mask_world(page, x, y)
				var expected := _expected_lit(world, margin, slot, casters)
				if expected < 0:
					continue
				var lit := (mask[y * mask_w + x] & (1 << slot)) != 0
				var region := "outside"
				var in_hole := false
				for caster in casters:
					var offset: Vector2 = (Vector2(world.x, world.z) - (caster[1] as Vector2)).abs()
					if offset.x <= PLATE * 0.5 and offset.y <= PLATE * 0.5:
						region = caster[0]
						in_hole = offset.x < HOLE and offset.y < HOLE
				var counts: Array = regions[region]
				counts[0] += 1
				if in_hole:
					counts[2] += 1
				if lit != (expected == 1):
					counts[1] += 1
					if in_hole:
						counts[3] += 1
					if bad_samples.size() < 8:
						bad_samples.append("%s (%.2f, %.2f) lit=%s" % [region, world.x, world.z, lit])
		slot_results.append({"slot": slot, "regions": regions, "bad_samples": bad_samples})
		var label := prefix + ("" if _suns == 1 else "light %d: " % slot)
		for region in regions:
			var counts: Array = regions[region]
			_check(int(counts[0]) >= MIN_CHECKED, "%s%s: %d texels checked, expected at least %d" % [label, region, counts[0], MIN_CHECKED])
			_check(int(counts[1]) == 0, "%s%s: %d of %d texels disagree with the expected shadow (%d in the hole)" % [label, region, counts[1], counts[0], counts[3]])
		for caster in casters:
			if caster[2] != "hole":
				continue
			var hole: String = caster[0]
			var counts: Array = regions[hole]
			_check(int(counts[2]) >= MIN_CHECKED, "%s%s: %d texels checked in the hole, expected at least %d" % [label, hole, counts[2], MIN_CHECKED])
			_check(int(counts[0]) - int(counts[2]) >= MIN_CHECKED, "%s%s: %d texels checked around the hole, expected at least %d" % [label, hole, int(counts[0]) - int(counts[2]), MIN_CHECKED])
	_result[key + ("slots" if p_prefix.is_empty() else "slots_shown_again")] = slot_results


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


func _check(p_condition: bool, p_message: String) -> void:
	if not p_condition:
		_failures.append(p_message)
		printerr("shadow_alpha_check: " + p_message)


func _finish() -> void:
	ProjectSettings.set_setting(MASK_SETTING, false)
	_result["pass"] = _failures.is_empty()
	_result["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify(_result))
	get_tree().quit(0 if _failures.is_empty() else 1)
