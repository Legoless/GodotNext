extends SceneTree
## Double-precision CPU mirror of the furnace estimator (#19).
##
## Cosine-sampled Lambert, Russian roulette from bounce 4, hard stop at 64.
## One million paths. Also checks the no-ray-query fallback payload and that
## the sealed-room soups block every dark patch's view of the omni light.
##
## godot --headless --path godotnext/testbed --script res://tests/reference/furnace_cpu.gd

const Soups = preload("res://tests/reference/soups.gd")
const Reference = preload("res://tests/reference/reference_pt.gd")

const PATHS := 1000000
const SEED := 1
const EXPECTED := 5.0
const RELATIVE_TOLERANCE := 0.002
const ALBEDO := 0.8
const EMISSION := 1.0
const BOUNCE_CAP := 64
const ROULETTE_FROM := 4
const ORIGIN_OFFSET := 1e-4

var _hit_position := Vector3.ZERO
var _hit_normal := Vector3.ZERO


func _initialize() -> void:
	var failures := PackedStringArray()
	_check_fallback(failures)
	_check_soups(failures)
	if not failures.is_empty():
		_finish(failures, 0.0, 0.0)
		return
	var started := Time.get_ticks_msec()
	var sum := 0.0
	var sum_sq := 0.0
	for path in PATHS:
		var sample := _estimate(path)
		sum += sample
		sum_sq += sample * sample
		if path > 0 and path % 100000 == 0:
			print("furnace_cpu path %d mean %.6f" % [path, sum / float(path)])
	var mean := sum / float(PATHS)
	var variance := sum_sq / float(PATHS) - mean * mean
	var sigma := sqrt(maxf(variance, 0.0))
	if absf(mean - EXPECTED) / EXPECTED > RELATIVE_TOLERANCE:
		failures.append("mean %.6f is outside 0.2 percent of %.1f" % [mean, EXPECTED])
	print("furnace_cpu paths %d mean %.6f sigma %.4f relative_sigma %.4f ms %d" % [PATHS, mean, sigma, sigma / mean, Time.get_ticks_msec() - started])
	_finish(failures, mean, sigma)


func _check_fallback(failures: PackedStringArray) -> void:
	var result: Dictionary = Reference.fallback_result("furnace")
	if not bool(result["pass"]) or String(result["fallback"]) != "NO_RAY_QUERY":
		failures.append("fallback result is not a passing NO_RAY_QUERY payload")


func _check_soups(failures: PackedStringArray) -> void:
	failures.append_array(Soups.validate_furnace(Soups.build_furnace()))
	failures.append_array(Soups.validate_sealed(Soups.build_sealed(0.10)))
	failures.append_array(Soups.validate_sealed(Soups.build_sealed(0.20)))


func _estimate(path: int) -> float:
	var weight := 1.0
	var radiance := 0.0
	var origin := Vector3.ZERO
	var direction := Vector3(0.0, 0.0, -1.0)
	for bounce in BOUNCE_CAP:
		if bounce >= ROULETTE_FROM:
			var survival := minf(weight, 0.95)
			if survival <= 0.0 or _rand(path, bounce, 0) > survival:
				break
			weight /= survival
		if not _trace_box(origin, direction):
			break
		radiance += weight * EMISSION
		var u1 := _rand(path, bounce, 1)
		var u2 := _rand(path, bounce, 2)
		var phi := TAU * u1
		var cos_theta := sqrt(1.0 - u2)
		var sin_theta := sqrt(u2)
		var helper := Vector3(0.0, 0.0, 1.0) if absf(_hit_normal.z) < 0.999 else Vector3(1.0, 0.0, 0.0)
		var tangent := helper.cross(_hit_normal).normalized()
		var bitangent := _hit_normal.cross(tangent)
		direction = (tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + _hit_normal * cos_theta).normalized()
		var pdf := cos_theta / PI
		var brdf_nl := cos_theta / PI
		weight *= ALBEDO * brdf_nl / pdf
		origin = _hit_position + _hit_normal * ORIGIN_OFFSET
	return radiance


func _trace_box(origin: Vector3, direction: Vector3) -> bool:
	var best_t := 1.0e30
	var best_normal := Vector3.ZERO
	var found := false
	for axis in 3:
		var component := direction[axis]
		if component == 0.0:
			continue
		var plane := 1.0 if component > 0.0 else -1.0
		var t := (plane - origin[axis]) / component
		if t > 1.0e-8 and t < best_t:
			best_t = t
			found = true
			best_normal = Vector3.ZERO
			best_normal[axis] = -plane
	if not found:
		return false
	_hit_position = origin + direction * best_t
	_hit_normal = best_normal
	return true


func _rand(path: int, bounce: int, dimension: int) -> float:
	var hashed := _pcg(path)
	hashed = _pcg(hashed ^ 0)
	hashed = _pcg(hashed ^ bounce)
	hashed = _pcg(hashed ^ dimension)
	hashed = _pcg(hashed ^ SEED)
	return float(hashed & 0xFFFFFF) / 16777216.0


func _pcg(value: int) -> int:
	var state := (value * 747796405 + 2891336453) & 0xFFFFFFFF
	var word := (((state >> ((state >> 28) + 4)) ^ state) * 277803737) & 0xFFFFFFFF
	return ((word >> 22) ^ word) & 0xFFFFFFFF


func _finish(failures: PackedStringArray, mean: float, sigma: float) -> void:
	var result := {
		"pass": failures.is_empty(),
		"mean": mean,
		"sigma": sigma,
		"paths": PATHS,
		"expected": EXPECTED,
		"failures": failures,
	}
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
