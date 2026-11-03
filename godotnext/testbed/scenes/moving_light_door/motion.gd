extends RefCounted
## Frame-indexed motion for Moving Light + Door (#33).
##
## Time is `frame / 60`, never wall-clock time, so a `--fixed-fps 60` capture
## is a pure function of the frame index. `--hold-frame=k` samples that function
## at k, which is the same state as frame k of a moving run.


const FPS := 60.0
const DOOR_SECONDS := 2.0
const DOOR_TRAVEL_M := 1.0
const LEAK_GAP_M := 0.02
const LIGHT_MOTION_FRAMES := 120
const SPLINE_END_FRAME := 240

## Offsets from the lit-room light center. The path runs for
## [constant LIGHT_MOTION_FRAMES] and then holds at the last point, which is
## also when the door reaches 1 m.
const LIGHT_OFFSETS: Array[Vector3] = [
	Vector3(0.0, 0.0, -1.2),
	Vector3(0.9, 0.15, 0.0),
	Vector3(0.0, 0.3, 1.2),
]


static func continuous_gap_m(p_frame: int) -> float:
	var time_s := float(maxi(p_frame, 0)) / FPS
	return clampf(time_s / DOOR_SECONDS, 0.0, 1.0) * DOOR_TRAVEL_M


static func door_gap_m(p_variant: String, p_frame: int) -> float:
	if p_variant == "door_teleport":
		return DOOR_TRAVEL_M if p_frame >= 0 else 0.0
	if p_variant == "light_toggle":
		return 0.0
	return continuous_gap_m(p_frame)


static func first_leak_frame() -> int:
	var frame := 0
	while continuous_gap_m(frame) < LEAK_GAP_M:
		frame += 1
	return frame


static func light_sample_frame(p_variant: String, p_frame: int) -> int:
	if p_variant != "door_open":
		return 0
	return clampi(p_frame, 0, LIGHT_MOTION_FRAMES)


static func light_position(p_variant: String, p_frame: int, p_lit_center: Vector3) -> Vector3:
	var sample := light_sample_frame(p_variant, p_frame)
	var segments := LIGHT_OFFSETS.size() - 1
	var span := float(LIGHT_MOTION_FRAMES) / float(segments)
	if sample >= LIGHT_MOTION_FRAMES:
		return p_lit_center + LIGHT_OFFSETS[segments]
	var t := float(sample) / span
	var index := mini(int(floor(t)), segments - 1)
	var local := t - float(index)
	var start := p_lit_center + LIGHT_OFFSETS[index]
	var finish := p_lit_center + LIGHT_OFFSETS[index + 1]
	return start.lerp(finish, local)


static func light_energy(p_variant: String, p_frame: int) -> float:
	# The toggle is the step at frame 0. Every captured frame is already past
	# it, so the light is on; the pre-step off state is not a rendered frame.
	if p_frame < 0:
		return 0.0
	if p_variant == "light_toggle" or p_variant == "door_open" or p_variant == "door_teleport":
		return 1.0
	return 1.0


static func contract_error() -> String:
	if not is_equal_approx(continuous_gap_m(0), 0.0):
		return "door gap at frame 0 is not 0"
	if not is_equal_approx(continuous_gap_m(60), 0.5):
		return "door gap at frame 60 is not 0.5 m"
	if not is_equal_approx(continuous_gap_m(120), 1.0):
		return "door gap at frame 120 is not 1 m"
	if not is_equal_approx(continuous_gap_m(240), 1.0):
		return "door gap at frame 240 is not held at 1 m"
	if continuous_gap_m(2) >= LEAK_GAP_M:
		return "frame 2 is already at the 2 cm leak gap"
	if continuous_gap_m(3) < LEAK_GAP_M:
		return "frame 3 is under the 2 cm leak gap"
	if first_leak_frame() != 3:
		return "first leak frame is not 3"
	if not is_equal_approx(door_gap_m("door_teleport", 0), 1.0):
		return "door_teleport frame 0 is not 1 m"
	if not is_equal_approx(door_gap_m("door_teleport", 240), 1.0):
		return "door_teleport does not hold"
	if not is_equal_approx(door_gap_m("light_toggle", 120), 0.0):
		return "light_toggle moves the door"
	if light_sample_frame("door_open", 0) != 0:
		return "light does not start at frame 0"
	if light_sample_frame("door_open", 120) != LIGHT_MOTION_FRAMES:
		return "light does not hold when the door finishes"
	if light_sample_frame("door_open", SPLINE_END_FRAME) != LIGHT_MOTION_FRAMES:
		return "light does not stay held through frame 240"
	if light_sample_frame("light_toggle", 60) != 0:
		return "light_toggle moves the light"
	if light_sample_frame("door_teleport", 60) != 0:
		return "door_teleport moves the light"
	return ""
