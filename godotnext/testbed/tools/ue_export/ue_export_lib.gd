extends RefCounted
## Format helpers for the UE comparison exporter (#235). ue_export.gd drives them, and
## tests/ue_export/check_ue_export.gd reads an export back with them. The format is described
## in ue_export_schema.md.

## 2 (#233): material blend modes and albedo textures, area lights, layers and light masks,
## textures and approximations. An import written for schema 1 builds every material opaque.
## 3 (#232): per-frame transform tracks for moving mesh instances, camera cuts, UV1 scale and
## offset, texture filtering and mipmaps, skinned meshes as rigid per-bone parts, ShaderMaterials
## and particles (approximations). An import written for schema 2 places movers at frame 0 only.
const SCHEMA := 3
const SKY_WIDTH := 512
const SKY_HEIGHT := 256


## A transform as 12 numbers: the 3x4 matrix, row-major, in Godot world space
## (world = M * [x, y, z, 1]).
static func transform_rows(p_transform: Transform3D) -> Array:
	var b := p_transform.basis
	var o := p_transform.origin
	return [
		b.x.x, b.y.x, b.z.x, o.x,
		b.x.y, b.y.y, b.z.y, o.y,
		b.x.z, b.y.z, b.z.z, o.z,
	]


static func rows_to_transform(p_rows: Array) -> Transform3D:
	var basis := Basis(
			Vector3(float(p_rows[0]), float(p_rows[4]), float(p_rows[8])),
			Vector3(float(p_rows[1]), float(p_rows[5]), float(p_rows[9])),
			Vector3(float(p_rows[2]), float(p_rows[6]), float(p_rows[10])))
	return Transform3D(basis, Vector3(float(p_rows[3]), float(p_rows[7]), float(p_rows[11])))


## SHA-256 of a list of transforms, each as 12 float32 values in transform_rows order.
static func transforms_sha256(p_transforms: Array) -> String:
	var values := PackedFloat32Array()
	for transform: Transform3D in p_transforms:
		for value: float in transform_rows(transform):
			values.append(value)
	return bytes_sha256(values.to_byte_array())


## SHA-256 of the origins, as the kitbash prints its plan (PackedVector3Array bytes).
static func positions_sha256(p_transforms: Array) -> String:
	var positions := PackedVector3Array()
	for transform: Transform3D in p_transforms:
		positions.append(transform.origin)
	return bytes_sha256(positions.to_byte_array())


static func bytes_sha256(p_bytes: PackedByteArray) -> String:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(p_bytes)
	return hashing.finish().hex_encode()


## A Variant as a JSON value. Colors stay as stored (sRGB for material and light colors).
## Resources become their class and, when saved to their own file, their path.
static func json_value(p_value: Variant) -> Variant:
	match typeof(p_value):
		TYPE_NIL, TYPE_BOOL, TYPE_INT, TYPE_STRING:
			return p_value
		TYPE_FLOAT:
			var number := float(p_value)
			if is_nan(number) or is_inf(number):
				return str(number)
			return number
		TYPE_STRING_NAME, TYPE_NODE_PATH:
			return str(p_value)
		TYPE_VECTOR2, TYPE_VECTOR2I:
			return [p_value.x, p_value.y]
		TYPE_VECTOR3, TYPE_VECTOR3I:
			return [p_value.x, p_value.y, p_value.z]
		TYPE_VECTOR4, TYPE_VECTOR4I, TYPE_QUATERNION:
			return [p_value.x, p_value.y, p_value.z, p_value.w]
		TYPE_COLOR:
			return [p_value.r, p_value.g, p_value.b, p_value.a]
		TYPE_AABB:
			return {"position": json_value(p_value.position), "size": json_value(p_value.size)}
		TYPE_BASIS:
			var rows := transform_rows(Transform3D(p_value, Vector3()))
			return [rows[0], rows[1], rows[2], rows[4], rows[5], rows[6], rows[8], rows[9], rows[10]]
		TYPE_TRANSFORM3D:
			return transform_rows(p_value)
		TYPE_ARRAY:
			var items := []
			for item: Variant in p_value:
				items.append(json_value(item))
			return items
		TYPE_DICTIONARY:
			var mapped := {}
			for key: Variant in p_value:
				mapped[str(key)] = json_value(p_value[key])
			return mapped
		TYPE_OBJECT:
			if p_value == null:
				return null
			var object: Object = p_value
			var entry := {"class": object.get_class()}
			if object is Resource:
				var path := (object as Resource).resource_path
				if not path.is_empty() and not path.contains("::"):
					entry["path"] = path
			return entry
		_:
			if typeof(p_value) >= TYPE_PACKED_BYTE_ARRAY:
				return {"packed_size": p_value.size()}
			return str(p_value)


## Content the UE import builds only approximately (#233). The exporter refuses it unless the run
## names the id in --approximate=<id>[,<id>]; scene.json then lists each one with the items it
## touches, so the UE import and the settings record carry it as a known difference.
const APPROXIMATIONS := {
	"diffuse_mode": "A diffuse mode other than Lambert (Godot's Burley default): the UE import builds UE's default-lit diffuse.",
	"shadow_caster_mask": "A light's shadow caster mask that leaves out placed layers: UE lights have no shadow caster mask, so every placed caster shadows the light.",
	"light_projector": "A light projector texture: the UE import builds the light without it. Epic's light functions page (UE 5.9) does not say how a light function maps a texture onto a spot light's cone.",
	"light_cull_mask": "A light whose cull mask lights none of the placed layers: the UE import puts it on its own lighting channel, but Epic's lighting channels page (UE 5.9) says channels do not work with Static-mobility mesh actors, and the import places its meshes Static, so in UE the light lights the placed geometry.",
	"translucent_shadow": "An alpha-blended surface on an instance that casts shadows: Godot's alpha blending casts no shadow, while the UE import keeps the instance's Cast Shadow on, so in UE the surface shadows the lights.",
	"alpha_antialiasing": "Alpha antialiasing (alpha to coverage) on an alpha-scissor material: the UE import builds a Masked material clipped at the threshold, without alpha to coverage.",
	"rigid_skin": "A skinned mesh whose every vertex follows one bone at full weight: the export writes one mesh per bone (the triangles bound to it) with a per-frame transform track, and the UE import places them as rigid moving meshes instead of a skeletal mesh. The poses are the same; UE draws rigid meshes, not a skinned one.",
	"shader_material": "A ShaderMaterial: scene.json carries its code, the code's SHA-256 and its uniform values, and the UE import builds a hand-written UE material only for a shader it knows by that SHA-256 (it refuses any other), so the UE surface is a port by hand, not a conversion.",
	"particles": "GPU or CPU particles: no public UE import path carries a Godot particle system, so the UE import builds none and the UE scene draws no particles.",
}

## Texture filters the format carries (BaseMaterial3D.texture_filter), as scene.json names them.
## The UE import builds nearest and linear without mipmaps, and linear with mipmaps as trilinear
## filtering; the other filters (nearest with mipmaps, anisotropic) are refused.
const CARRIED_FILTERS := {
	BaseMaterial3D.TEXTURE_FILTER_NEAREST: "nearest",
	BaseMaterial3D.TEXTURE_FILTER_LINEAR: "linear",
	BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS: "linear_mipmaps",
}

## Material properties that change what Godot draws and that the UE import does not build:
## property -> [the value the format carries, what the refusal names]. Any other value is refused.
const REFUSED_FLAGS := {
	"depth_draw_mode": [BaseMaterial3D.DEPTH_DRAW_OPAQUE_ONLY, "a depth draw mode other than opaque only"],
	"no_depth_test": [false, "no depth test"],
	"render_priority": [0, "a render priority other than 0"],
	"proximity_fade_enabled": [false, "proximity fade"],
	"distance_fade_mode": [BaseMaterial3D.DISTANCE_FADE_DISABLED, "distance fade"],
	"billboard_mode": [BaseMaterial3D.BILLBOARD_DISABLED, "a billboard mode"],
	"grow": [false, "grow"],
	"disable_receive_shadows": [false, "shadow receiving disabled"],
	"shadow_to_opacity": [false, "shadow to opacity"],
	"disable_ambient_light": [false, "ambient light disabled"],
	"disable_fog": [false, "fog disabled"],
}

## Texture slots the format carries, and what the UE import builds from them.
const CARRIED_TEXTURES := [BaseMaterial3D.TEXTURE_ALBEDO]

const FEATURE_NAMES := {
	BaseMaterial3D.FEATURE_NORMAL_MAPPING: "normal mapping",
	BaseMaterial3D.FEATURE_RIM: "rim",
	BaseMaterial3D.FEATURE_CLEARCOAT: "clearcoat",
	BaseMaterial3D.FEATURE_ANISOTROPY: "anisotropy",
	BaseMaterial3D.FEATURE_AMBIENT_OCCLUSION: "ambient occlusion",
	BaseMaterial3D.FEATURE_HEIGHT_MAPPING: "height mapping",
	BaseMaterial3D.FEATURE_SUBSURFACE_SCATTERING: "subsurface scattering",
	BaseMaterial3D.FEATURE_SUBSURFACE_TRANSMITTANCE: "subsurface transmittance",
	BaseMaterial3D.FEATURE_BACKLIGHT: "backlight",
	BaseMaterial3D.FEATURE_REFRACTION: "refraction",
	BaseMaterial3D.FEATURE_DETAIL: "detail",
	BaseMaterial3D.FEATURE_BENT_NORMAL_MAPPING: "bent normal mapping",
	BaseMaterial3D.FEATURE_PARTICLES_ANIMATION: "particles animation",
}


## How a material is blended, as scene.json "blend": "opaque", "masked" (alpha scissor: the
## UE import builds a Masked material clipped at alpha_scissor_threshold) or "translucent" (alpha
## blending: a Translucent material whose opacity is the albedo alpha).
static func material_blend(p_material: BaseMaterial3D) -> String:
	match p_material.transparency:
		BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR:
			return "masked"
		BaseMaterial3D.TRANSPARENCY_ALPHA:
			return "translucent"
	return "opaque"


## What the format cannot carry in a material yet, as messages (empty when it can). The UE import
## builds default-lit constant materials from scene.json: opaque, masked by the albedo alpha, or
## alpha-blended, with an optional albedo texture. Anything else would be dropped.
static func material_refusals(p_material: BaseMaterial3D) -> PackedStringArray:
	var found := PackedStringArray()
	if p_material.vertex_color_use_as_albedo:
		found.append("vertex colors as albedo")
	if p_material.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_HASH:
		found.append("alpha hash")
	elif p_material.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_DEPTH_PRE_PASS:
		found.append("alpha with a depth pre-pass")
	if p_material.blend_mode != BaseMaterial3D.BLEND_MODE_MIX:
		found.append("a blend mode other than mix")
	if p_material.shading_mode != BaseMaterial3D.SHADING_MODE_PER_PIXEL:
		found.append("a shading mode other than per-pixel")
	if p_material.specular_mode != BaseMaterial3D.SPECULAR_SCHLICK_GGX:
		found.append("a specular mode other than Schlick-GGX")
	if p_material.cull_mode == BaseMaterial3D.CULL_FRONT:
		found.append("front-face culling")
	for property: String in REFUSED_FLAGS:
		if p_material.get(property) != REFUSED_FLAGS[property][0]:
			found.append(String(REFUSED_FLAGS[property][1]))
	for feature: int in FEATURE_NAMES:
		if p_material.get_feature(feature):
			found.append(String(FEATURE_NAMES[feature]))
	var textured := false
	for slot in BaseMaterial3D.TEXTURE_MAX:
		if p_material.get_texture(slot) == null:
			continue
		if slot in CARRIED_TEXTURES:
			textured = true
		else:
			found.append("a texture in slot %d (only the albedo texture is exported)" % slot)
	if textured:
		# UV1 scale and offset are carried (the UE import tiles and offsets the texture coordinate);
		# triplanar mapping is not.
		if p_material.uv1_triplanar:
			found.append("triplanar UV1 mapping")
		if p_material.albedo_texture_msdf:
			found.append("an MSDF albedo texture")
		if not CARRIED_FILTERS.has(p_material.texture_filter):
			found.append("a texture filter with mipmaps other than linear (nearest with mipmaps, or anisotropic)")
		if not p_material.texture_repeat:
			found.append("texture repeat disabled")
	return found


## The APPROXIMATIONS ids a material needs (empty when the UE import builds it as it is).
static func material_approximations(p_material: BaseMaterial3D) -> PackedStringArray:
	var found := PackedStringArray()
	if p_material.diffuse_mode != BaseMaterial3D.DIFFUSE_LAMBERT:
		found.append("diffuse_mode")
	if p_material.transparency == BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR and p_material.alpha_antialiasing_mode != BaseMaterial3D.ALPHA_ANTIALIASING_OFF:
		found.append("alpha_antialiasing")
	return found


## The material's texture filter as scene.json names it ("nearest", "linear", "linear_mipmaps"), or
## an empty string for one the format does not carry.
static func texture_filter_name(p_material: BaseMaterial3D) -> String:
	return String(CARRIED_FILTERS.get(p_material.texture_filter, ""))


## Splits a skinned mesh into rigid parts, one per skin bind, for the rigid_skin approximation.
## The mesh must be an ArrayMesh of triangle-list surfaces without blend shapes (the parts carry
## neither morphs nor another primitive type), every vertex must follow exactly one bind at full
## weight, and every triangle must stay on one bind. Returns {"parts": [{"bind": int, "surfaces": [original surface index, ...],
## "arrays": [surface arrays without bones and weights, ...], "sources": [PackedInt32Array of the
## original vertex index of each part vertex, ...]}], "triangles": int} sorted by bind, or
## {"error": message}. The part vertices stay in the mesh's own space, so a part placed at
## mesh_instance.global_transform * bone_global_pose * bind_pose draws what the skinned mesh draws.
static func rigid_skin_parts(p_mesh: Mesh) -> Dictionary:
	if not (p_mesh is ArrayMesh):
		return {"error": "it is a %s, not an ArrayMesh" % p_mesh.get_class()}
	var array_mesh := p_mesh as ArrayMesh
	if array_mesh.get_blend_shape_count() > 0:
		return {"error": "it has blend shapes, which rigid parts would drop"}
	for surface in array_mesh.get_surface_count():
		if array_mesh.surface_get_primitive_type(surface) != Mesh.PRIMITIVE_TRIANGLES:
			return {"error": "surface %d is not a triangle list" % surface}
	var parts := {}
	var triangles := 0
	for surface in p_mesh.get_surface_count():
		var arrays := p_mesh.surface_get_arrays(surface)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bones: Variant = arrays[Mesh.ARRAY_BONES]
		var weights: Variant = arrays[Mesh.ARRAY_WEIGHTS]
		if bones == null or weights == null:
			return {"error": "surface %d has no bones or weights" % surface}
		var bone_ids := bones as PackedInt32Array
		var bone_weights := weights as PackedFloat32Array
		if vertices.is_empty() or bone_ids.size() != bone_weights.size() or bone_ids.size() % vertices.size() != 0:
			return {"error": "surface %d has a bone array that does not match its vertices" % surface}
		var per_vertex := bone_ids.size() / vertices.size()
		var bind_of := PackedInt32Array()
		bind_of.resize(vertices.size())
		for vertex in vertices.size():
			var found := -1
			for slot in per_vertex:
				var weight := bone_weights[vertex * per_vertex + slot]
				if weight >= 1.0 - 1e-6 and found < 0:
					found = bone_ids[vertex * per_vertex + slot]
				elif absf(weight) > 1e-6:
					return {"error": "vertex %d of surface %d blends more than one bone" % [vertex, surface]}
			if found < 0:
				return {"error": "vertex %d of surface %d follows no bone at full weight" % [vertex, surface]}
			bind_of[vertex] = found
		var indices: Variant = arrays[Mesh.ARRAY_INDEX]
		var corners := PackedInt32Array()
		if indices != null:
			corners = indices as PackedInt32Array
		else:
			for vertex in vertices.size():
				corners.append(vertex)
		# bind -> {"remap": {old: new}, "corners": PackedInt32Array}
		var by_bind := {}
		for triangle in corners.size() / 3:
			var a := corners[triangle * 3]
			var bind := bind_of[a]
			if bind_of[corners[triangle * 3 + 1]] != bind or bind_of[corners[triangle * 3 + 2]] != bind:
				return {"error": "triangle %d of surface %d spans more than one bone" % [triangle, surface]}
			if not by_bind.has(bind):
				by_bind[bind] = {"remap": {}, "corners": PackedInt32Array()}
			var entry: Dictionary = by_bind[bind]
			var part_corners: PackedInt32Array = entry["corners"]
			for corner in 3:
				var old := corners[triangle * 3 + corner]
				var remap: Dictionary = entry["remap"]
				if not remap.has(old):
					remap[old] = remap.size()
				part_corners.append(int(remap[old]))
			entry["corners"] = part_corners
			triangles += 1
		var binds: Array = by_bind.keys()
		binds.sort()
		for bind: int in binds:
			var entry: Dictionary = by_bind[bind]
			var remap: Dictionary = entry["remap"]
			var order := PackedInt32Array()
			order.resize(remap.size())
			for old: int in remap:
				order[int(remap[old])] = old
			var part_arrays := []
			part_arrays.resize(Mesh.ARRAY_MAX)
			for slot in [Mesh.ARRAY_VERTEX, Mesh.ARRAY_NORMAL, Mesh.ARRAY_TANGENT, Mesh.ARRAY_COLOR, Mesh.ARRAY_TEX_UV, Mesh.ARRAY_TEX_UV2]:
				part_arrays[slot] = _gather(arrays[slot], order, vertices.size())
			part_arrays[Mesh.ARRAY_INDEX] = entry["corners"]
			if not parts.has(bind):
				parts[bind] = {"bind": bind, "surfaces": [], "arrays": [], "sources": []}
			(parts[bind]["surfaces"] as Array).append(surface)
			(parts[bind]["arrays"] as Array).append(part_arrays)
			(parts[bind]["sources"] as Array).append(order)
	var sorted: Array = parts.keys()
	sorted.sort()
	var result: Array = []
	for bind: int in sorted:
		result.append(parts[bind])
	return {"parts": result, "triangles": triangles}


## The rigid parts of p_split against the renderer's own skinning of the same pose (p_baked, from
## MeshInstance3D.bake_mesh_from_current_skeleton_pose): every part vertex, moved by its part's
## transform in the mesh instance's space (p_local_parts: bind -> Transform3D), must sit where
## p_baked puts the source vertex. Returns {"max_m": the largest distance in meters, "compared":
## the number of vertices compared}; max_m is INF when a part has no transform or p_baked does not
## hold a source vertex.
static func rigid_skin_error(p_split: Dictionary, p_baked: Mesh, p_local_parts: Dictionary) -> Dictionary:
	if p_baked == null:
		return {"max_m": INF, "compared": 0}
	var worst := 0.0
	var compared := 0
	for part: Dictionary in p_split["parts"]:
		var bind := int(part["bind"])
		if not p_local_parts.has(bind):
			worst = INF
			continue
		var local: Transform3D = p_local_parts[bind]
		var surfaces: Array = part["surfaces"]
		for index in surfaces.size():
			var surface := int(surfaces[index])
			if surface >= p_baked.get_surface_count():
				worst = INF
				continue
			var skinned: PackedVector3Array = p_baked.surface_get_arrays(surface)[Mesh.ARRAY_VERTEX]
			var part_vertices: PackedVector3Array = part["arrays"][index][Mesh.ARRAY_VERTEX]
			var sources: PackedInt32Array = part["sources"][index]
			for vertex in part_vertices.size():
				if sources[vertex] >= skinned.size():
					worst = INF
					continue
				worst = maxf(worst, (local * part_vertices[vertex] - skinned[sources[vertex]]).length())
				compared += 1
	return {"max_m": worst, "compared": compared}


## The elements of a per-vertex mesh array at p_order (components per vertex inferred from the
## array's size), or null for an absent array.
static func _gather(p_array: Variant, p_order: PackedInt32Array, p_vertices: int) -> Variant:
	if p_array == null:
		return null
	match typeof(p_array):
		TYPE_PACKED_VECTOR3_ARRAY:
			var out := PackedVector3Array()
			for index in p_order:
				out.append(p_array[index])
			return out
		TYPE_PACKED_VECTOR2_ARRAY:
			var out := PackedVector2Array()
			for index in p_order:
				out.append(p_array[index])
			return out
		TYPE_PACKED_COLOR_ARRAY:
			var out := PackedColorArray()
			for index in p_order:
				out.append(p_array[index])
			return out
		TYPE_PACKED_FLOAT32_ARRAY:
			var stride := (p_array as PackedFloat32Array).size() / p_vertices
			var out := PackedFloat32Array()
			for index in p_order:
				for component in stride:
					out.append(p_array[index * stride + component])
			return out
	return null


## Whether a surface with this material on an instance with this cast_shadow setting needs the
## translucent_shadow approximation: Godot's alpha blending (TRANSPARENCY_ALPHA) casts no shadow,
## and the UE import keeps the instance's Cast Shadow on.
static func translucent_shadow(p_material: Material, p_cast_shadow: int) -> bool:
	if p_cast_shadow == GeometryInstance3D.SHADOW_CASTING_SETTING_OFF or not (p_material is BaseMaterial3D):
		return false
	return (p_material as BaseMaterial3D).transparency == BaseMaterial3D.TRANSPARENCY_ALPHA


## How a light's layer mask treats the placed geometry: "all" when it keeps every layer the placed
## instances use, "none" when it keeps none of them, "some" otherwise.
static func mask_effect(p_mask: int, p_layers_in_use: int) -> String:
	var kept := p_mask & p_layers_in_use
	if kept == p_layers_in_use:
		return "all"
	if kept == 0:
		return "none"
	return "some"


## A texture's pixels as PNG bytes (RGBA8), or an empty array when it has no image.
static func texture_png(p_texture: Texture2D) -> PackedByteArray:
	if p_texture == null:
		return PackedByteArray()
	var image := p_texture.get_image()
	if image == null or image.is_empty():
		return PackedByteArray()
	image = image.duplicate() as Image
	if image.is_compressed():
		image.decompress()
	if image.has_mipmaps():
		image.clear_mipmaps()
	image.convert(Image.FORMAT_RGBA8)
	return image.save_png_to_buffer()


## What the format cannot carry in the environment or the camera yet, as messages.
static func environment_refusals(p_environment: Environment, p_camera: Camera3D) -> PackedStringArray:
	var found := PackedStringArray()
	if p_camera != null and p_camera.environment != null:
		found.append("an Environment on the camera (only the WorldEnvironment's is exported)")
	if p_environment != null and p_environment.sky != null and p_environment.sky.sky_material is ProceduralSkyMaterial:
		if (p_environment.sky.sky_material as ProceduralSkyMaterial).sky_cover != null:
			found.append("a sky cover texture on the procedural sky")
	return found


## Every stored property of an object (what a .tscn or .tres saves), as JSON values.
static func storage_properties(p_object: Object) -> Dictionary:
	var result := {}
	for property: Dictionary in p_object.get_property_list():
		var usage := int(property["usage"])
		var name := String(property["name"])
		if (usage & PROPERTY_USAGE_STORAGE) == 0 or name == "script" or name.begins_with("metadata/"):
			continue
		if name == "resource_path" or name == "resource_local_to_scene" or name == "resource_scene_unique_id":
			continue
		result[name] = json_value(p_object.get(name))
	return result


## The radiance of a ProceduralSkyMaterial in direction p_dir (Godot world space), as Godot's
## sky shader computes it (scene/resources/3d/sky_material.cpp) without debanding noise.
## p_suns: up to four {direction (toward the light), color (linear), energy, size (radians)}.
static func procedural_sky_radiance(p_sky: ProceduralSkyMaterial, p_suns: Array, p_dir: Vector3) -> Color:
	var top := (p_sky.sky_top_color * p_sky.sky_energy_multiplier).srgb_to_linear()
	var horizon := (p_sky.sky_horizon_color * p_sky.sky_energy_multiplier).srgb_to_linear()
	var bottom := (p_sky.ground_bottom_color * p_sky.ground_energy_multiplier).srgb_to_linear()
	var ground_horizon := (p_sky.ground_horizon_color * p_sky.ground_energy_multiplier).srgb_to_linear()
	var v_angle := clampf(p_dir.y, -1.0, 1.0)
	var sky := top.lerp(horizon, clampf(pow(1.0 - v_angle, 0.6 / p_sky.sky_curve), 0.0, 1.0))
	var sun_angle_max := cos(deg_to_rad(p_sky.sun_angle_max))
	var inv_sun_curve := 1.6 / pow(p_sky.sun_curve, 1.4)
	for index in mini(p_suns.size(), 4):
		var sun: Dictionary = p_suns[index]
		var sun_color: Color = sun["color"]
		var light := Color(sun_color.r, sun_color.g, sun_color.b) * float(sun["energy"])
		var sun_angle := (sun["direction"] as Vector3).dot(p_dir)
		var sun_size := cos(float(sun["size"]))
		if sun_angle > sun_size:
			sky = light
		elif sun_angle > sun_angle_max:
			var c2 := (sun_size - sun_angle) / (sun_size - sun_angle_max)
			sky = sky.lerp(light, clampf(pow(1.0 - c2, inv_sun_curve), 0.0, 1.0))
	var ground := bottom.lerp(ground_horizon, clampf(pow(1.0 + v_angle, 0.6 / p_sky.ground_curve), 0.0, 1.0))
	var color := sky if p_dir.y >= 0.0 else ground
	return Color(color.r, color.g, color.b) * p_sky.energy_multiplier


## The direction of panorama pixel (p_x, p_y), at its center, in Godot's equirectangular
## convention (the one PanoramaSkyMaterial samples): u = atan2(x, -z) / 2pi, v = acos(y) / pi.
static func panorama_direction(p_x: int, p_y: int, p_width: int, p_height: int) -> Vector3:
	var phi := TAU * (float(p_x) + 0.5) / float(p_width)
	var theta := PI * (float(p_y) + 0.5) / float(p_height)
	return Vector3(sin(theta) * sin(phi), cos(theta), -sin(theta) * cos(phi))


## Linear RGB floats, row-major from the top row, of the sky as a panorama.
static func procedural_sky_panorama(p_sky: ProceduralSkyMaterial, p_suns: Array, p_width: int, p_height: int) -> PackedFloat32Array:
	var pixels := PackedFloat32Array()
	pixels.resize(p_width * p_height * 3)
	var cursor := 0
	for y in p_height:
		for x in p_width:
			var color := procedural_sky_radiance(p_sky, p_suns, panorama_direction(x, y, p_width, p_height))
			pixels[cursor] = color.r
			pixels[cursor + 1] = color.g
			pixels[cursor + 2] = color.b
			cursor += 3
	return pixels


## A Radiance .hdr file (RGBE, run-length encoded scanlines written as literal runs).
static func encode_hdr(p_width: int, p_height: int, p_pixels: PackedFloat32Array) -> PackedByteArray:
	assert(p_width >= 8 and p_width < 32768)
	var out := PackedByteArray()
	out.append_array(("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % [p_height, p_width]).to_ascii_buffer())
	var channels: Array[PackedByteArray] = []
	for channel in 4:
		var bytes := PackedByteArray()
		bytes.resize(p_width)
		channels.append(bytes)
	for y in p_height:
		for x in p_width:
			var base := (y * p_width + x) * 3
			var rgbe := _rgbe(p_pixels[base], p_pixels[base + 1], p_pixels[base + 2])
			for channel in 4:
				channels[channel][x] = rgbe[channel]
		out.append_array(PackedByteArray([2, 2, p_width >> 8, p_width & 255]))
		for channel in 4:
			var start := 0
			while start < p_width:
				var count := mini(128, p_width - start)
				out.append(count)
				out.append_array(channels[channel].slice(start, start + count))
				start += count
	return out


## Reads a Radiance .hdr written by encode_hdr (or any run-length encoded one) back to linear
## RGB floats. Returns {"width", "height", "pixels"}, or {"error"}.
static func decode_hdr(p_bytes: PackedByteArray) -> Dictionary:
	var cursor := 0
	var lines: Array[String] = []
	while cursor < p_bytes.size() and lines.size() < 32:
		var end := cursor
		while end < p_bytes.size() and p_bytes[end] != 10:
			end += 1
		lines.append(p_bytes.slice(cursor, end).get_string_from_ascii())
		cursor = end + 1
		if lines.size() > 1 and lines[-1].begins_with("-Y "):
			break
	if lines.is_empty() or lines[0] != "#?RADIANCE" or not lines[-1].begins_with("-Y "):
		return {"error": "not a Radiance HDR header"}
	var size := lines[-1].split(" ")
	if size.size() != 4 or size[2] != "+X":
		return {"error": "unsupported HDR orientation: %s" % lines[-1]}
	var height := int(size[1])
	var width := int(size[3])
	var pixels := PackedFloat32Array()
	pixels.resize(width * height * 3)
	var scan := PackedByteArray()
	scan.resize(width * 4)
	for y in height:
		if cursor + 4 > p_bytes.size() or p_bytes[cursor] != 2 or p_bytes[cursor + 1] != 2:
			return {"error": "scanline %d is not run-length encoded" % y}
		cursor += 4
		for channel in 4:
			var x := 0
			while x < width:
				if cursor >= p_bytes.size():
					return {"error": "truncated HDR data"}
				var count := p_bytes[cursor]
				cursor += 1
				if count > 128:
					count -= 128
					for i in count:
						scan[(x + i) * 4 + channel] = p_bytes[cursor]
					cursor += 1
				else:
					for i in count:
						scan[(x + i) * 4 + channel] = p_bytes[cursor + i]
					cursor += count
				x += count
		for x in width:
			var e := scan[x * 4 + 3]
			var base := (y * width + x) * 3
			if e == 0:
				continue
			var f := pow(2.0, float(e) - 136.0)
			pixels[base] = (float(scan[x * 4]) + 0.5) * f
			pixels[base + 1] = (float(scan[x * 4 + 1]) + 0.5) * f
			pixels[base + 2] = (float(scan[x * 4 + 2]) + 0.5) * f
	return {"width": width, "height": height, "pixels": pixels}


static func _rgbe(p_r: float, p_g: float, p_b: float) -> PackedByteArray:
	var v := maxf(p_r, maxf(p_g, p_b))
	if v < 1e-32:
		return PackedByteArray([0, 0, 0, 0])
	var e := int(floor(log(v) / log(2.0))) + 1
	var m := v / pow(2.0, e)
	if m >= 1.0:
		e += 1
	elif m < 0.5:
		e -= 1
	var scale := 256.0 / pow(2.0, e)
	return PackedByteArray([
		clampi(int(p_r * scale), 0, 255),
		clampi(int(p_g * scale), 0, 255),
		clampi(int(p_b * scale), 0, 255),
		clampi(e + 128, 0, 255),
	])
