extends SceneTree

## #205: the RR hit distance rule (hit_distance_rule.gd): at least the minimum number of
## samples, |median ratio - 1| <= 1%, p05 >= 0.85 and p95 <= 1.15. No GPU and no window. Any error
## the engine logs during the tests fails them.
##
## Usage: <godot> --headless --path godotnext/testbed --script res://tests/rr_guides/test_hit_distance_rule.gd

const HitDistanceRule = preload("res://tests/rr_guides/hit_distance_rule.gd")

## The minimum the runner asks for (--min-hit-samples=64).
const MIN_SAMPLES := 64


## Counts the errors the engine logs while the tests run. A script error does not always stop the
## test function that raised it, and never counts as a failed check, so any error fails the test.
class ErrorCounter extends Logger:
	var _count := 0
	var _mutex := Mutex.new()

	func _log_error(_function: String, _file: String, _line: int, _code: String, _rationale: String, _editor_notify: bool, p_error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		if p_error_type == Logger.ERROR_TYPE_WARNING:
			return
		# Called from any thread.
		_mutex.lock()
		_count += 1
		_mutex.unlock()

	func count() -> int:
		_mutex.lock()
		var result := _count
		_mutex.unlock()
		return result


var _failures := 0


func _initialize() -> void:
	var errors := ErrorCounter.new()
	OS.add_logger(errors)
	_test_limits()
	_test_passes()
	_test_sample_count()
	_test_median()
	_test_p05()
	_test_p95()
	_test_bounds_inclusive()
	_test_every_failure_reported()
	_test_no_samples()
	OS.remove_logger(errors)
	var error_count := errors.count()
	if error_count > 0:
		_failures += 1
		print("FAIL: %d error(s) logged while the tests ran" % error_count)
	if _failures == 0:
		print("test_hit_distance_rule: ok")
		quit(0)
	else:
		print("test_hit_distance_rule: %d failure(s)" % _failures)
		quit(1)


func _process(_delta: float) -> bool:
	return true


func _check(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	print("FAIL: " + message)


## p_count ratios evenly spaced from p_low to p_high.
func _spread(p_count: int, p_low: float, p_high: float) -> PackedFloat64Array:
	var ratios := PackedFloat64Array()
	for i in p_count:
		ratios.append(p_low + (p_high - p_low) * float(i) / float(p_count - 1))
	return ratios


## 100 ratios: 10 at p_low, 80 at p_middle and 10 at p_high, shuffled so the rule has to sort them.
func _tails(p_low: float, p_middle: float, p_high: float) -> PackedFloat64Array:
	var values := []
	for i in 10:
		values.append(p_low)
		values.append(p_high)
	for i in 80:
		values.append(p_middle)
	var ratios := PackedFloat64Array()
	for i in values.size():
		# A fixed permutation of 0..99 (37 and 100 are coprime).
		ratios.append(values[(i * 37) % values.size()])
	return ratios


## 101 ratios, shuffled: p_low at sorted indices 0-5, p_middle at 6-94 and p_high at 95-100. With
## 101 ratios the 5th, 50th and 95th percentiles fall exactly on indices 5, 50 and 95, so p05, the
## median and p95 are exactly p_low, p_middle and p_high, with no interpolation.
func _exact(p_low: float, p_middle: float, p_high: float) -> PackedFloat64Array:
	var values := []
	for i in 6:
		values.append(p_low)
		values.append(p_high)
	for i in 89:
		values.append(p_middle)
	var ratios := PackedFloat64Array()
	for i in values.size():
		# A fixed permutation of 0..100 (101 is prime).
		ratios.append(values[(i * 37) % values.size()])
	return ratios


func _failed(p_verdict: Dictionary) -> PackedStringArray:
	return p_verdict["failed"]


func _test_limits() -> void:
	_check(HitDistanceRule.MEDIAN_TOLERANCE == 0.01, "the median may be 1% from 1")
	_check(HitDistanceRule.P05_MIN == 0.85, "p05 must be at least 0.85")
	_check(HitDistanceRule.P95_MAX == 1.15, "p95 must be at most 1.15")


func _test_passes() -> void:
	# 100 ratios from 0.90 to 1.10: p05 0.91, median 1.0, p95 1.09.
	var verdict := HitDistanceRule.judge(_spread(100, 0.90, 1.10), MIN_SAMPLES)
	_check(bool(verdict["pass"]) and _failed(verdict).is_empty(), "a spread inside the bounds passes")
	_check(int(verdict["samples"]) == 100, "the verdict reports the sample count")
	_check(is_equal_approx(float(verdict["p05"]), 0.91), "p05 interpolates between ranks (0.91, got %s)" % str(verdict["p05"]))
	_check(is_equal_approx(float(verdict["median"]), 1.0), "the median of an even count is the mean of the middle two")
	_check(is_equal_approx(float(verdict["p95"]), 1.09), "p95 interpolates between ranks (1.09, got %s)" % str(verdict["p95"]))
	# No per-sample rule: 90 of these 100 ratios are more than 1% from 1.
	var off := 0
	for ratio in _spread(100, 0.90, 1.10):
		off += 1 if absf(ratio - 1.0) > 0.01 else 0
	_check(off == 90, "the passing spread has 90 samples more than 1%% from 1 (got %d)" % off)
	# Just inside each bound at once.
	var edge := HitDistanceRule.judge(_tails(0.851, 1.009, 1.149), MIN_SAMPLES)
	_check(bool(edge["pass"]), "median 1.009, p05 0.851 and p95 1.149 pass (failed %s)" % str(edge["failed"]))


func _test_sample_count() -> void:
	var ones := PackedFloat64Array()
	ones.resize(MIN_SAMPLES - 1)
	ones.fill(1.0)
	var short := HitDistanceRule.judge(ones, MIN_SAMPLES)
	_check(not bool(short["pass"]) and _failed(short) == PackedStringArray(["samples"]), "63 perfect samples fail on the sample count alone (failed %s)" % str(short["failed"]))
	_check(int(short["samples"]) == MIN_SAMPLES - 1 and is_equal_approx(float(short["median"]), 1.0), "a short run still reports its count and percentiles")
	ones.append(1.0)
	_check(bool(HitDistanceRule.judge(ones, MIN_SAMPLES)["pass"]), "64 perfect samples pass")


func _test_median() -> void:
	var high := HitDistanceRule.judge(_tails(0.95, 1.011, 1.05), MIN_SAMPLES)
	_check(not bool(high["pass"]) and _failed(high) == PackedStringArray(["median"]), "median 1.011 fails on the median alone (failed %s)" % str(high["failed"]))
	var low := HitDistanceRule.judge(_tails(0.95, 0.989, 1.05), MIN_SAMPLES)
	_check(not bool(low["pass"]) and _failed(low) == PackedStringArray(["median"]), "median 0.989 fails on the median alone (failed %s)" % str(low["failed"]))
	_check(bool(HitDistanceRule.judge(_tails(0.95, 1.009, 1.05), MIN_SAMPLES)["pass"]), "median 1.009 passes")
	_check(bool(HitDistanceRule.judge(_tails(0.95, 0.991, 1.05), MIN_SAMPLES)["pass"]), "median 0.991 passes")
	# The median is a percentile of the samples, not their mean: a long tail on one side leaves
	# it at 1.
	var tail := HitDistanceRule.judge(_tails(0.86, 1.0, 1.0), MIN_SAMPLES)
	_check(bool(tail["pass"]) and is_equal_approx(float(tail["median"]), 1.0), "a low tail does not move the median")


func _test_p05() -> void:
	var verdict := HitDistanceRule.judge(_tails(0.849, 1.0, 1.05), MIN_SAMPLES)
	_check(not bool(verdict["pass"]) and _failed(verdict) == PackedStringArray(["p05"]), "p05 0.849 fails on p05 alone (failed %s)" % str(verdict["failed"]))
	_check(is_equal_approx(float(verdict["p05"]), 0.849), "p05 is the low tail (got %s)" % str(verdict["p05"]))
	_check(bool(HitDistanceRule.judge(_tails(0.851, 1.0, 1.05), MIN_SAMPLES)["pass"]), "p05 0.851 passes")


func _test_p95() -> void:
	var verdict := HitDistanceRule.judge(_tails(0.95, 1.0, 1.151), MIN_SAMPLES)
	_check(not bool(verdict["pass"]) and _failed(verdict) == PackedStringArray(["p95"]), "p95 1.151 fails on p95 alone (failed %s)" % str(verdict["failed"]))
	_check(is_equal_approx(float(verdict["p95"]), 1.151), "p95 is the high tail (got %s)" % str(verdict["p95"]))
	_check(bool(HitDistanceRule.judge(_tails(0.95, 1.0, 1.149), MIN_SAMPLES)["pass"]), "p95 1.149 passes")


func _test_bounds_inclusive() -> void:
	# Every bound is inclusive: |median - 1| <= 1%, p05 >= 0.85 and p95 <= 1.15.
	for median: float in [1.01, 0.99]:
		var verdict := HitDistanceRule.judge(_exact(0.85, median, 1.15), MIN_SAMPLES)
		_check(float(verdict["p05"]) == 0.85 and float(verdict["median"]) == median and float(verdict["p95"]) == 1.15, "the percentiles sit exactly on the bounds (got %s / %s / %s)" % [str(verdict["p05"]), str(verdict["median"]), str(verdict["p95"])])
		_check(bool(verdict["pass"]), "p05 0.85, median %s and p95 1.15 pass (failed %s)" % [str(median), str(verdict["failed"])])


func _test_every_failure_reported() -> void:
	# 20 ratios from 0.5 to 1.6: too few, median 1.05, p05 0.555, p95 1.545.
	var verdict := HitDistanceRule.judge(_spread(20, 0.5, 1.6), MIN_SAMPLES)
	_check(_failed(verdict) == PackedStringArray(["samples", "median", "p05", "p95"]), "every failed rule is reported (failed %s)" % str(verdict["failed"]))
	var messages: PackedStringArray = verdict["failures"]
	_check(messages.size() == 4, "one message per failed rule")


func _test_no_samples() -> void:
	var none := HitDistanceRule.judge(PackedFloat64Array(), MIN_SAMPLES)
	_check(not bool(none["pass"]) and _failed(none) == PackedStringArray(["samples"]) and not none.has("median"), "no samples fail on the count and report no percentiles")
	# Without a minimum (a scene pass that does not ask for the hit distance) nothing fails.
	_check(bool(HitDistanceRule.judge(PackedFloat64Array(), 0)["pass"]), "no samples and no minimum pass")
