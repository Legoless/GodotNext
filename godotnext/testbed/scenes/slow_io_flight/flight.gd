extends RefCounted
## Seeded Dense City flight and the Slow IO delay parameter (#74).
##
## The control points are the Dense City `FlightPath` (plan §13.3). The
## camera samples them by frame index, never by wall time, so capture mode
## stays byte-identical. IO delays are a wall-time parameter for the page
## streamer (#78). This scene records the parameter and does not sleep:
## `rendering/nextgeo/streaming/debug_io_delay_ms` is not in the engine yet.
##
## 3600 frames at 60 fps is 60 s. The nodelay variant is the same path and
## is the G5-2 crack and hole flight.


const CONTROL_POINTS: Array[Vector3] = [
	Vector3(-700.0, 22.0, 0.0),
	Vector3(-260.0, 3.0, 0.0),
	Vector3(120.0, 18.0, 8.0),
	Vector3(520.0, 55.0, -30.0),
	Vector3(760.0, 140.0, -260.0),
	Vector3(300.0, 260.0, -620.0),
	Vector3(-320.0, 240.0, -540.0),
	Vector3(-720.0, 90.0, -180.0),
]

const FLIGHT_FRAMES := 3600
const FLIGHT_FPS := 60
const BAKE_INTERVAL := 0.25
const DELAY_MIN_MS := 50
const DELAY_MAX_MS := 250
const DELAY_SETTING := "rendering/nextgeo/streaming/debug_io_delay_ms"
const GEOMETRY_CACHE_SETTING := "rendering/limits/nextgeo/geometry_cache_mb"
const GEOMETRY_CACHE_MB := 128
const DENSE_CITY_SCENE := "res://generated/dense_city/dense_city.tscn"
const DENSE_CITY_GENERATOR := "res://generators/dense_city/dense_city_generator.gd"
const EXPECTED_JSON := "res://generators/dense_city/expected.json"
const PAGE_UPLOAD_METHOD := "virtual_geometry_get_routing_counters"
const MEMORY_METHOD := "virtual_geometry_get_memory_report"

const VARIANTS: Array[String] = ["delayed", "nodelay"]


static func make_curve() -> Curve3D:
	var curve := Curve3D.new()
	curve.bake_interval = BAKE_INTERVAL
	for point in CONTROL_POINTS:
		curve.add_point(point)
	return curve


static func position_at_frame(p_frame: int) -> Vector3:
	var curve := make_curve()
	var length := curve.get_baked_length()
	var frame := clampi(p_frame, 0, FLIGHT_FRAMES - 1)
	if length <= 0.0:
		return CONTROL_POINTS[0]
	var distance := length * float(frame) / float(FLIGHT_FRAMES - 1)
	return curve.sample_baked(distance)


static func camera_transform(p_frame: int) -> Transform3D:
	var position := position_at_frame(p_frame)
	var ahead := position_at_frame(mini(p_frame + 8, FLIGHT_FRAMES - 1))
	var forward := ahead - position
	if forward.length_squared() < 1e-8:
		forward = Vector3(1.0, 0.0, 0.0)
	return Transform3D(Basis.looking_at(forward.normalized(), Vector3.UP), position)


static func delay_record(p_variant: String) -> Dictionary:
	var delayed := p_variant != "nodelay"
	return {
		"distribution": "uniform" if delayed else "fixed",
		"min_ms": DELAY_MIN_MS if delayed else 0,
		"max_ms": DELAY_MAX_MS if delayed else 0,
		"applied_in": "wall_time",
		"setting": DELAY_SETTING,
		"per_request": delayed,
		"gate": false,
	}


static func apply_delay_setting(p_variant: String) -> bool:
	if not ProjectSettings.has_setting(DELAY_SETTING):
		return false
	var record := delay_record(p_variant)
	# One project value cannot hold a per-request range. #78 reads min_ms and
	# max_ms from the run report and sleeps that long per request. When the
	# setting exists early, the delayed variant stores the lower bound so a
	# run is never silent, and the report still carries the full range.
	ProjectSettings.set_setting(DELAY_SETTING, int(record["min_ms"]))
	return true


static func contract_error() -> String:
	if CONTROL_POINTS.size() != 8:
		return "flight path needs the 8 Dense City control points."
	if FLIGHT_FRAMES != FLIGHT_FPS * 60:
		return "flight length must be 60 s at 60 fps."
	if position_at_frame(0).distance_to(CONTROL_POINTS[0]) > 0.05:
		return "frame 0 must start on the first control point."
	var again := position_at_frame(0)
	if again.distance_to(position_at_frame(0)) > 1e-6:
		return "flight sampling is not deterministic."
	var delayed := delay_record("delayed")
	var nodelay := delay_record("nodelay")
	if int(delayed["min_ms"]) != DELAY_MIN_MS or int(delayed["max_ms"]) != DELAY_MAX_MS:
		return "delayed variant must record uniform 50-250 ms."
	if String(delayed["applied_in"]) != "wall_time":
		return "IO delay must be recorded as wall time."
	if int(nodelay["min_ms"]) != 0 or int(nodelay["max_ms"]) != 0:
		return "nodelay variant must record 0 ms."
	if String(nodelay["applied_in"]) != "wall_time":
		return "nodelay path must keep the wall-time field."
	return ""
