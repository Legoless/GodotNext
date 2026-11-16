extends RefCounted

## #205: the rule the RR hit distance guide is judged by (lead decision of 2026-11-16).
##
## Each compared sample gives one ratio: the guide's mean hit distance over the frames divided by
## the reference path tracer's mean hit distance at that pixel. The guide passes when at least
## the minimum number of samples is compared (the runner asks for 64), the median ratio is within
## MEDIAN_TOLERANCE of 1, the 5th percentile is at least P05_MIN and the 95th percentile at most
## P95_MAX. No single sample has to be within 1%. Percentiles interpolate linearly between ranks
## (harness/manifest.gd). Static, so test_hit_distance_rule.gd loads it without a window or a GPU.

const ManifestScript = preload("res://harness/manifest.gd")

const MEDIAN_TOLERANCE := 0.01
const P05_MIN := 0.85
const P95_MAX := 1.15


## Judges the ratios. Returns {"pass", "samples", "min_samples", and with at least one ratio "p05",
## "median", "p95"; "failed": the rules that failed ("samples", "median", "p05", "p95"),
## "failures": one message per failed rule}.
static func judge(p_ratios: PackedFloat64Array, p_min_samples: int) -> Dictionary:
	var verdict := {"samples": p_ratios.size(), "min_samples": p_min_samples}
	var failed := PackedStringArray()
	var failures := PackedStringArray()
	if p_ratios.size() < p_min_samples:
		failed.append("samples")
		failures.append("%d samples to compare the hit distance on, expected at least %d" % [p_ratios.size(), p_min_samples])
	if not p_ratios.is_empty():
		var p05 := ManifestScript.percentile(p_ratios, 5.0)
		var median := ManifestScript.percentile(p_ratios, 50.0)
		var p95 := ManifestScript.percentile(p_ratios, 95.0)
		verdict["p05"] = p05
		verdict["median"] = median
		verdict["p95"] = p95
		# Against 1 +/- MEDIAN_TOLERANCE, not absf(median - 1.0): in float64 |1.01 - 1| is just
		# above 0.01, so a median exactly on the bound would fail.
		if median > 1.0 + MEDIAN_TOLERANCE or median < 1.0 - MEDIAN_TOLERANCE:
			failed.append("median")
			failures.append("the median hit distance ratio to the reference path tracer is %.4f, more than %s from 1 (%d samples)" % [median, str(MEDIAN_TOLERANCE), p_ratios.size()])
		if p05 < P05_MIN:
			failed.append("p05")
			failures.append("the 5th percentile of the hit distance ratio to the reference path tracer is %.4f, below %s (%d samples)" % [p05, str(P05_MIN), p_ratios.size()])
		if p95 > P95_MAX:
			failed.append("p95")
			failures.append("the 95th percentile of the hit distance ratio to the reference path tracer is %.4f, above %s (%d samples)" % [p95, str(P95_MAX), p_ratios.size()])
	verdict["failed"] = failed
	verdict["failures"] = failures
	verdict["pass"] = failed.is_empty()
	return verdict
