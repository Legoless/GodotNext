extends Node

## Render-pass clears with unused framebuffer slots (#275; plan §21.1).
##
## On a local RenderingDevice, builds framebuffers whose attachment lists have unused slots
## (null RIDs) before, between and after color and depth attachments, opens and closes one draw
## list on each, and reads every attachment back. Each attachment that clears must hold its own
## clear value in every texel of the draw list's region; one that loads must keep the contents it
## was created with, and so must every color texel outside the region.
##
## RenderingDevice numbers its clear flags and clear colors by the color attachments that exist:
## the n-th color attachment of the list, unused slots skipped, takes DRAW_CLEAR_COLOR_0 << n and
## clear color n. Every attachment gets a value no other attachment of its framebuffer has, and
## none is the (0, 0, 0, 1) an unused slot's clear value holds, so a value that lands on another
## attachment shows. Depth clears to a non-zero value for the same reason.
##
## Shapes ("-" is an unused slot, "c" clears, "l" loads, "d" is depth):
## - control       c c d          no unused slot
## - gap           c - c d        an unused slot before a cleared color attachment
## - two_gaps      c - - c c d    the color pass of #205 without separate specular or velocity
## - leading_gap   - c - c d      the first slot unused
## - gap_depth     c - d          an unused slot right before depth
## - depth_between c d - c        depth, then an unused slot, then color
## - mixed_loads   c - l c - l d  attachments that load between ones that clear
## - region        c - c l        the gap shape with a draw list region (Metal clears a region
##                                with a draw, not with its load action)
## Formats: RGBA8 unorm, RGBA16F, RGBA32F, R32F and D32F, with values each one stores exactly.
##
## Prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on failure.

const WIDTH := 64
const HEIGHT := 32
const REGION := Rect2i(8, 4, 32, 16)

# One slot: "" is unused; otherwise "<format>:<clear|load>".
const SHAPES := {
	"control": ["rgba8:clear", "rgba16f:clear", "d32:clear"],
	"gap": ["rgba8:clear", "", "rgba16f:clear", "d32:clear"],
	"two_gaps": ["rgba16f:clear", "", "", "rgba8:clear", "rgba8:clear", "d32:clear"],
	"leading_gap": ["", "rgba32f:clear", "", "r32f:clear", "d32:clear"],
	"gap_depth": ["rgba8:clear", "", "d32:clear"],
	"depth_between": ["rgba16f:clear", "d32:clear", "", "rgba32f:clear"],
	"mixed_loads": ["rgba8:clear", "", "rgba16f:load", "rgba32f:clear", "", "rgba8:load", "d32:clear"],
	"region": ["rgba8:clear", "", "rgba16f:clear", "rgba8:load"],
}
const REGION_SHAPES := ["region"]

const FORMATS := {
	"rgba8": RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM,
	"rgba16f": RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT,
	"rgba32f": RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT,
	"r32f": RenderingDevice.DATA_FORMAT_R32_SFLOAT,
	"d32": RenderingDevice.DATA_FORMAT_D32_SFLOAT,
}
const TEXEL_BYTES := { "rgba8": 4, "rgba16f": 8, "rgba32f": 16, "r32f": 4, "d32": 4 }
# The contents a color attachment is created with: no clear value in this file has them.
const PREFILL_UNORM := Color(0.0, 1.0, 0.0, 1.0)
const PREFILL_FLOAT := Color(-1.0, 3.0, -5.0, 7.0)

var _failures := PackedStringArray()


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var result := {
		"test": "attachment_clears",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"size": [WIDTH, HEIGHT],
		"region": [REGION.position.x, REGION.position.y, REGION.size.x, REGION.size.y],
	}
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result
	result["adapter"] = rd.get_device_name()

	var shapes := {}
	for shape_name: String in SHAPES:
		shapes[shape_name] = _run_shape(rd, shape_name, SHAPES[shape_name], REGION_SHAPES.has(shape_name))
	rd.free()

	result["shapes"] = shapes
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


func _run_shape(rd: RenderingDevice, shape_name: String, slots: Array, use_region: bool) -> Dictionary:
	var textures: Array[RID] = []
	var attachments: Array[Dictionary] = []
	var clear_colors := PackedColorArray()
	var flags := 0
	var depth_value := 0.0
	var color_index := 0
	var created := true
	for slot_index in slots.size():
		var slot: String = slots[slot_index]
		if slot.is_empty():
			textures.append(RID())
			continue
		var parts := slot.split(":")
		var format_name := parts[0]
		var clears := parts[1] == "clear"
		var attachment := { "slot": slot_index, "format": format_name, "op": parts[1] }
		var texture := _create_attachment(rd, format_name, clears)
		textures.append(texture)
		if not texture.is_valid():
			_check(false, "%s: the %s attachment in slot %d is created" % [shape_name, format_name, slot_index])
			created = false
			continue
		if format_name == "d32":
			# One depth attachment per shape; its value differs from the shape's other depth uses.
			depth_value = 0.25 + 0.125 * float(slot_index)
			attachment["clear"] = depth_value
			if clears:
				flags |= RenderingDevice.DRAW_CLEAR_DEPTH
		else:
			var color := _clear_color(format_name, color_index)
			clear_colors.append(color)
			attachment["clear"] = [color.r, color.g, color.b, color.a]
			if clears:
				flags |= RenderingDevice.DRAW_CLEAR_COLOR_0 << color_index
			color_index += 1
		attachment["texture"] = texture
		attachments.append(attachment)

	var shape_result := { "slots": slots, "attachments": [] }
	var framebuffer := RID()
	if created:
		framebuffer = rd.framebuffer_create(textures)
		_check(framebuffer.is_valid(), "%s: the framebuffer is created" % shape_name)
	if framebuffer.is_valid():
		var region := Rect2(REGION) if use_region else Rect2()
		var list := rd.draw_list_begin(framebuffer, flags, clear_colors, depth_value, 0, region)
		_check(list != RenderingDevice.INVALID_ID, "%s: the draw list begins" % shape_name)
		if list != RenderingDevice.INVALID_ID:
			rd.draw_list_end()
		rd.submit()
		rd.sync()
		var shape_pass := true
		for attachment in attachments:
			var outcome := _compare(rd, shape_name, attachment, use_region)
			shape_pass = shape_pass and outcome["pass"]
			shape_result["attachments"].append(outcome)
		shape_result["pass"] = shape_pass
		rd.free_rid(framebuffer)
	else:
		shape_result["pass"] = false
	for texture in textures:
		if texture.is_valid():
			rd.free_rid(texture)
	return shape_result


func _create_attachment(rd: RenderingDevice, format_name: String, clears: bool) -> RID:
	var format := RDTextureFormat.new()
	format.format = FORMATS[format_name]
	format.width = WIDTH
	format.height = HEIGHT
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	if format_name == "d32":
		format.usage_bits |= RenderingDevice.TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
	else:
		format.usage_bits |= RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT
	if not rd.texture_is_format_supported_for_usage(format.format, format.usage_bits):
		_check(false, "%s supports attachment and copy usage" % format_name)
		return RID()
	# Depth attachments cannot be created with data; the region shape has none. A color
	# attachment that clears is created with the prefill too, so the texels outside a region
	# have known contents.
	if format_name == "d32":
		return rd.texture_create(format, RDTextureView.new())
	var texel := _encode(format_name, PREFILL_UNORM if format_name == "rgba8" else PREFILL_FLOAT)
	var data := PackedByteArray()
	for i in WIDTH * HEIGHT:
		data.append_array(texel)
	return rd.texture_create(format, RDTextureView.new(), [data])


# A clear color the format stores exactly, different for every color index and never
# (0, 0, 0, 1).
static func _clear_color(format_name: String, index: int) -> Color:
	if format_name == "rgba8":
		# Multiples of 0.2 are multiples of 51 / 255.
		return Color(float((index + 1) % 6) / 5.0, float((index + 2) % 6) / 5.0, float((index + 3) % 6) / 5.0, float((index + 4) % 6) / 5.0)
	return Color(0.25 * float(index + 1), -0.5 * float(index + 1), 1.5 + float(index), 8.0 - float(index))


static func _encode(format_name: String, color: Color) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(TEXEL_BYTES[format_name])
	match format_name:
		"rgba8":
			bytes[0] = roundi(color.r * 255.0)
			bytes[1] = roundi(color.g * 255.0)
			bytes[2] = roundi(color.b * 255.0)
			bytes[3] = roundi(color.a * 255.0)
		"rgba16f":
			bytes.encode_half(0, color.r)
			bytes.encode_half(2, color.g)
			bytes.encode_half(4, color.b)
			bytes.encode_half(6, color.a)
		"rgba32f":
			bytes.encode_float(0, color.r)
			bytes.encode_float(4, color.g)
			bytes.encode_float(8, color.b)
			bytes.encode_float(12, color.a)
		"r32f", "d32":
			bytes.encode_float(0, color.r)
	return bytes


# What a texel reads as, for the result.
static func _decode(format_name: String, bytes: PackedByteArray) -> Array:
	match format_name:
		"rgba8":
			return [bytes[0], bytes[1], bytes[2], bytes[3]]
		"rgba16f":
			return [bytes.decode_half(0), bytes.decode_half(2), bytes.decode_half(4), bytes.decode_half(6)]
		"rgba32f":
			return [bytes.decode_float(0), bytes.decode_float(4), bytes.decode_float(8), bytes.decode_float(12)]
	return [bytes.decode_float(0)]


func _compare(rd: RenderingDevice, shape_name: String, attachment: Dictionary, use_region: bool) -> Dictionary:
	var format_name: String = attachment["format"]
	var clears: bool = attachment["op"] == "clear"
	var texel_bytes: int = TEXEL_BYTES[format_name]
	var clear_texel := PackedByteArray()
	var prefill_texel := PackedByteArray()
	if format_name == "d32":
		clear_texel = _encode(format_name, Color(float(attachment["clear"]), 0.0, 0.0, 0.0))
	else:
		var clear: Array = attachment["clear"]
		clear_texel = _encode(format_name, Color(clear[0], clear[1], clear[2], clear[3]))
		prefill_texel = _encode(format_name, PREFILL_UNORM if format_name == "rgba8" else PREFILL_FLOAT)
	var outcome := { "slot": attachment["slot"], "format": format_name, "op": attachment["op"], "expected": _decode(format_name, clear_texel if clears else prefill_texel) }

	var data := rd.texture_get_data(attachment["texture"], 0)
	if data.size() != WIDTH * HEIGHT * texel_bytes:
		_check(false, "%s: slot %d (%s) read back %d bytes, expected %d" % [shape_name, attachment["slot"], format_name, data.size(), WIDTH * HEIGHT * texel_bytes])
		outcome["pass"] = false
		return outcome
	var wrong := 0
	var checked := 0
	var first_wrong := -1
	for y in HEIGHT:
		for x in WIDTH:
			var inside := not use_region or REGION.has_point(Vector2i(x, y))
			var expected := clear_texel if (clears and inside) else prefill_texel
			if expected.is_empty():
				# Depth outside the region holds nothing known.
				continue
			checked += 1
			var offset := (y * WIDTH + x) * texel_bytes
			if data.slice(offset, offset + texel_bytes) != expected:
				wrong += 1
				if first_wrong < 0:
					first_wrong = offset
	outcome["texels_checked"] = checked
	outcome["texels_wrong"] = wrong
	if wrong > 0:
		var got := _decode(format_name, data.slice(first_wrong, first_wrong + texel_bytes))
		outcome["first_wrong"] = got
		_check(false, "%s: slot %d (%s, %s) has %d of %d texels wrong, first %s, expected %s" % [shape_name, attachment["slot"], format_name, attachment["op"], wrong, checked, str(got), str(outcome["expected"])])
	outcome["pass"] = wrong == 0
	return outcome


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
