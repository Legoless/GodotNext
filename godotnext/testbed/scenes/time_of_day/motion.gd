extends RefCounted
## Frame-indexed sun motion for Time of Day (#33).
##
## `sweep` adds 90° of elevation over 10 s (frame 600 at 60 fps). `sun_jump`
## adds exactly 15° at frame 0 and holds. Azimuth stays 180° so the light
## travels toward +Z, into the interior opening.


const FPS := 60.0
const BASE_ELEVATION_DEG := 10.0
const SWEEP_DEG := 90.0
const SWEEP_SECONDS := 10.0
const SWEEP_END_FRAME := 600
const JUMP_DEG := 15.0
const AZIMUTH_DEG := 180.0


static func elevation_deg(p_variant: String, p_frame: int) -> float:
	if p_variant == "sun_jump":
		return BASE_ELEVATION_DEG + JUMP_DEG
	var time_s := float(maxi(p_frame, 0)) / FPS
	var u := clampf(time_s / SWEEP_SECONDS, 0.0, 1.0)
	return BASE_ELEVATION_DEG + u * SWEEP_DEG


static func jump_delta_deg() -> float:
	return JUMP_DEG


static func sun_direction(p_elevation_deg: float) -> Vector3:
	var elevation := deg_to_rad(p_elevation_deg)
	var azimuth := deg_to_rad(AZIMUTH_DEG)
	return Vector3(
			-cos(elevation) * sin(azimuth),
			-sin(elevation),
			-cos(elevation) * cos(azimuth))


static func sun_basis(p_elevation_deg: float) -> Basis:
	var forward := sun_direction(p_elevation_deg).normalized()
	# Straight down is parallel to +Y, which looking_at cannot use as up.
	var auxiliary := Vector3.FORWARD if absf(forward.dot(Vector3.UP)) > 0.99 else Vector3.UP
	return Basis.looking_at(forward, auxiliary)


static func contract_error() -> String:
	var jump := elevation_deg("sun_jump", 0) - BASE_ELEVATION_DEG
	if not is_equal_approx(jump, JUMP_DEG):
		return "sun_jump is not exactly 15 deg"
	if not is_equal_approx(elevation_deg("sun_jump", 240), elevation_deg("sun_jump", 0)):
		return "sun_jump does not hold through frame 240"
	if not is_equal_approx(elevation_deg("sweep", 0), BASE_ELEVATION_DEG):
		return "sweep frame 0 is not the base elevation"
	if not is_equal_approx(elevation_deg("sweep", 300), BASE_ELEVATION_DEG + 0.5 * SWEEP_DEG):
		return "sweep frame 300 is not halfway"
	if not is_equal_approx(elevation_deg("sweep", SWEEP_END_FRAME), BASE_ELEVATION_DEG + SWEEP_DEG):
		return "sweep frame 600 is not +90 deg"
	for elevation in [0.0, 10.0, 90.0, 100.0]:
		var basis := sun_basis(elevation)
		if not basis.x.is_finite() or not basis.y.is_finite() or not basis.z.is_finite():
			return "sun basis is not finite at elevation %s" % elevation
	return ""
