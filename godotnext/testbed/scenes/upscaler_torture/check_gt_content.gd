extends SceneTree
## GT-2 content check for Upscaler Torture (#190). No GPU: it projects the generated scene from the
## static camera at 1440p internal (2560x1440) and checks
## - the 12 cables: 6 against the sky above the backdrop and 6 against the checker backdrop, each
##   0.3-1.0 px wide along its visible length;
## - the two moiré grilles: a projected bar period from 1 to 4 px, receding at a grazing angle;
## - the mask layers: only the grilles are on the moiré layer and only the cables on the thin-wire one.
##
## Usage: <godot> --headless --path godotnext/testbed --script res://scenes/upscaler_torture/check_gt_content.gd

const Generator := preload("res://generators/upscaler_torture.gd")

const SAMPLES := 241
const SIZE := Generator.GT_INTERNAL_SIZE

var _failures: PackedStringArray = []
var _report := {}


func _initialize() -> void:
	var root := (load(Generator.SCENE_PATH) as PackedScene).instantiate() as Node3D
	root.set_meta("skip_runtime", true)
	_check_camera(root)
	_check_cables(root)
	_check_grilles(root)
	_check_layers(root)
	root.free()
	_report["failures"] = _failures
	print("GODOTNEXT_RESULT " + JSON.stringify({"test": "upscaler_torture_gt_content", "pass": _failures.is_empty(), "report": _report}))
	quit(0 if _failures.is_empty() else 1)


func _fail(message: String) -> void:
	_failures.append(message)
	printerr("check_gt_content FAIL: " + message)


func _visible(screen: Vector3) -> bool:
	return screen.z > 0.0 and screen.x >= 0.0 and screen.x <= float(SIZE.x) and screen.y >= 0.0 and screen.y <= float(SIZE.y)


func _check_camera(root: Node3D) -> void:
	var path := root.get_node("CameraPath") as Path3D
	var camera := root.get_node("CameraPath/CameraFollow/Camera3D") as Camera3D
	if not path.curve.get_point_position(0).is_equal_approx(Generator.STATIC_CAMERA_POSITION):
		_fail("the camera spline does not start at the static camera position")
	if not is_equal_approx(camera.fov, Generator.CAMERA_FOV):
		_fail("the camera field of view is not %s" % Generator.CAMERA_FOV)
	if (camera.cull_mask & Generator.MOIRE_LAYER) == 0 or (camera.cull_mask & Generator.THIN_WIRE_LAYER) == 0:
		_fail("the camera does not see the GT-2 layers")


## The backdrop's screen rectangle from the static camera: left, top, right, bottom.
func _backdrop_rect(root: Node3D) -> Rect2:
	var backdrop := root.get_node("CheckerBackdrop") as MeshInstance3D
	var half := (backdrop.mesh as QuadMesh).size * 0.5
	var a := Generator.project_static(backdrop.position + Vector3(-half.x, -half.y, 0.0), SIZE)
	var b := Generator.project_static(backdrop.position + Vector3(half.x, half.y, 0.0), SIZE)
	return Rect2(Vector2(a.x, b.y), Vector2(b.x - a.x, a.y - b.y))


func _check_cables(root: Node3D) -> void:
	var cables := root.get_node("ThinCables").get_children()
	if cables.size() != 12:
		_fail("expected 12 cables, found %d" % cables.size())
	var backdrop := _backdrop_rect(root)
	var counts := {"sky": 0, "geometry": 0}
	var rows: Array = []
	var overall := Vector2(INF, -INF)
	for node in cables:
		var cable := node as MeshInstance3D
		var against := String(cable.get_meta("against", ""))
		counts[against] = int(counts.get(against, 0)) + 1
		var cylinder := cable.mesh as CylinderMesh
		var axis := cable.transform.basis.y.normalized()
		var half := cylinder.height * 0.5
		var widths := Vector2(INF, -INF)
		var span := Vector2(INF, -INF)
		var wrong_background := 0
		for index in SAMPLES:
			var point := cable.position + axis * lerpf(-half, half, float(index) / float(SAMPLES - 1))
			var screen := Generator.project_static(point, SIZE)
			if not _visible(screen):
				continue
			var width := Generator.projected_width_px(point, axis, cylinder.top_radius * 2.0, SIZE)
			widths = Vector2(minf(widths.x, width), maxf(widths.y, width))
			span = Vector2(minf(span.x, screen.x), maxf(span.y, screen.x))
			if against == "sky" and screen.y >= backdrop.position.y:
				wrong_background += 1
			elif against == "geometry" and not backdrop.has_point(Vector2(screen.x, screen.y)):
				wrong_background += 1
		var row := {"name": String(cable.name), "against": against, "width_px": [widths.x, widths.y], "screen_x": [span.x, span.y]}
		rows.append(row)
		if widths.x == INF:
			_fail("%s is not visible from the static camera" % cable.name)
			continue
		overall = Vector2(minf(overall.x, widths.x), maxf(overall.y, widths.y))
		if widths.x < Generator.CABLE_WIDTH_PX.x or widths.y > Generator.CABLE_WIDTH_PX.y:
			_fail("%s is %.3f-%.3f px wide, outside %.1f-%.1f px" % [cable.name, widths.x, widths.y, Generator.CABLE_WIDTH_PX.x, Generator.CABLE_WIDTH_PX.y])
		if span.y - span.x < 1000.0:
			_fail("%s spans only %.0f px of the frame" % [cable.name, span.y - span.x])
		if wrong_background > 0:
			_fail("%s has %d samples off its %s background" % [cable.name, wrong_background, against])
		if (cable.layers & Generator.THIN_WIRE_LAYER) == 0:
			_fail("%s is not on the thin-wire layer" % cable.name)
	if int(counts.get("sky", 0)) != 6 or int(counts.get("geometry", 0)) != 6:
		_fail("expected 6 cables against the sky and 6 against geometry, found %s" % str(counts))
	_report["cables"] = rows
	_report["cable_width_px"] = [overall.x, overall.y]


func _check_grilles(root: Node3D) -> void:
	var rows: Array = []
	for spec in Generator.GRILLES:
		var grille := root.get_node(String(spec["name"])) as Node3D
		var x := float(spec["x"])
		var period := float((grille.get_node("Bars") as MeshInstance3D).get_meta("period_m", 0.0))
		var periods := Vector2(INF, -INF)
		var angles := Vector2(INF, -INF)
		for y in [Generator.GRILLE_Y.x, (Generator.GRILLE_Y.x + Generator.GRILLE_Y.y) * 0.5, Generator.GRILLE_Y.y]:
			for index in Generator.grille_bar_count() - 1:
				var z := Generator.GRILLE_Z_NEAR - float(index) * period
				var a := Generator.project_static(Vector3(x, y, z), SIZE)
				var b := Generator.project_static(Vector3(x, y, z - period), SIZE)
				if not _visible(a) or not _visible(b):
					continue
				# The bars project close to vertical, so the period runs across the screen.
				var pixels := absf(a.x - b.x)
				periods = Vector2(minf(periods.x, pixels), maxf(periods.y, pixels))
				# Angle between the view ray and the grille's plane (x = const).
				var ray := (Vector3(x, y, z) - Generator.STATIC_CAMERA_POSITION).normalized()
				var angle := rad_to_deg(asin(absf(ray.x)))
				angles = Vector2(minf(angles.x, angle), maxf(angles.y, angle))
		rows.append({"name": String(spec["name"]), "period_px": [periods.x, periods.y], "view_angle_deg": [angles.x, angles.y]})
		if periods.x == INF:
			_fail("%s is not visible from the static camera" % spec["name"])
			continue
		if periods.x < Generator.GRILLE_PERIOD_PX.x or periods.y > Generator.GRILLE_PERIOD_PX.y:
			_fail("%s period is %.3f-%.3f px, outside 1-4 px" % [spec["name"], periods.x, periods.y])
		# The grille has to sweep most of the band, not sit at one end of it.
		if periods.x > 1.25 or periods.y < 3.5:
			_fail("%s period %.3f-%.3f px does not span 1-4 px" % [spec["name"], periods.x, periods.y])
		if angles.y > 45.0:
			_fail("%s is seen at up to %.1f degrees, not at a grazing angle" % [spec["name"], angles.y])
	_report["grilles"] = rows


func _check_layers(root: Node3D) -> void:
	for node in root.find_children("*", "VisualInstance3D", true, false):
		var instance := node as VisualInstance3D
		var in_grille := String(instance.get_parent().name).begins_with("MoireGrille")
		var in_cables := String(instance.get_parent().name) == "ThinCables"
		if ((instance.layers & Generator.MOIRE_LAYER) != 0) != in_grille:
			_fail("%s/%s: moiré layer is %s" % [instance.get_parent().name, instance.name, "set" if not in_grille else "missing"])
		if ((instance.layers & Generator.THIN_WIRE_LAYER) != 0) != in_cables:
			_fail("%s/%s: thin-wire layer is %s" % [instance.get_parent().name, instance.name, "set" if not in_cables else "missing"])
