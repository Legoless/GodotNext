extends RefCounted
## Format helpers for the UE comparison exporter (#235). ue_export.gd drives them, and
## tests/ue_export/check_ue_export.gd reads an export back with them. The format is described
## in ue_export_schema.md.

const SCHEMA := 1
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


## What the format cannot carry in a material yet, as messages (empty when it can). The UE import
## builds opaque, default-lit constant materials from scene.json, so anything else would be dropped.
static func material_refusals(p_material: BaseMaterial3D) -> PackedStringArray:
	var found := PackedStringArray()
	if p_material.vertex_color_use_as_albedo:
		found.append("vertex colors as albedo")
	if p_material.transparency != BaseMaterial3D.TRANSPARENCY_DISABLED:
		found.append("transparency, alpha scissor or alpha hash")
	if p_material.diffuse_mode != BaseMaterial3D.DIFFUSE_LAMBERT:
		found.append("a diffuse mode other than Lambert")
	if p_material.cull_mode == BaseMaterial3D.CULL_FRONT:
		found.append("front-face culling")
	return found


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
