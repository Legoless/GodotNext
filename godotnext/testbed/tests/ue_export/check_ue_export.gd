extends SceneTree
## Checks two exports of one scene made by tools/ue_export/ue_export.gd: the overlap-heavy kitbash
## (#235), RT Shadow Yard or MegaLights Hall at 1,024 lights (#233), or Upscaler Torture (#232).
## run_ue_export_check.sh makes them; this script runs headless:
##
##   godot --headless --path godotnext/testbed --script res://tests/ue_export/check_ue_export.gd -- \
##     <kitbash|yard|hall|torture> <export A> <export B>
##
## It checks that the two exports are byte-identical apart from export_manifest.json, that the
## manifest checksums match, that scene.json follows ue_export_schema.md, that library.glb and
## axes.glb re-import with the triangle counts, bounds and marker positions scene.json states,
## that every camera row is the scene camera's pose, that every texture is the PNG of the scene's
## own texture, and that sky.hdr holds the procedural sky. Per scene: every kitbash rock placement
## equals the seeded layout's plan (so the MultiMesh half kept its transforms); the yard's
## caster-mask matrix, blend modes and approximations; the hall's light mix, area lights, light
## masks and approximations; Upscaler Torture's camera path and cut, recomputed from the scene's
## curve, its moving fan and occluders and the rigid parts of its skinned character, recomputed from
## the scene's nodes and animation, its UV transforms, texture filters, ShaderMaterial and
## approximations. Prints GODOTNEXT_RESULT and exits 1 on any failure.

const Lib := preload("res://tools/ue_export/ue_export_lib.gd")
const Layout := preload("res://scenes/swrt_kitbash/layout.gd")

const SCENES := {
	"kitbash": {"scene": "res://scenes/swrt_kitbash/swrt_kitbash.tscn", "camera": "CaptureCam", "sky": true},
	"yard": {"scene": "res://scenes/rt_shadow_yard/rt_shadow_yard.tscn", "camera": "cam_main", "sky": true},
	"hall": {"scene": "res://scenes/mega_lights_hall/mega_lights_hall.tscn", "camera": "cam_main", "sky": true},
	"torture": {"scene": "res://scenes/upscaler_torture/upscaler_torture.tscn", "camera": "CameraPath/CameraFollow/Camera3D", "sky": false},
}
## Upscaler Torture (#232): the frame the scene resets the temporal history at, and its timeline
## (torture_timeline.gd), restated here so the check recomputes the poses on its own.
const TORTURE_CUT := 400
const TORTURE_TARGET := Vector3(0.0, 1.1, -0.4)
const TORTURE_FRAMES := [0, 150, 199, 200, 299, 300, 320, 340, 399, 400, 450, 599]
const EPSILON := 1e-5

var _scene_path := ""
var _camera_name := ""
var _has_sky := true

var _failures: Array[String] = []
var _checks := 0


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 3 or not SCENES.has(String(args[0])):
		printerr("check_ue_export.gd: pass <kitbash|yard|hall|torture> and two export directories after --")
		quit(2)
		return
	var which := String(args[0])
	_scene_path = String(SCENES[which]["scene"])
	_camera_name = String(SCENES[which]["camera"])
	_has_sky = bool(SCENES[which]["sky"])
	_test_lib()
	var first := String(args[1])
	var second := String(args[2])
	_check_determinism(first, second)
	var scene := _check_manifest_and_load(first)
	if not scene.is_empty():
		_check_schema(scene)
		match which:
			"kitbash":
				_check_kitbash(scene)
			"yard":
				_check_yard(scene)
			"hall":
				_check_hall(scene)
			"torture":
				_check_torture(first, scene)
		_check_library(first, scene)
		_check_axes(first)
		_check_camera_track(first, scene)
		_check_textures(first, scene)
		if _has_sky:
			_check_sky(first, scene)
		else:
			_check(scene["environment"]["sky"] == null and not FileAccess.file_exists(first.path_join("sky.hdr")), "a scene without a sky has no sky.hdr")
	var ok := _failures.is_empty()
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "ue_export", "scene": which, "ok": ok, "checks": _checks, "failures": _failures.size()}))
	for failure in _failures:
		printerr("check_ue_export.gd: FAIL " + failure)
	quit(0 if ok else 1)


func _check(p_condition: bool, p_message: String) -> void:
	_checks += 1
	if not p_condition:
		_failures.append(p_message)


func _test_lib() -> void:
	_test_refusals()
	var transform := Transform3D(Basis.from_euler(Vector3(0.3, -1.2, 2.0)) * Basis.from_scale(Vector3.ONE * 0.75), Vector3(1.5, -2.0, 3.25))
	var back := Lib.rows_to_transform(Lib.transform_rows(transform))
	_check(back.is_equal_approx(transform), "transform rows round-trip")
	var rows := Lib.transform_rows(Transform3D(Basis(), Vector3(4.0, 5.0, 6.0)))
	_check(rows == [1.0, 0.0, 0.0, 4.0, 0.0, 1.0, 0.0, 5.0, 0.0, 0.0, 1.0, 6.0], "transform rows are row-major with the origin last")
	_check(Lib.json_value(Color(0.25, 0.5, 1.0, 1.0)) == [0.25, 0.5, 1.0, 1.0], "a color becomes four numbers")
	_check(Lib.json_value(Vector3i(1, 2, 3)) == [1, 2, 3], "a Vector3i becomes three numbers")
	_check(Lib.json_value(null) == null, "null stays null")
	var direction := Lib.panorama_direction(128, 0, 512, 256)
	_check(direction.y > 0.99, "the top panorama row looks up")
	direction = Lib.panorama_direction(0, 128, 512, 256)
	_check(absf(direction.y) < 0.02 and direction.z < -0.99, "the left edge of the horizon row looks down -Z")
	var pixels := PackedFloat32Array()
	for index in 16 * 2:
		pixels.append_array(PackedFloat32Array([0.001 * index, 0.5, 3.0 + index]))
	var decoded := Lib.decode_hdr(Lib.encode_hdr(16, 2, pixels))
	_check(not decoded.has("error") and int(decoded["width"]) == 16 and int(decoded["height"]) == 2, "an HDR round-trips its size")
	if not decoded.has("error"):
		var worst := 0.0
		var largest := 0.0
		for index in pixels.size():
			largest = maxf(largest, pixels[index])
		for index in pixels.size():
			# RGBE keeps 8 mantissa bits of the largest channel of each pixel.
			worst = maxf(worst, absf(decoded["pixels"][index] - pixels[index]) / largest)
		_check(worst < 0.01, "an HDR round-trips its values within RGBE precision (worst %f)" % worst)


## The content the format does not carry yet is refused, not dropped, and content the UE import
## builds only approximately is named (ue_export_schema.md).
func _test_refusals() -> void:
	var plain := StandardMaterial3D.new()
	plain.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
	_check(Lib.material_refusals(plain).is_empty() and Lib.material_approximations(plain).is_empty(), "an opaque Lambert constant material is carried as it is")
	_check(Lib.material_blend(plain) == "opaque", "an opaque material blends opaque")
	var burley := StandardMaterial3D.new()
	_check(Lib.material_refusals(burley).is_empty(), "Burley diffuse (the StandardMaterial3D default) is not refused outright")
	_check(Lib.material_approximations(burley) == PackedStringArray(["diffuse_mode"]), "Burley diffuse needs the diffuse_mode approximation")
	var vertex_albedo := plain.duplicate() as StandardMaterial3D
	vertex_albedo.vertex_color_use_as_albedo = true
	_check(Lib.material_refusals(vertex_albedo).size() == 1, "vertex colors as albedo are refused")
	var transparent := plain.duplicate() as StandardMaterial3D
	transparent.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_check(Lib.material_refusals(transparent).is_empty() and Lib.material_blend(transparent) == "translucent", "alpha blending is carried as translucent")
	var scissor := plain.duplicate() as StandardMaterial3D
	scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	_check(Lib.material_refusals(scissor).is_empty() and Lib.material_blend(scissor) == "masked", "alpha scissor is carried as masked")
	scissor.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
	_check(Lib.material_refusals(scissor).is_empty() and Lib.material_approximations(scissor) == PackedStringArray(["alpha_antialiasing"]), "alpha scissor with alpha antialiasing needs the alpha_antialiasing approximation")
	transparent.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE
	_check(Lib.material_approximations(transparent).is_empty(), "alpha antialiasing on an alpha-blended material (where Godot does not use it) needs no approximation")
	transparent.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_OFF
	var hashed := plain.duplicate() as StandardMaterial3D
	hashed.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_HASH
	_check(Lib.material_refusals(hashed).size() == 1, "alpha hash is refused")
	var pre_pass := plain.duplicate() as StandardMaterial3D
	pre_pass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS
	_check(Lib.material_refusals(pre_pass).size() == 1, "alpha with a depth pre-pass is refused")
	var additive := transparent.duplicate() as StandardMaterial3D
	additive.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	_check(Lib.material_refusals(additive).size() == 1, "an additive blend mode is refused")
	var unshaded := plain.duplicate() as StandardMaterial3D
	unshaded.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_check(Lib.material_refusals(unshaded).size() == 1, "an unshaded material is refused")
	var front := plain.duplicate() as StandardMaterial3D
	front.cull_mode = BaseMaterial3D.CULL_FRONT
	_check(Lib.material_refusals(front).size() == 1, "front-face culling is refused")
	var two_sided := plain.duplicate() as StandardMaterial3D
	two_sided.cull_mode = BaseMaterial3D.CULL_DISABLED
	_check(Lib.material_refusals(two_sided).is_empty(), "two-sided culling is carried")
	var sss := plain.duplicate() as StandardMaterial3D
	sss.subsurf_scatter_strength = 0.8
	_check(Lib.material_refusals(sss).is_empty(), "a subsurface strength without the feature enabled is carried (nothing renders it)")
	sss.subsurf_scatter_enabled = true
	_check(Lib.material_refusals(sss).size() == 1, "enabled subsurface scattering is refused")
	var leaf := ImageTexture.create_from_image(Image.create_empty(4, 4, false, Image.FORMAT_RGBA8))
	var textured := scissor.duplicate() as StandardMaterial3D
	textured.alpha_antialiasing_mode = BaseMaterial3D.ALPHA_ANTIALIASING_OFF
	textured.albedo_texture = leaf
	_check(Lib.material_refusals(textured).is_empty(), "an albedo texture is carried")
	textured.uv1_scale = Vector3(2.0, 2.0, 1.0)
	textured.uv1_offset = Vector3(0.25, 0.5, 0.0)
	_check(Lib.material_refusals(textured).is_empty(), "an albedo texture with a UV1 scale and offset is carried")
	textured.uv1_triplanar = true
	_check(Lib.material_refusals(textured).size() == 1, "triplanar UV1 mapping is refused")
	textured.uv1_triplanar = false
	for filter: int in BaseMaterial3D.TEXTURE_FILTER_MAX:
		textured.texture_filter = filter as BaseMaterial3D.TextureFilter
		var carried := filter in [BaseMaterial3D.TEXTURE_FILTER_NEAREST, BaseMaterial3D.TEXTURE_FILTER_LINEAR, BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS]
		_check(Lib.material_refusals(textured).is_empty() == carried and Lib.texture_filter_name(textured).is_empty() != carried, "texture filter %d is %s" % [filter, "carried" if carried else "refused"])
	_check(Lib.texture_filter_name(plain) == "linear_mipmaps", "the default texture filter is linear with mipmaps")
	textured.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	_check(Lib.texture_filter_name(textured) == "nearest", "nearest filtering is named nearest")
	textured.texture_repeat = false
	_check(Lib.material_refusals(textured).size() == 1, "an albedo texture without repeat is refused")
	textured.texture_repeat = true
	var roughness_map := plain.duplicate() as StandardMaterial3D
	roughness_map.roughness_texture = leaf
	_check(Lib.material_refusals(roughness_map).size() == 1, "a texture in another slot is refused")
	# One value per Lib.REFUSED_FLAGS property that changes what Godot draws.
	var flag_values := {
		"depth_draw_mode": BaseMaterial3D.DEPTH_DRAW_ALWAYS,
		"no_depth_test": true,
		"render_priority": 1,
		"proximity_fade_enabled": true,
		"distance_fade_mode": BaseMaterial3D.DISTANCE_FADE_PIXEL_ALPHA,
		"billboard_mode": BaseMaterial3D.BILLBOARD_ENABLED,
		"grow": true,
		"disable_receive_shadows": true,
		"shadow_to_opacity": true,
		"disable_ambient_light": true,
		"disable_fog": true,
	}
	_check(flag_values.keys() == Lib.REFUSED_FLAGS.keys(), "every refused material flag has a unit check")
	for property: String in flag_values:
		var flagged := transparent.duplicate() as StandardMaterial3D
		flagged.set(property, flag_values[property])
		var refusals := Lib.material_refusals(flagged)
		_check(refusals.size() == 1 and Lib.REFUSED_FLAGS.has(property) and refusals[0] == String(Lib.REFUSED_FLAGS[property][1]), "material flag %s is refused (%s)" % [property, refusals])
	var drawn_depth := plain.duplicate() as StandardMaterial3D
	drawn_depth.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	_check(Lib.material_refusals(drawn_depth).size() == 1, "a disabled depth draw is refused")
	_check(Lib.translucent_shadow(transparent, GeometryInstance3D.SHADOW_CASTING_SETTING_ON), "an alpha-blended surface on a shadow caster needs translucent_shadow")
	_check(Lib.translucent_shadow(transparent, GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY), "an alpha-blended surface on a shadows-only caster needs translucent_shadow")
	_check(not Lib.translucent_shadow(transparent, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF), "an alpha-blended surface that casts no shadow is carried as it is")
	_check(not Lib.translucent_shadow(scissor, GeometryInstance3D.SHADOW_CASTING_SETTING_ON) and not Lib.translucent_shadow(plain, GeometryInstance3D.SHADOW_CASTING_SETTING_ON), "opaque and alpha-scissor casters are carried as they are")
	_check(not Lib.translucent_shadow(null, GeometryInstance3D.SHADOW_CASTING_SETTING_ON), "a surface without a material is carried as it is")
	_check(Lib.mask_effect(0xFFFFFFFF, 0b111) == "all", "a full mask keeps every placed layer")
	_check(Lib.mask_effect(1 << 18, 0b111) == "none", "a mask on unused layers keeps none")
	_check(Lib.mask_effect(0b010, 0b111) == "some", "a mask on one used layer keeps some")
	_check(Lib.mask_effect(0b010, 0) == "all", "no placed layer: every mask keeps all")
	var png := Lib.texture_png(leaf)
	var decoded := Image.new()
	_check(not png.is_empty() and decoded.load_png_from_buffer(png) == OK and decoded.get_size() == Vector2i(4, 4), "a texture becomes a PNG of its size")
	_check(Lib.texture_png(null).is_empty(), "no texture gives no PNG")
	_test_texture_png()
	for id: String in ["diffuse_mode", "shadow_caster_mask", "light_projector", "light_cull_mask", "translucent_shadow", "alpha_antialiasing", "rigid_skin", "shader_material", "particles"]:
		_check(Lib.APPROXIMATIONS.has(id) and not String(Lib.APPROXIMATIONS[id]).is_empty(), "approximation %s is described" % id)
	_test_rigid_skin()
	var environment := Environment.new()
	environment.sky = Sky.new()
	var sky := ProceduralSkyMaterial.new()
	environment.sky.sky_material = sky
	var camera := Camera3D.new()
	_check(Lib.environment_refusals(environment, camera).is_empty(), "a procedural sky without a cover and a camera without an Environment are carried")
	sky.sky_cover = ImageTexture.create_from_image(Image.create_empty(4, 4, false, Image.FORMAT_RGBA8))
	_check(Lib.environment_refusals(environment, camera).size() == 1, "a sky cover texture is refused")
	sky.sky_cover = null
	camera.environment = Environment.new()
	_check(Lib.environment_refusals(environment, camera).size() == 1, "an Environment on the camera is refused")
	_check(Lib.environment_refusals(null, null).is_empty(), "no environment and no camera refuse nothing")
	camera.free()


## rigid_skin_parts splits a mesh whose vertices each follow one bone at full weight into one part
## per bone, keeping each vertex's position and its source index, and refuses blended weights and
## triangles that span bones.
func _test_rigid_skin() -> void:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var positions: Array[Vector3] = [Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(2, 0, 0), Vector3(3, 0, 0), Vector3(2, 1, 0)]
	for index in positions.size():
		st.set_bones(PackedInt32Array([0 if index < 3 else 3, 1, 2, 0]))
		st.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
		st.add_vertex(positions[index])
	var mesh := st.commit()
	var split := Lib.rigid_skin_parts(mesh)
	_check(not split.has("error") and int(split["triangles"]) == 2 and (split["parts"] as Array).size() == 2, "a two-bone mesh splits into two rigid parts (%s)" % split.get("error", ""))
	if not split.has("error") and (split["parts"] as Array).size() == 2:
		var second: Dictionary = split["parts"][1]
		var vertices: PackedVector3Array = second["arrays"][0][Mesh.ARRAY_VERTEX]
		var sources: PackedInt32Array = second["sources"][0]
		var kept: bool = int(second["bind"]) == 3 and second["surfaces"] == [0] and vertices.size() == 3
		for index in vertices.size():
			kept = kept and vertices[index] == positions[sources[index]] and sources[index] >= 3
		_check(kept and second["arrays"][0][Mesh.ARRAY_BONES] == null, "a rigid part keeps its vertices and their source indices, without bones")
	st.clear()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for index in 3:
		st.set_bones(PackedInt32Array([0, 1, 0, 0]))
		st.set_weights(PackedFloat32Array([0.5, 0.5, 0.0, 0.0]))
		st.add_vertex(positions[index])
	_check(Lib.rigid_skin_parts(st.commit()).has("error"), "blended bone weights are refused")
	st.clear()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for index in 3:
		st.set_bones(PackedInt32Array([index, 0, 0, 0]))
		st.set_weights(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
		st.add_vertex(positions[index])
	_check(Lib.rigid_skin_parts(st.commit()).has("error"), "a triangle that spans bones is refused")


## texture_png writes the base level's pixels as stored, independently of how the texture keeps
## them: channel order and alpha survive, mipmaps are dropped, nothing is converted to another
## color space, and a VRAM-compressed texture is written decompressed.
func _test_texture_png() -> void:
	var source := Image.create_empty(4, 2, false, Image.FORMAT_RGBA8)
	var expected := PackedByteArray()
	for y in 2:
		for x in 4:
			var texel := PackedByteArray([10 + 60 * x, 20 + 100 * y, 200 - 40 * x, 30 + 40 * x + 80 * y])
			source.set_pixel(x, y, Color8(texel[0], texel[1], texel[2], texel[3]))
			expected.append_array(texel)
	var mipmapped := source.duplicate() as Image
	mipmapped.generate_mipmaps()
	var decoded := Image.new()
	var png := Lib.texture_png(ImageTexture.create_from_image(mipmapped))
	var ok := not png.is_empty() and decoded.load_png_from_buffer(png) == OK
	if ok:
		decoded.convert(Image.FORMAT_RGBA8)
	_check(ok and decoded.get_size() == Vector2i(4, 2) and not decoded.has_mipmaps() and decoded.get_data() == expected, "a texture with mipmaps becomes a PNG of its base level's RGBA bytes")
	# One hand-made DXT1 block: endpoints red (0xF800) and blue (0x001F) in RGB565, the top two rows
	# use endpoint 0 and the bottom two endpoint 1, so the decoded texels are known exactly.
	var block := Image.create_from_data(4, 4, false, Image.FORMAT_DXT1, PackedByteArray([0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x55, 0x55]))
	var block_expected := PackedByteArray()
	for y in 4:
		for x in 4:
			block_expected.append_array(PackedByteArray([255, 0, 0, 255]) if y < 2 else PackedByteArray([0, 0, 255, 255]))
	decoded = Image.new()
	png = Lib.texture_png(ImageTexture.create_from_image(block))
	ok = block.is_compressed() and not png.is_empty() and decoded.load_png_from_buffer(png) == OK
	if ok:
		decoded.convert(Image.FORMAT_RGBA8)
	_check(ok and decoded.get_size() == Vector2i(4, 4) and decoded.get_data() == block_expected, "a VRAM-compressed texture becomes a PNG of its decoded texels")


func _check_determinism(p_first: String, p_second: String) -> void:
	var first: Variant = _read_json(p_first.path_join("export_manifest.json"))
	var second: Variant = _read_json(p_second.path_join("export_manifest.json"))
	_check(first is Dictionary and second is Dictionary, "both manifests parse")
	if not (first is Dictionary and second is Dictionary):
		return
	var names: Array = (first["files"] as Dictionary).keys()
	names.sort()
	var other: Array = (second["files"] as Dictionary).keys()
	other.sort()
	_check(names == other, "both exports list the same files")
	var required := ["library.glb", "axes.glb", "scene.json", "camera_track.csv"]
	if _has_sky:
		required.append("sky.hdr")
	for name: String in required:
		_check(name in names, "the export has %s" % name)
	for file_name: String in names:
		var a := FileAccess.get_sha256(p_first.path_join(file_name))
		var b := FileAccess.get_sha256(p_second.path_join(file_name))
		_check(not a.is_empty() and a == b, "%s is the same in both exports" % file_name)


func _check_manifest_and_load(p_dir: String) -> Dictionary:
	var manifest: Variant = _read_json(p_dir.path_join("export_manifest.json"))
	_check(manifest is Dictionary, "export_manifest.json parses")
	if not (manifest is Dictionary):
		return {}
	_check(int(manifest["schema"]) == Lib.SCHEMA, "manifest schema %d" % Lib.SCHEMA)
	_check(String(manifest["scene"]) == _scene_path, "manifest names %s" % _scene_path)
	_check(String(manifest["scene_sha256"]) == FileAccess.get_sha256(_scene_path), "manifest has the scene's SHA-256")
	var files: Dictionary = manifest["files"]
	for file_name: String in files:
		_check(String(files[file_name]) == FileAccess.get_sha256(p_dir.path_join(file_name)), "manifest checksum of %s" % file_name)
	var scene: Variant = _read_json(p_dir.path_join("scene.json"))
	_check(scene is Dictionary, "scene.json parses")
	return scene if scene is Dictionary else {}


## The keys and types ue_export_schema.md promises.
func _check_schema(p_scene: Dictionary) -> void:
	var top := {
		"schema": TYPE_FLOAT, "scene": TYPE_STRING, "units": TYPE_DICTIONARY, "fps": TYPE_FLOAT,
		"warmup": TYPE_FLOAT, "frames": TYPE_FLOAT, "camera": TYPE_DICTIONARY, "meshes": TYPE_ARRAY,
		"materials": TYPE_ARRAY, "textures": TYPE_ARRAY, "instances": TYPE_ARRAY, "lights": TYPE_ARRAY,
		"approximations": TYPE_ARRAY, "environment": TYPE_DICTIONARY, "rendering": TYPE_DICTIONARY,
		"checks": TYPE_DICTIONARY,
	}
	var before := _failures.size()
	for key: String in top:
		_check(p_scene.has(key) and typeof(p_scene[key]) == top[key], "scene.json has %s of the right type" % key)
	if _failures.size() > before:
		return
	_check(p_scene["units"] == {"length": "m", "up": "+Y", "handedness": "right", "camera_forward": "-Z", "light_forward": "-Z"}, "units")
	var camera: Dictionary = p_scene["camera"]
	for key in ["node", "track", "fov_deg", "fov_axis", "near", "far", "projection", "cuts"]:
		_check(camera.has(key), "camera has %s" % key)
	var mesh_ids := {}
	for mesh: Dictionary in p_scene["meshes"]:
		for key in ["id", "gltf_node", "surfaces", "triangles", "vertices", "aabb", "nextgeo"]:
			_check(mesh.has(key), "mesh has %s" % key)
		mesh_ids[String(mesh["id"])] = mesh
	var texture_ids := {}
	for texture: Dictionary in p_scene["textures"]:
		for key in ["id", "file", "class", "size", "mipmaps", "usages"]:
			_check(texture.has(key), "texture has %s" % key)
		texture_ids[String(texture["id"])] = true
	var material_ids := {}
	for material: Dictionary in p_scene["materials"]:
		material_ids[String(material["id"])] = true
		if String(material.get("blend", "")) == "shader":
			for key in ["id", "class", "shader", "properties"]:
				_check(material.has(key), "shader material has %s" % key)
			var shader: Dictionary = material.get("shader", {})
			_check(String(shader.get("sha256", "")) == String(shader.get("code", "")).sha256_text(), "shader material %s carries its code's SHA-256" % material["id"])
			for uniform: Dictionary in (shader.get("uniforms", {}) as Dictionary).values():
				_check(uniform.has("value") != uniform.has("texture"), "a shader uniform has a value or a texture")
				_check(not uniform.has("texture") or texture_ids.has(String(uniform["texture"])), "shader uniform texture exists")
			continue
		for key in ["id", "class", "blend", "alpha_scissor_threshold", "two_sided", "albedo_linear", "albedo_texture", "uv1_scale", "uv1_offset", "texture_filter", "texture_repeat", "emission_linear", "properties"]:
			_check(material.has(key), "material has %s" % key)
		_check(String(material.get("blend", "")) in ["opaque", "masked", "translucent"], "material blend is opaque, masked or translucent")
		_check(material.get("albedo_texture") == null or texture_ids.has(String(material["albedo_texture"])), "material texture exists")
		_check(String(material.get("texture_filter", "")) in ["nearest", "linear", "linear_mipmaps"], "material texture filter is carried")
	var placements: Array = []
	var triangles := 0
	var node_count := 0
	var multimesh_count := 0
	var blends := {}
	for material: Dictionary in p_scene["materials"]:
		blends[String(material["id"])] = String(material.get("blend", ""))
	var translucent_casters := PackedStringArray()
	for instance: Dictionary in p_scene["instances"]:
		if int(instance.get("cast_shadow", 0)) != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF:
			for material_id: Variant in instance["materials"]:
				if blends.get(String(material_id), "") == "translucent":
					translucent_casters.append(String(instance["node"]))
					break
	for instance: Dictionary in p_scene["instances"]:
		var kind := String(instance["kind"])
		_check(kind == "node" or kind == "multimesh", "instance kind is node or multimesh")
		_check(mesh_ids.has(String(instance["mesh"])), "instance mesh %s exists" % instance["mesh"])
		for material_id: Variant in instance["materials"]:
			_check(material_ids.has(String(material_id)), "instance material %s exists" % material_id)
		_check((instance["transform"] as Array).size() == 12, "instance transform has 12 numbers")
		_check(instance.has("layers"), "instance has layers")
		_check(kind == "multimesh" or instance.has("track"), "a node instance has a track entry")
		var mesh_triangles := int(mesh_ids[String(instance["mesh"])]["triangles"]) if mesh_ids.has(String(instance["mesh"])) else 0
		if kind == "node":
			placements.append(Lib.rows_to_transform(instance["transform"]))
			triangles += mesh_triangles
			node_count += 1
		else:
			_check((instance["instances"] as Array).size() == int(instance["count"]), "multimesh count matches its rows")
			for rows: Array in instance["instances"]:
				placements.append(Lib.rows_to_transform(rows))
				triangles += mesh_triangles
				multimesh_count += 1
	var light_types := {}
	var unlit_lights := PackedStringArray()
	for light: Dictionary in p_scene["lights"]:
		for key in ["node", "type", "transform", "direction", "color_linear", "projector", "properties", "cull_mask_effect", "caster_mask_effect"]:
			_check(light.has(key), "light has %s" % key)
		_check(String(light.get("type", "")) in ["directional", "omni", "spot", "area"], "light type is known")
		_check(String(light.get("cull_mask_effect", "")) in ["all", "none"], "an exported cull mask lights all placed layers or none (none is the light_cull_mask approximation)")
		if String(light.get("cull_mask_effect", "")) == "none":
			unlit_lights.append(String(light["node"]))
		_check(light.get("projector") == null or texture_ids.has(String(light["projector"])), "light projector texture exists")
		if light.get("type") == "area":
			_check(light.has("area") and (light["area"] as Dictionary).has("size") and (light["area"] as Dictionary).has("normalize_energy"), "area light has its size and normalization")
			_check(light["area"].get("texture") == null or texture_ids.has(String(light["area"]["texture"])), "area light texture exists")
		light_types[light["type"]] = int(light_types.get(light["type"], 0)) + 1
	for approximation: Dictionary in p_scene["approximations"]:
		_check(Lib.APPROXIMATIONS.has(String(approximation["id"])) and not (approximation["items"] as Array).is_empty(), "approximation %s is known and names its items" % approximation["id"])
	var approximations := _approximations(p_scene)
	_check(PackedStringArray(approximations.get("light_cull_mask", [])) == unlit_lights, "light_cull_mask names every light that lights no placed layer (%s)" % unlit_lights)
	_check(PackedStringArray(approximations.get("translucent_shadow", [])) == translucent_casters, "translucent_shadow names every shadow-casting instance with an alpha-blended surface (%s)" % translucent_casters)
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["node_instances"]) == node_count, "checks.node_instances")
	_check(int(checks["multimesh_instances"]) == multimesh_count, "checks.multimesh_instances")
	_check(int(checks["instances_total"]) == placements.size(), "checks.instances_total")
	_check(int(checks["triangles_total"]) == triangles, "checks.triangles_total")
	_check(int(checks["meshes"]) == mesh_ids.size() and int(checks["materials"]) == material_ids.size(), "checks.meshes and checks.materials")
	_check(int(checks["textures"]) == texture_ids.size(), "checks.textures")
	_check(int(checks["lights"]) == (p_scene["lights"] as Array).size(), "checks.lights")
	var stated_types := {}
	for type: String in checks["light_types"]:
		stated_types[type] = int(checks["light_types"][type])
	_check(stated_types == light_types, "checks.light_types matches the light rows")
	_check(String(checks["instances_sha256"]) == Lib.transforms_sha256(placements), "checks.instances_sha256 matches the instance rows")
	_check(int(checks["camera_rows"]) == int(p_scene["frames"]), "one camera row per frame")
	var movers := 0
	for instance: Dictionary in p_scene["instances"]:
		if instance.get("track") != null:
			movers += 1
	_check(int(checks["movers"]) == movers, "checks.movers counts the instances with a track")
	var cuts: Array = p_scene["camera"]["cuts"]
	for index in cuts.size():
		_check(int(cuts[index]) > 0 and int(cuts[index]) < int(p_scene["frames"]) and (index == 0 or int(cuts[index]) > int(cuts[index - 1])), "camera cuts are increasing frames inside the path")
	var rows: Dictionary = p_scene["rendering"]["record_rows"]
	_check(rows.has("rendering/global_illumination/nextgi/hardware_ray_tracing"), "rendering names the settings-record row of hardware_ray_tracing")


## The kitbash's own numbers: 2,400 rocks from the seeded plan plus the ground, a sun and a spot.
func _check_kitbash(p_scene: Dictionary) -> void:
	var plan: Dictionary = Layout.plan(Layout.DEFAULT_INSTANCES, Layout.SEED)
	var positions: PackedVector3Array = plan["positions"]
	var rotations: PackedVector3Array = plan["rotations"]
	var scales: PackedFloat32Array = plan["scales"]
	var variants: PackedInt32Array = plan["variants"]
	var count := positions.size()
	var expected: Array[Transform3D] = []
	for index in count:
		var basis := Basis.from_euler(rotations[index]) * Basis.from_scale(Vector3.ONE * float(scales[index]))
		expected.append(Transform3D(basis, positions[index]))
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["instances_total"]) == count + 1, "2,400 rocks and the ground (%d)" % int(checks["instances_total"]))
	_check(int(checks["multimesh_instances"]) == count / 2 and int(checks["multimesh_nodes"]) == Layout.ROCK_VARIANTS, "1,200 MultiMesh rocks in 4 MultiMeshes")
	_check(int(checks["lights"]) == 2, "a sun and the off-screen spot")
	var exported: Array = []
	exported.resize(count)
	var mismatched := 0
	var cursors := {}
	var by_variant := {}
	for index in range(count / 2, count):
		var variant := int(variants[index])
		if not by_variant.has(variant):
			by_variant[variant] = []
		(by_variant[variant] as Array).append(index)
	var ground := 0
	for instance: Dictionary in p_scene["instances"]:
		var node := String(instance["node"])
		if instance["kind"] == "node" and node.begins_with("Instances/Meshes/Rock"):
			var index := int(node.trim_prefix("Instances/Meshes/Rock"))
			exported[index] = Lib.rows_to_transform(instance["transform"])
		elif instance["kind"] == "multimesh":
			var variant := int(node.trim_prefix("Instances/MultiMeshes/RockVariant"))
			var indices: Array = by_variant.get(variant, [])
			_check(indices.size() == int(instance["count"]), "MultiMesh variant %d has the plan's count" % variant)
			for row in mini(indices.size(), (instance["instances"] as Array).size()):
				exported[int(indices[row])] = Lib.rows_to_transform(instance["instances"][row])
		elif node == "Ground":
			ground += 1
			_check(Lib.rows_to_transform(instance["transform"]).is_equal_approx(Transform3D(Basis(), Vector3(0.0, -0.1, 0.0))), "the ground sits at y = -0.1")
	_check(ground == 1, "one ground instance")
	var placed := PackedVector3Array()
	for index in count:
		if not (exported[index] is Transform3D):
			mismatched += 1
			placed.append(Vector3())
			continue
		var transform: Transform3D = exported[index]
		placed.append(transform.origin)
		if not _transforms_close(transform, expected[index]):
			mismatched += 1
	_check(mismatched == 0, "every rock placement equals the seeded plan (%d differ)" % mismatched)
	_check(Layout.positions_sha256(placed) == Layout.positions_sha256(positions), "the placements hash to the scene's positions_sha256")


## The yard (#233): sun and 64 local lights, the 20-layer caster-mask matrix, two alpha-scissor
## foliage cards and three alpha-blended glass panes, exported with the diffuse_mode,
## shadow_caster_mask and translucent_shadow approximations.
func _check_yard(p_scene: Dictionary) -> void:
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["node_instances"]) == 34 and int(checks["multimesh_instances"]) == 0, "the ground, 20 occluders, 2 foliage cards, 3 glass panes and 8 pillars (%d)" % int(checks["node_instances"]))
	_check(_int_counts(checks["light_types"]) == {"directional": 1, "spot": 20, "omni": 44}, "a sun, 20 matrix spots and 44 omnis (%s)" % checks["light_types"])
	var occluders := 0
	for instance: Dictionary in p_scene["instances"]:
		var node := String(instance["node"])
		if node.begins_with("LayerOccluder"):
			occluders += 1
			_check(int(instance["layers"]) == 1 << int(node.trim_prefix("LayerOccluder")), "%s sits on its own layer" % node)
		else:
			_check(int(instance["layers"]) == 1, "%s sits on layer 1" % node)
	_check(occluders == 20, "20 layer occluders")
	var matrix := PackedStringArray()
	for light: Dictionary in p_scene["lights"]:
		var node := String(light["node"])
		var expected := "some" if node.begins_with("MatrixLight") else "all"
		_check(String(light["caster_mask_effect"]) == expected, "%s caster mask keeps %s placed layers" % [node, expected])
		_check(String(light["cull_mask_effect"]) == "all", "%s lights every placed layer" % node)
		if node.begins_with("MatrixLight"):
			matrix.append(node)
			_check(int(light["properties"]["shadow_caster_mask"]) == 1 << int(node.trim_prefix("MatrixLight")), "%s keeps its caster mask in its properties" % node)
	var approximations := _approximations(p_scene)
	_check(approximations.keys() == ["diffuse_mode", "shadow_caster_mask", "translucent_shadow"], "the yard needs diffuse_mode, shadow_caster_mask and translucent_shadow (%s)" % [approximations.keys()])
	_check(PackedStringArray(approximations.get("shadow_caster_mask", [])) == matrix, "shadow_caster_mask names the 20 matrix lights")
	_check((approximations.get("diffuse_mode", []) as Array).size() == 5, "diffuse_mode names the 2 foliage and 3 glass materials")
	_check(PackedStringArray(approximations.get("translucent_shadow", [])) == PackedStringArray(["YardGlass0", "YardGlass1", "YardGlass2"]), "translucent_shadow names the 3 glass panes")
	var masked := 0
	var translucent := 0
	for material: Dictionary in p_scene["materials"]:
		match String(material["blend"]):
			"masked":
				masked += 1
				_check(material["albedo_texture"] != null and bool(material["two_sided"]) and is_equal_approx(float(material["alpha_scissor_threshold"]), 0.5), "foliage %s is two-sided, textured and clipped at 0.5" % material["name"])
			"translucent":
				translucent += 1
				_check(material["albedo_texture"] == null and is_equal_approx(float(material["albedo_linear"][3]), 0.35) and bool(material["two_sided"]), "glass %s is two-sided with opacity 0.35" % material["name"])
	_check(masked == 2 and translucent == 3, "2 masked and 3 translucent materials (%d, %d)" % [masked, translucent])
	_check(int(checks["textures"]) == 2, "the two foliage textures")


## MegaLights Hall at --lights=1024 (#233): 64 base lights (42 points, 8 spots, 8 rects of which
## 4 textured, 4 IES projector spots, 2 lighting-channel omnis) plus 960 spawned omnis, the sun,
## volumetric fog, glass and foliage, with the diffuse_mode, light_cull_mask, light_projector and
## translucent_shadow approximations.
func _check_hall(p_scene: Dictionary) -> void:
	var checks: Dictionary = p_scene["checks"]
	_check(_int_counts(checks["light_types"]) == {"directional": 1, "omni": 1004, "spot": 12, "area": 8}, "the sun and 1,024 local lights (%s)" % checks["light_types"])
	_check(int(checks["node_instances"]) == 14, "the ground, 3 walls, 4 glass panes and 6 foliage cards (%d)" % int(checks["node_instances"]))
	var projectors := PackedStringArray()
	var textured_areas := 0
	var area_texture: Variant = null
	for light: Dictionary in p_scene["lights"]:
		var node := String(light["node"])
		_check(String(light["cull_mask_effect"]) == ("none" if node.begins_with("Lights/ChannelLight") else "all"), "%s lights the placed layers as its cull mask says" % node)
		_check(String(light["caster_mask_effect"]) == "all", "%s casts from every placed layer" % node)
		if light["projector"] != null:
			projectors.append(node)
		if light["type"] == "area":
			_check(light["area"]["size"] == [3.0, 1.5] and bool(light["area"]["normalize_energy"]), "%s is 3 x 1.5 m, energy normalized" % node)
			if light["area"]["texture"] != null:
				textured_areas += 1
				_check(area_texture == null or area_texture == light["area"]["texture"], "the textured rect lights share one texture")
				area_texture = light["area"]["texture"]
	_check(projectors == PackedStringArray(["Lights/Ies00", "Lights/Ies01", "Lights/Ies02", "Lights/Ies03"]), "the four IES spots carry their projector")
	_check(textured_areas == 4, "four textured rect lights")
	var approximations := _approximations(p_scene)
	_check(approximations.keys() == ["diffuse_mode", "light_cull_mask", "light_projector", "translucent_shadow"], "the hall needs diffuse_mode, light_cull_mask, light_projector and translucent_shadow (%s)" % [approximations.keys()])
	_check(PackedStringArray(approximations.get("light_projector", [])) == projectors, "light_projector names the four IES spots")
	_check(PackedStringArray(approximations.get("light_cull_mask", [])) == PackedStringArray(["Lights/ChannelLight0", "Lights/ChannelLight1"]), "light_cull_mask names the two lighting-channel omnis")
	_check(PackedStringArray(approximations.get("translucent_shadow", [])) == PackedStringArray(["FrontGlass0", "FrontGlass1", "FrontGlass2", "FrontGlass3"]), "translucent_shadow names the 4 glass panes")
	_check(bool(p_scene["environment"]["properties"]["volumetric_fog_enabled"]), "the environment's volumetric fog is carried")
	var masked := 0
	var translucent := 0
	for material: Dictionary in p_scene["materials"]:
		match String(material["blend"]):
			"masked":
				masked += 1
			"translucent":
				translucent += 1
	_check(masked == 6 and translucent == 4, "6 foliage and 4 glass materials (%d, %d)" % [masked, translucent])
	_check(int(checks["textures"]) == 11, "6 foliage, 1 rect-light and 4 projector textures (%d)" % int(checks["textures"]))


## Upscaler Torture (#232): 55 placed meshes (24 wires, the fence, the checker floor and backdrop,
## the moiré plane, the fan's 4 blades and hub, the scrolling emissive, 2 grilles of bars and a
## backing, 12 cables, and the character's 5 rigid parts), 10 of them moving; a sun and a fill
## light; the cut at frame 400; the fan and occluder motion and the character's bone poses
## recomputed from the scene's nodes and animation at TORTURE_FRAMES; the fence, checker and moiré
## materials' UV transforms and filters; the scrolling emissive's shader; and the alpha_antialiasing,
## diffuse_mode, particles, rigid_skin and shader_material approximations.
func _check_torture(p_dir: String, p_scene: Dictionary) -> void:
	var checks: Dictionary = p_scene["checks"]
	_check(int(checks["node_instances"]) == 55 and int(checks["multimesh_instances"]) == 0, "55 placed meshes (%d)" % int(checks["node_instances"]))
	_check(int(checks["movers"]) == 10, "the fan's 5 meshes and the character's 5 parts move (%d)" % int(checks["movers"]))
	_check(_int_counts(checks["light_types"]) == {"directional": 1, "omni": 1}, "the sun and the fill light (%s)" % checks["light_types"])
	_check(float(checks["rigid_skin_max_error_m"]) <= 1e-4, "the rigid parts match the renderer's skinning within 0.1 mm (%s)" % checks["rigid_skin_max_error_m"])
	_check(p_scene["camera"]["cuts"] == [float(TORTURE_CUT)], "the camera cuts at frame %d (%s)" % [TORTURE_CUT, p_scene["camera"]["cuts"]])
	var environment: Dictionary = p_scene["environment"]["properties"]
	_check(int(environment["background_mode"]) == Environment.BG_COLOR and int(environment["ambient_light_source"]) == Environment.AMBIENT_SOURCE_COLOR, "a color background and color ambient light")
	var approximations := _approximations(p_scene)
	_check(approximations.keys() == ["alpha_antialiasing", "diffuse_mode", "particles", "rigid_skin", "shader_material"], "the torture needs alpha_antialiasing, diffuse_mode, particles, rigid_skin and shader_material (%s)" % [approximations.keys()])
	_check(approximations.get("particles", []) == ["AlphaParticles"], "particles names the alpha particles")
	_check(approximations.get("rigid_skin", []) == ["Occluders/SkinnedCharacter/CharacterMesh"], "rigid_skin names the character")
	var by_node := {}
	var parts := 0
	for instance: Dictionary in p_scene["instances"]:
		var node := String(instance["node"])
		if instance.has("skin_part"):
			parts += 1
			_check(node == "Occluders/SkinnedCharacter/CharacterMesh", "only the character has rigid parts")
			by_node["%s|%d" % [node, int(instance["skin_part"]["bind"])]] = instance
		else:
			by_node[node] = instance
		_check((instance["track"] != null) == node.begins_with("Occluders/"), "%s has a track exactly when it is an occluder" % node)
	_check(parts == 5, "the character splits into 5 rigid parts (hips has no triangles) (%d)" % parts)
	# The shell stays out of the tree (a SceneTree script's _initialize runs before the root
	# enters it): world transforms are the products of the local ones, and the bone poses come
	# from the animation's own keys.
	var shell := (load(_scene_path) as PackedScene).instantiate() as Node3D
	shell.set_meta("skip_runtime", true)
	var tracks := {}
	for key: String in by_node:
		var instance: Dictionary = by_node[key]
		if instance["track"] != null:
			var rows := _read_track(p_dir.path_join(String(instance["track"])))
			_check(rows.size() == int(p_scene["frames"]), "%s track has one row per frame" % key)
			if not rows.is_empty():
				_check(_transforms_close(rows[0], Lib.rows_to_transform(instance["transform"])), "%s track row 0 is its placement" % key)
			tracks[key] = rows
	var occluders := shell.get_node("Occluders") as Node3D
	var fan := shell.get_node("Occluders/Fan") as Node3D
	var player := shell.get_node("Occluders/SkinnedCharacter/AnimationPlayer") as AnimationPlayer
	var skeleton := shell.get_node("Occluders/SkinnedCharacter/Skeleton3D") as Skeleton3D
	var character := shell.get_node("Occluders/SkinnedCharacter/CharacterMesh") as MeshInstance3D
	var animation := player.get_animation("swing")
	var off := 0
	var compared := 0
	for frame: int in TORTURE_FRAMES:
		if frame >= int(p_scene["frames"]):
			continue
		occluders.position.x = -1.35 if frame < 300 else (lerpf(-1.35, 1.35, float(frame - 300) / 40.0) if frame <= 340 else 1.35)
		fan.rotation.z = float(frame) * 0.24
		var bones := _animated_bones(skeleton, animation, fposmod(float(frame) / 60.0 * 3.0, 0.5))
		for key: String in tracks:
			var rows: Array = tracks[key]
			if frame >= rows.size():
				continue
			var expected: Transform3D
			if key.contains("|"):
				var bind := int(key.get_slice("|", 1))
				var bone := character.skin.get_bind_bone(bind)
				expected = _global(character) * bones[bone] * character.skin.get_bind_pose(bind)
			else:
				expected = _global(shell.get_node(key) as Node3D)
			compared += 1
			if not _transforms_close(rows[frame], expected, 1e-4):
				off += 1
	_check(compared == tracks.size() * TORTURE_FRAMES.size() and off == 0, "every track row is the occluder's pose at frames %s (%d of %d differ)" % [TORTURE_FRAMES, off, compared])
	shell.free()
	var materials := {}
	for material: Dictionary in p_scene["materials"]:
		materials[String(material["id"])] = material
	var textures := {}
	for texture: Dictionary in p_scene["textures"]:
		textures[String(texture["id"])] = texture
	var expectations := {
		"ChainLinkFence": {"blend": "masked", "uv1_scale": [4.0, 2.0, 1.0], "texture_filter": "linear_mipmaps", "mipmaps": true, "two_sided": true},
		"CheckerPlane": {"blend": "opaque", "uv1_scale": [48.0, 48.0, 1.0], "texture_filter": "nearest", "mipmaps": false, "two_sided": false},
		"CheckerBackdrop": {"blend": "opaque", "uv1_scale": [16.0, 8.0, 1.0], "texture_filter": "nearest", "mipmaps": false, "two_sided": false},
		"MoirePlane": {"blend": "opaque", "uv1_scale": [1.0, 1.0, 1.0], "texture_filter": "linear_mipmaps", "mipmaps": true, "two_sided": true},
	}
	for node: String in expectations:
		var instance: Dictionary = by_node.get(node, {})
		var material: Dictionary = materials.get(String(instance.get("materials", [""])[0]), {})
		var want: Dictionary = expectations[node]
		var texture: Dictionary = textures.get(String(material.get("albedo_texture", "")), {})
		_check(material.get("blend") == want["blend"] and material.get("uv1_scale") == want["uv1_scale"] and material.get("uv1_offset") == [0.0, 0.0, 0.0] and material.get("texture_filter") == want["texture_filter"] and bool(material.get("texture_repeat", false)) and bool(material.get("two_sided", false)) == want["two_sided"] and texture.get("mipmaps") == want["mipmaps"], "%s: %s blend, UV1 scale %s, %s filtering, mipmaps %s (%s, %s)" % [node, want["blend"], want["uv1_scale"], want["texture_filter"], want["mipmaps"], material.get("uv1_scale"), material.get("texture_filter")])
	var emissive: Dictionary = materials.get(String(by_node.get("ScrollingEmissive", {}).get("materials", [""])[0]), {})
	var source_shader := ((load(_scene_path) as PackedScene).instantiate() as Node3D)
	var code := (((source_shader.get_node("ScrollingEmissive") as MeshInstance3D).material_override as ShaderMaterial).shader.code)
	source_shader.free()
	var shader: Dictionary = emissive.get("shader", {})
	var uniforms: Dictionary = shader.get("uniforms", {})
	_check(emissive.get("blend") == "shader" and shader.get("code") == code and shader.get("sha256") == code.sha256_text(), "the scrolling emissive carries the scene's shader code and its SHA-256")
	var uniform_names: Array = uniforms.keys()
	uniform_names.sort()
	_check(uniform_names == ["scroll_speed", "stripes", "time_offset"] and uniforms["stripes"].has("texture") and is_equal_approx(float(uniforms["scroll_speed"].get("value", 0.0)), 2.0), "the shader's uniforms: the stripes texture and a scroll speed of 2 (%s)" % [uniforms])
	_check(approximations.get("shader_material", []) == ["%s" % _material_label(emissive)], "shader_material names the scrolling emissive's material")


## A node's world transform outside the tree: its local transforms multiplied up to the scene root.
func _global(p_node: Node3D) -> Transform3D:
	var transform := p_node.transform
	var parent := p_node.get_parent()
	while parent is Node3D:
		transform = (parent as Node3D).transform * transform
		parent = parent.get_parent()
	return transform


## Every bone's skeleton-space pose at p_time from the animation's rotation keys: each bone's
## stored pose (position, rotation, scale; the scene stores zero positions, not its rests), its
## rotation replaced where the animation has a rotation track for it, times its parent's pose.
func _animated_bones(p_skeleton: Skeleton3D, p_animation: Animation, p_time: float) -> Array[Transform3D]:
	var rotations := {}
	for track in p_animation.get_track_count():
		if p_animation.track_get_type(track) == Animation.TYPE_ROTATION_3D:
			var path := p_animation.track_get_path(track)
			rotations[String(path.get_concatenated_subnames())] = p_animation.rotation_track_interpolate(track, p_time)
	var poses: Array[Transform3D] = []
	for bone in p_skeleton.get_bone_count():
		var rotation := p_skeleton.get_bone_pose_rotation(bone)
		var name := p_skeleton.get_bone_name(bone)
		if rotations.has(name):
			rotation = rotations[name] as Quaternion
		var local := Transform3D(Basis(rotation).scaled_local(p_skeleton.get_bone_pose_scale(bone)), p_skeleton.get_bone_pose_position(bone))
		var parent := p_skeleton.get_bone_parent(bone)
		poses.append(poses[parent] * local if parent >= 0 else local)
	return poses


func _material_label(p_material: Dictionary) -> String:
	var name := String(p_material.get("name", ""))
	return String(p_material["id"]) if name.is_empty() else "%s (%s)" % [p_material["id"], name]


## A track CSV as transforms, one per row (empty when it does not parse).
func _read_track(p_path: String) -> Array:
	var file := FileAccess.open(p_path, FileAccess.READ)
	if file == null or file.get_line() != "frame,m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23":
		return []
	var rows: Array = []
	while not file.eof_reached():
		var line := file.get_line()
		if line.is_empty():
			continue
		var cells := line.split(",")
		if cells.size() != 13 or int(cells[0]) != rows.size():
			return []
		var values: Array = []
		for index in range(1, 13):
			values.append(float(cells[index]))
		rows.append(Lib.rows_to_transform(values))
	return rows


## JSON numbers come back as floats.
func _int_counts(p_counts: Dictionary) -> Dictionary:
	var counts := {}
	for key: String in p_counts:
		counts[key] = int(p_counts[key])
	return counts


func _approximations(p_scene: Dictionary) -> Dictionary:
	var found := {}
	for entry: Dictionary in p_scene["approximations"]:
		found[String(entry["id"])] = entry["items"]
	return found


## Every exported texture is a PNG of its stated size, and the textures the scene's own nodes use
## (material albedo textures, rect-light textures, projectors) are written byte for byte.
func _check_textures(p_dir: String, p_scene: Dictionary) -> void:
	var files := {}
	for texture: Dictionary in p_scene["textures"]:
		var bytes := FileAccess.get_file_as_bytes(p_dir.path_join(String(texture["file"])))
		var image := Image.new()
		var ok := not bytes.is_empty() and image.load_png_from_buffer(bytes) == OK
		_check(ok and image.get_size() == Vector2i(int(texture["size"][0]), int(texture["size"][1])), "%s is a PNG of its stated size" % texture["file"])
		files[String(texture["id"])] = bytes
	var materials := {}
	for material: Dictionary in p_scene["materials"]:
		materials[String(material["id"])] = material
	var shell := (load(_scene_path) as PackedScene).instantiate()
	shell.set_meta("skip_runtime", true)
	var compared := {}
	for instance: Dictionary in p_scene["instances"]:
		var node := shell.get_node_or_null(NodePath(String(instance["node"]))) as MeshInstance3D
		if node == null or instance["kind"] != "node":
			continue
		for surface in (instance["materials"] as Array).size():
			var source_surface := surface
			if instance.has("skin_part"):
				source_surface = int(instance["skin_part"]["surfaces"][surface])
			var material: Dictionary = materials.get(String(instance["materials"][surface]), {})
			if node.get_active_material(source_surface) is ShaderMaterial:
				var shader_material := node.get_active_material(source_surface) as ShaderMaterial
				for name: String in material.get("shader", {}).get("uniforms", {}):
					var uniform: Dictionary = material["shader"]["uniforms"][name]
					if uniform.has("texture"):
						compared[String(uniform["texture"])] = true
						_check(files.get(String(uniform["texture"]), PackedByteArray()) == _source_png(shader_material.get_shader_parameter(name)), "%s uniform %s texture is the scene's" % [instance["node"], name])
				continue
			var source := node.get_active_material(source_surface) as BaseMaterial3D
			if source == null or source.albedo_texture == null:
				_check(material.get("albedo_texture") == null, "%s surface %d has no texture" % [instance["node"], surface])
				continue
			compared[String(material.get("albedo_texture", ""))] = true
			_check(files.get(String(material.get("albedo_texture", "")), PackedByteArray()) == _source_png(source.albedo_texture), "%s surface %d texture is the scene's" % [instance["node"], surface])
	for light: Dictionary in p_scene["lights"]:
		var node := shell.get_node_or_null(NodePath(String(light["node"]))) as Light3D
		if node == null:
			continue
		if node.light_projector != null:
			compared[String(light["projector"])] = true
			_check(files.get(String(light["projector"]), PackedByteArray()) == _source_png(node.light_projector), "%s projector is the scene's" % light["node"])
		if node is AreaLight3D and (node as AreaLight3D).area_texture != null:
			compared[String(light["area"]["texture"])] = true
			_check(files.get(String(light["area"]["texture"]), PackedByteArray()) == _source_png((node as AreaLight3D).area_texture), "%s rect texture is the scene's" % light["node"])
	var compared_ids: Array = compared.keys()
	compared_ids.sort()
	var texture_ids: Array = files.keys()
	texture_ids.sort()
	_check(compared_ids == texture_ids, "every texture was compared with its source (%s of %s)" % [compared_ids, texture_ids])
	shell.free()


## The PNG of a scene texture, from a fresh copy: the dummy renderer of a headless run keeps the
## image of a texture it creates, not of one a GradientTexture2D replaced after a placeholder.
func _source_png(p_texture: Texture2D) -> PackedByteArray:
	return Lib.texture_png(p_texture.duplicate() as Texture2D)


func _check_library(p_dir: String, p_scene: Dictionary) -> void:
	var nodes := _gltf_meshes(p_dir.path_join("library.glb"))
	_check(nodes.size() == (p_scene["meshes"] as Array).size(), "library.glb has one mesh node per scene mesh (%d)" % nodes.size())
	for mesh: Dictionary in p_scene["meshes"]:
		var name := String(mesh["gltf_node"])
		_check(nodes.has(name), "library.glb has node %s" % name)
		if not nodes.has(name):
			continue
		var imported: Dictionary = nodes[name]
		_check(int(imported["triangles"]) == int(mesh["triangles"]), "%s keeps %d triangles (%d)" % [name, int(mesh["triangles"]), int(imported["triangles"])])
		var aabb: AABB = imported["aabb"]
		var stated := AABB(_vector(mesh["aabb"]["position"]), _vector(mesh["aabb"]["size"]))
		# An ArrayMesh pads a flat axis of its bounds by 1e-5 (a PlaneMesh's height), so a small
		# absolute tolerance.
		_check((aabb.position - stated.position).length() <= 1e-4 and (aabb.size - stated.size).length() <= 1e-4, "%s keeps its bounds" % name)
		_check(Transform3D(imported["transform"]).is_equal_approx(Transform3D.IDENTITY), "%s sits at the origin" % name)


func _check_axes(p_dir: String) -> void:
	var nodes := _gltf_meshes(p_dir.path_join("axes.glb"))
	var expected := {"axis_o": Vector3.ZERO, "axis_x": Vector3.RIGHT, "axis_y": Vector3.UP, "axis_z": Vector3.BACK}
	for name: String in expected:
		_check(nodes.has(name), "axes.glb has %s" % name)
		if nodes.has(name):
			var center: Vector3 = (nodes[name]["aabb"] as AABB).get_center()
			_check(center.is_equal_approx(expected[name]), "%s is centered at %s (%s)" % [name, expected[name], center])


## Every camera row against the camera's pose: the static pose of the kitbash, yard and hall
## cameras, and for Upscaler Torture the pose recomputed from the scene's curve (the timeline's
## progress for that frame, then the look-at target), plus the cut: the only large jump in the path
## is at the cut frame.
func _check_camera_track(p_dir: String, p_scene: Dictionary) -> void:
	var shell := (load(_scene_path) as PackedScene).instantiate() as Node3D
	shell.set_meta("skip_runtime", true)
	var torture := _scene_path == String(SCENES["torture"]["scene"])
	var camera := shell.get_node(_camera_name) as Camera3D
	var pose := camera.transform.orthonormalized()
	var follow := shell.get_node_or_null("CameraPath/CameraFollow") as PathFollow3D
	var curve: Curve3D = null
	if torture:
		curve = (shell.get_node("CameraPath") as Path3D).curve
		_check(follow.rotation_mode == PathFollow3D.ROTATION_NONE and _global(follow.get_parent() as Node3D).is_equal_approx(Transform3D.IDENTITY), "the torture camera follows its path without rotation, from the origin")
	var origins := PackedVector3Array()
	var file := FileAccess.open(p_dir.path_join("camera_track.csv"), FileAccess.READ)
	_check(file != null, "camera_track.csv opens")
	if file == null:
		shell.free()
		return
	_check(file.get_line() == "frame,px,py,pz,qx,qy,qz,qw,fov_deg,near,far", "camera_track.csv header")
	var rows := 0
	var off := 0
	while not file.eof_reached():
		var line := file.get_line()
		if line.is_empty():
			continue
		var cells := line.split(",")
		if cells.size() != 11 or int(cells[0]) != rows:
			off += 1
			rows += 1
			continue
		var q := Quaternion(float(cells[4]), float(cells[5]), float(cells[6]), float(cells[7]))
		var row_pose := Transform3D(Basis(q), Vector3(float(cells[1]), float(cells[2]), float(cells[3])))
		origins.append(row_pose.origin)
		if torture:
			# The follower sits on the curve at the timeline's progress; the camera looks at the
			# timeline's target from there (-Z forward, +Y up).
			var position := curve.sample_baked(_torture_progress(rows) * curve.get_baked_length(), follow.cubic_interp)
			pose = Transform3D(Basis.looking_at(TORTURE_TARGET - position, Vector3.UP), position)
		if not _transforms_close(row_pose, pose, 1e-4) or absf(float(cells[8]) - camera.fov) > 1e-6:
			off += 1
		rows += 1
	_check(rows == int(p_scene["frames"]), "camera_track.csv has %d rows (%d)" % [int(p_scene["frames"]), rows])
	_check(off == 0, "every camera row is %s's pose and fov (%d differ)" % [_camera_name, off])
	if torture:
		var jumps := PackedInt32Array()
		var still := 0
		for index in range(1, origins.size()):
			var step := origins[index].distance_to(origins[index - 1])
			if step > 0.5:
				jumps.append(index)
			if index < 200 and step == 0.0:
				still += 1
		_check(jumps == PackedInt32Array([TORTURE_CUT]), "the path's only jump is the cut at frame %d (%s)" % [TORTURE_CUT, jumps])
		_check(still == 199, "the camera holds still through frames 0 to 199 (%d still steps)" % still)
	shell.free()


## torture_timeline.gd's path progress for a frame.
func _torture_progress(p_frame: int) -> float:
	if p_frame < 200:
		return 0.0
	if p_frame < 400:
		return float(p_frame - 200) / 199.0 * 0.45
	return 0.78 + clampf(float(p_frame - 400) / 199.0, 0.0, 1.0) * 0.22


func _check_sky(p_dir: String, p_scene: Dictionary) -> void:
	var sky_entry: Dictionary = p_scene["environment"]["sky"]
	_check(String(sky_entry.get("panorama", "")) == "sky.hdr" and String(sky_entry.get("panorama_projection", "")) == "godot_equirect", "the environment names sky.hdr")
	var decoded := Lib.decode_hdr(FileAccess.get_file_as_bytes(p_dir.path_join("sky.hdr")))
	_check(not decoded.has("error"), "sky.hdr decodes")
	if decoded.has("error"):
		return
	var width := int(decoded["width"])
	var height := int(decoded["height"])
	_check(width == Lib.SKY_WIDTH and height == Lib.SKY_HEIGHT, "sky.hdr is %dx%d" % [Lib.SKY_WIDTH, Lib.SKY_HEIGHT])
	var shell := (load(_scene_path) as PackedScene).instantiate()
	shell.set_meta("skip_runtime", true)
	var environment := (shell.get_node("WorldEnvironment") as WorldEnvironment).environment
	var sun := shell.get_node("Sun") as DirectionalLight3D
	var suns := [{
		"direction": sun.transform.basis.z.normalized(),
		"color": sun.light_color.srgb_to_linear(),
		"energy": sun.light_energy,
		"size": deg_to_rad(sun.light_angular_distance),
	}]
	var sky := environment.sky.sky_material as ProceduralSkyMaterial
	var worst := 0.0
	for sample: Vector2i in [Vector2i(0, 0), Vector2i(100, 60), Vector2i(256, 127), Vector2i(300, 128), Vector2i(511, 255), Vector2i(64, 90)]:
		var expected := Lib.procedural_sky_radiance(sky, suns, Lib.panorama_direction(sample.x, sample.y, width, height))
		var base := (sample.y * width + sample.x) * 3
		var pixels: PackedFloat32Array = decoded["pixels"]
		var peak := maxf(expected.r, maxf(expected.g, expected.b))
		for channel in 3:
			worst = maxf(worst, absf(pixels[base + channel] - [expected.r, expected.g, expected.b][channel]) / peak)
	_check(worst < 0.01, "sky.hdr holds the procedural sky (worst relative error %f)" % worst)
	var zenith := Lib.procedural_sky_radiance(sky, [], Vector3.UP)
	_check(zenith.is_equal_approx(sky.sky_top_color.srgb_to_linear()), "the zenith is the linear sky top color")
	shell.free()


## Mesh nodes of a glTF file: name -> {triangles, aabb, transform}.
func _gltf_meshes(p_path: String) -> Dictionary:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var result := {}
	if document.append_from_file(p_path, state) != OK:
		_check(false, "%s imports" % p_path.get_file())
		return result
	var root_node := document.generate_scene(state)
	var stack: Array[Node] = [root_node]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is MeshInstance3D and (node as MeshInstance3D).mesh != null:
			var mesh := (node as MeshInstance3D).mesh
			var triangles := 0
			for surface in mesh.get_surface_count():
				var arrays := mesh.surface_get_arrays(surface)
				var indices: Variant = arrays[Mesh.ARRAY_INDEX]
				triangles += (indices as PackedInt32Array).size() / 3 if indices != null else (arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size() / 3
			result[String(node.name)] = {"triangles": triangles, "aabb": mesh.get_aabb(), "transform": (node as Node3D).transform}
		for child in node.get_children():
			stack.append(child)
	root_node.free()
	return result


func _transforms_close(p_a: Transform3D, p_b: Transform3D, p_epsilon: float = EPSILON) -> bool:
	for axis in 3:
		if not (p_a.basis[axis] - p_b.basis[axis]).length() <= p_epsilon:
			return false
	return (p_a.origin - p_b.origin).length() <= p_epsilon


func _vector(p_value: Array) -> Vector3:
	return Vector3(float(p_value[0]), float(p_value[1]), float(p_value[2]))


func _read_json(p_path: String) -> Variant:
	var text := FileAccess.get_file_as_string(p_path)
	if text.is_empty():
		return null
	return JSON.parse_string(text)
