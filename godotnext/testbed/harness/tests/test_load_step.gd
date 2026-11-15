extends SceneTree

## GT-6 load step logic (#190): the burner switch, the calibration update and its tolerance.
## No GPU and no window.
##
## Usage: <godot> --headless --path godotnext/testbed --script res://harness/tests/test_load_step.gd

const LoadStep = preload("res://tests/gpu_load_step.gd")
const Burner = preload("res://tests/gpu_load_step_burner.gd")

var _failures := 0


func _initialize() -> void:
	_test_switch()
	_test_calibration_sweep()
	_test_tolerance()
	_test_median()
	_test_next_iterations()
	_test_converges_on_a_linear_burner()
	if _failures == 0:
		print("test_load_step: ok")
		quit(0)
	else:
		print("test_load_step: %d failure(s)" % _failures)
		quit(1)


func _process(_delta: float) -> bool:
	return true


func _check(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	print("FAIL: " + message)


func _test_switch() -> void:
	_check(LoadStep.SWITCH_FRAME == 600, "the load step switches at flight frame 600")
	_check(not LoadStep.burner_active(599, LoadStep.SWITCH_FRAME), "burner off before the switch")
	_check(LoadStep.burner_active(600, LoadStep.SWITCH_FRAME), "burner on at the switch frame")
	_check(LoadStep.burner_active(3599, LoadStep.SWITCH_FRAME), "burner stays on to the end of the flight")
	_check(LoadStep.TARGET_FRAME_RATE == 60, "the load step targets 60 Hz")


## Calibration sweeps the 30 frames before the switch back and forth, one frame per step.
func _test_calibration_sweep() -> void:
	var window := LoadStep.CALIBRATION_WINDOW
	_check(window == 30, "calibration sweeps the 30-frame window the GT-6 check compares against")
	_check(LoadStep.calibration_pose(0, 600, window) == 570, "the sweep starts 30 frames before the switch")
	_check(LoadStep.calibration_pose(29, 600, window) == 599, "the sweep reaches the frame before the switch")
	_check(LoadStep.calibration_pose(30, 600, window) == 598, "then turns back")
	_check(LoadStep.calibration_pose(58, 600, window) == 570, "and returns to the start")
	var seen := {}
	var previous := LoadStep.calibration_pose(0, 600, window)
	var steady := true
	for step in range(1, 400):
		var pose := LoadStep.calibration_pose(step, 600, window)
		seen[pose] = true
		steady = steady and absi(pose - previous) == 1 and pose >= 570 and pose < 600
		previous = pose
	_check(steady, "the camera moves one flight frame per step and never cuts or leaves the window")
	_check(seen.size() == window, "every frame of the window is shown")


func _test_tolerance() -> void:
	_check(LoadStep.within(2.0, 2.0, LoadStep.TOLERANCE), "exactly 2x passes")
	_check(LoadStep.within(1.9, 2.0, LoadStep.TOLERANCE), "1.90x is inside 5%")
	_check(LoadStep.within(2.1, 2.0, LoadStep.TOLERANCE), "2.10x is inside 5%")
	_check(not LoadStep.within(1.89, 2.0, LoadStep.TOLERANCE), "1.89x is outside 5%")
	_check(not LoadStep.within(2.11, 2.0, LoadStep.TOLERANCE), "2.11x is outside 5%")
	_check(LoadStep.AIM < LoadStep.TOLERANCE, "calibration aims inside the pass band")


func _test_median() -> void:
	var odd: Array[float] = [3.0, 1.0, 2.0]
	var even: Array[float] = [4.0, 1.0, 3.0, 2.0]
	var empty: Array[float] = []
	_check(is_equal_approx(LoadStep.median(odd), 2.0), "median of an odd count")
	_check(is_equal_approx(LoadStep.median(even), 2.5), "median of an even count")
	_check(LoadStep.median(empty) == 0.0, "median of nothing is 0")
	_check(odd == [3.0, 1.0, 2.0], "median leaves its input alone")


func _test_next_iterations() -> void:
	# 1 ms baseline; 64 iterations added 0.5 ms, so 128 add the wanted 1 ms.
	_check(LoadStep.next_iterations(64, 1.0, 1.5, 2.0) == 128, "linear step to the target")
	# 64 iterations added 4 ms: a quarter of them.
	_check(LoadStep.next_iterations(64, 1.0, 5.0, 2.0) == 16, "linear step down")
	# Under 2% added is noise: grow 8x.
	_check(LoadStep.next_iterations(64, 10.0, 10.1, 2.0) == 512, "unmeasurable step grows 8x")
	# Damped to 8x even when the linear estimate is larger.
	_check(LoadStep.next_iterations(64, 10.0, 10.3, 2.0) == 512, "growth is damped to 8x")
	_check(LoadStep.next_iterations(64, 1.0, 1000.0, 2.0) == 8, "shrink is damped to 1/8")
	_check(LoadStep.next_iterations(1, 1.0, 1000.0, 2.0) == 1, "never below one iteration")
	_check(LoadStep.next_iterations(Burner.MAX_ITERATIONS, 10.0, 10.0, 2.0) == Burner.MAX_ITERATIONS, "capped at the burner maximum")


## A burner whose cost is linear in its iterations, plus a fixed dispatch cost, reaches 2x within
## the round budget.
func _test_converges_on_a_linear_burner() -> void:
	var baseline := 6.4
	var per_iteration := 0.0013
	var dispatch := 0.05
	var iterations := LoadStep.FIRST_ITERATIONS
	var rounds := 0
	var ratio := 0.0
	while rounds < LoadStep.MAX_ROUNDS:
		rounds += 1
		var loaded := baseline + dispatch + per_iteration * float(iterations)
		ratio = loaded / baseline
		if LoadStep.within(ratio, LoadStep.TARGET_RATIO, LoadStep.AIM):
			break
		iterations = LoadStep.next_iterations(iterations, baseline, loaded, LoadStep.TARGET_RATIO)
	_check(LoadStep.within(ratio, LoadStep.TARGET_RATIO, LoadStep.TOLERANCE), "calibration reaches 2x (ratio %.4f after %d rounds)" % [ratio, rounds])
	_check(rounds < LoadStep.MAX_ROUNDS, "calibration converges inside the round budget")
