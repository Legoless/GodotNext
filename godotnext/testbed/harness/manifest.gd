extends RefCounted

## Manifest schema, percentile math, and timing-file writers.
## Static on purpose so tests can load it without a window or a GPU.

const SCHEMA := 1

const DLSS_FIELDS: Array = [
	"dll_path",
	"dll_version",
	"dll_sha256",
	"dll_sha_matches_manifest",
	"preset",
	"allow_ota_updates",
	"nvidia_driver",
	"override_checklist",
	"ngx_log",
]


static func percentile(values: PackedFloat64Array, percent: float) -> float:
	# Linear interpolation, numpy-style. Rank is (percent/100) * (n-1).
	var count := values.size()
	if count <= 0:
		return 0.0
	if count == 1:
		return float(values[0])
	var ordered := values.duplicate()
	ordered.sort()
	var rank := (percent / 100.0) * float(count - 1)
	var low := int(floor(rank))
	var high := int(ceil(rank))
	if low < 0:
		low = 0
	if high > count - 1:
		high = count - 1
	if low == high:
		return float(ordered[low])
	var fraction := rank - float(low)
	return float(ordered[low]) * (1.0 - fraction) + float(ordered[high]) * fraction


static func sanitize_pass_name(raw: String) -> String:
	var lower := raw.to_lower()
	var out := ""
	var pending_break := false
	for index in lower.length():
		var code := lower.unicode_at(index)
		var is_letter := code >= 97 and code <= 122
		var is_digit := code >= 48 and code <= 57
		if is_letter or is_digit:
			if pending_break and not out.is_empty():
				out += "_"
			pending_break = false
			out += char(code)
		else:
			pending_break = true
	if out.is_empty():
		return "pass"
	return out


static func columns_for_passes(names: PackedStringArray) -> PackedStringArray:
	var used := {}
	var columns := PackedStringArray()
	for raw in names:
		var base := sanitize_pass_name(String(raw))
		var column := base + "_ms"
		var suffix := 2
		while used.has(column):
			column = "%s_%d_ms" % [base, suffix]
			suffix += 1
		used[column] = true
		columns.append(column)
	return columns


static func column_stats(values: PackedFloat64Array) -> Dictionary:
	return {
		"p50": percentile(values, 50.0),
		"p95": percentile(values, 95.0),
		"p99": percentile(values, 99.0),
		"n": values.size(),
	}


static func summarize(rows: Array, warmup: int) -> Dictionary:
	var wall := PackedFloat64Array()
	var order: Array = []
	var series := {}
	for row in rows:
		wall.append(float(row["wall_ms"]))
		var seen_in_row := {}
		for sample in row["passes"]:
			var column := String(sample["column"])
			if seen_in_row.has(column):
				continue
			seen_in_row[column] = true
			if not series.has(column):
				series[column] = PackedFloat64Array()
				order.append(column)
			series[column].append(float(sample["ms"]))
	var columns := {"wall_ms": column_stats(wall)}
	for column in order:
		columns[column] = column_stats(series[column])
	return {
		"schema": SCHEMA,
		"frames": rows.size(),
		"warmup": warmup,
		"columns": columns,
	}


static func csv_header(rows: Array) -> PackedStringArray:
	var header := PackedStringArray(["frame", "wall_ms"])
	var seen := {"frame": true, "wall_ms": true}
	for row in rows:
		for sample in row["passes"]:
			var column := String(sample["column"])
			if not seen.has(column):
				seen[column] = true
				header.append(column)
	return header


static func render_csv(rows: Array) -> String:
	var header := csv_header(rows)
	var lines: Array = [",".join(header)]
	for index in rows.size():
		var row: Dictionary = rows[index]
		var by_column := {}
		for sample in row["passes"]:
			by_column[String(sample["column"])] = float(sample["ms"])
		var cells: Array = []
		for column in header:
			if column == "frame":
				cells.append(str(index))
			elif column == "wall_ms":
				cells.append("%.6f" % float(row["wall_ms"]))
			elif by_column.has(column):
				cells.append("%.6f" % float(by_column[column]))
			else:
				cells.append("")
		lines.append(",".join(cells))
	return "\n".join(lines) + "\n"


static func validate(data: Dictionary) -> PackedStringArray:
	var errors := PackedStringArray()
	_require_keys(data, [
		"schema",
		"host",
		"adapter",
		"commit",
		"command_line",
		"seed",
		"mode",
		"scene",
		"camera",
		"frames",
		"warmup",
		"capture_frames",
		"runner",
		"scaling_3d_mode",
		"scaling_3d_scale",
		"output_size",
	], "", errors)
	if not _is_number(data.get("schema", null)) or int(data.get("schema", -1)) != SCHEMA:
		errors.append("schema must be 1")
	var mode := String(data.get("mode", ""))
	if mode != "capture" and mode != "timing":
		errors.append("mode must be capture or timing")
	if not _is_string(data.get("host", null)):
		errors.append("host must be a string")
	if not _is_string(data.get("commit", null)):
		errors.append("commit must be a string")
	if not _is_string(data.get("scene", null)) or String(data.get("scene", "")).is_empty():
		errors.append("scene must be a non-empty string")
	if not _is_string(data.get("camera", null)):
		errors.append("camera must be a string")
	if not _is_number(data.get("seed", null)):
		errors.append("seed must be a number")
	if not _is_number(data.get("frames", null)) or int(data.get("frames", 0)) < 1:
		errors.append("frames must be a positive number")
	if not _is_number(data.get("warmup", null)) or int(data.get("warmup", -1)) < 0:
		errors.append("warmup must be a non-negative number")
	if not _is_number(data.get("scaling_3d_mode", null)):
		errors.append("scaling_3d_mode must be a number")
	if not _is_number(data.get("scaling_3d_scale", null)):
		errors.append("scaling_3d_scale must be a number")
	_validate_output_size(data.get("output_size", null), errors)
	_validate_capture_frames(data.get("capture_frames", null), errors)
	_validate_adapter(data.get("adapter", null), errors)
	_validate_command_line(data.get("command_line", null), errors)
	_validate_runner(data.get("runner", null), errors)
	if data.has("dlss"):
		_validate_dlss(data["dlss"], errors)
	return errors


static func _validate_output_size(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_ARRAY or value.size() != 2:
		errors.append("output_size must be [width, height]")
		return
	if not _is_number(value[0]) or not _is_number(value[1]):
		errors.append("output_size values must be numbers")


static func _validate_capture_frames(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_ARRAY:
		errors.append("capture_frames must be an array")
		return
	for frame in value:
		if not _is_number(frame) or int(frame) < 0:
			errors.append("capture_frames values must be non-negative numbers")
			return


static func _validate_adapter(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_DICTIONARY:
		errors.append("adapter must be an object")
		return
	_require_keys(value, ["name", "api_version"], "adapter", errors)
	if value.has("name") and not _is_string(value["name"]):
		errors.append("adapter.name must be a string")
	if value.has("api_version") and not _is_string(value["api_version"]):
		errors.append("adapter.api_version must be a string")


static func _validate_command_line(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_DICTIONARY:
		errors.append("command_line must be an object")
		return
	_require_keys(value, ["engine", "user"], "command_line", errors)
	if value.has("engine") and typeof(value["engine"]) != TYPE_ARRAY:
		errors.append("command_line.engine must be an array")
	if value.has("user") and typeof(value["user"]) != TYPE_ARRAY:
		errors.append("command_line.user must be an array")


static func _validate_runner(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_DICTIONARY:
		errors.append("runner must be an object")
		return
	_require_keys(value, ["clock", "power"], "runner", errors)
	var clock = value.get("clock", null)
	if typeof(clock) != TYPE_DICTIONARY:
		errors.append("runner.clock must be an object")
	else:
		_require_keys(clock, ["locked", "detail"], "runner.clock", errors)
		if clock.has("locked") and typeof(clock["locked"]) != TYPE_BOOL:
			errors.append("runner.clock.locked must be a bool")
		if clock.has("detail") and not _is_string(clock["detail"]):
			errors.append("runner.clock.detail must be a string")
	var power = value.get("power", null)
	if typeof(power) != TYPE_DICTIONARY:
		errors.append("runner.power must be an object")
		return
	_require_keys(power, ["on_ac", "powermode", "powermode_name", "thermal", "before", "after"], "runner.power", errors)
	if power.has("on_ac") and typeof(power["on_ac"]) != TYPE_BOOL:
		errors.append("runner.power.on_ac must be a bool")
	if power.has("powermode") and not _is_number(power["powermode"]):
		errors.append("runner.power.powermode must be a number")
	if power.has("powermode_name") and not _is_string(power["powermode_name"]):
		errors.append("runner.power.powermode_name must be a string")
	if power.has("thermal") and not _is_string(power["thermal"]):
		errors.append("runner.power.thermal must be a string")
	if power.has("before") and typeof(power["before"]) != TYPE_DICTIONARY:
		errors.append("runner.power.before must be an object")
	if power.has("after") and typeof(power["after"]) != TYPE_DICTIONARY:
		errors.append("runner.power.after must be an object")


static func _validate_dlss(value, errors: PackedStringArray) -> void:
	if typeof(value) != TYPE_DICTIONARY:
		errors.append("dlss must be an object when present")
		return
	for field in DLSS_FIELDS:
		if not value.has(field):
			errors.append("dlss missing %s" % field)
	if value.has("dll_path") and not _is_string(value["dll_path"]):
		errors.append("dlss.dll_path must be a string")
	if value.has("dll_version") and not _is_string(value["dll_version"]):
		errors.append("dlss.dll_version must be a string")
	if value.has("dll_sha256") and not _is_string(value["dll_sha256"]):
		errors.append("dlss.dll_sha256 must be a string")
	if value.has("dll_sha_matches_manifest") and typeof(value["dll_sha_matches_manifest"]) != TYPE_BOOL:
		errors.append("dlss.dll_sha_matches_manifest must be a bool")
	if value.has("preset") and not _is_string(value["preset"]):
		errors.append("dlss.preset must be a string")
	if value.has("allow_ota_updates") and typeof(value["allow_ota_updates"]) != TYPE_BOOL:
		errors.append("dlss.allow_ota_updates must be a bool")
	if value.has("nvidia_driver") and not _is_string(value["nvidia_driver"]):
		errors.append("dlss.nvidia_driver must be a string")
	if value.has("ngx_log") and not _is_string(value["ngx_log"]):
		errors.append("dlss.ngx_log must be a string")
	var checklist = value.get("override_checklist", null)
	if typeof(checklist) != TYPE_DICTIONARY:
		errors.append("dlss.override_checklist must be an object")
		return
	if not checklist.has("screenshot") or not checklist.has("state"):
		errors.append("dlss.override_checklist needs screenshot and state")
		return
	if not _is_string(checklist["screenshot"]) or not _is_string(checklist["state"]):
		errors.append("dlss.override_checklist screenshot and state must be strings")


static func _require_keys(data: Dictionary, keys: Array, prefix: String, errors: PackedStringArray) -> void:
	for key in keys:
		if data.has(key):
			continue
		if prefix.is_empty():
			errors.append("missing %s" % key)
		else:
			errors.append("missing %s.%s" % [prefix, key])


static func _is_number(value) -> bool:
	var kind := typeof(value)
	return kind == TYPE_INT or kind == TYPE_FLOAT


static func _is_string(value) -> bool:
	return typeof(value) == TYPE_STRING
