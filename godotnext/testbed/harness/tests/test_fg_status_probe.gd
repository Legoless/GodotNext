extends SceneTree

## Checks the GU+-7 status probe's enum lookup (#202) on enums the engine has today, since
## FrameGenerationReason arrives with #203. No GPU and no window.
##
## Usage: <godot> --headless --path godotnext/testbed --script res://harness/tests/test_fg_status_probe.gd

const Probe = preload("res://harness/fg_status_probe.gd")

var _failures := 0


func _initialize() -> void:
	_test_enum_on_rendering_server()
	_test_enum_on_another_class()
	_test_missing_enum()
	if _failures == 0:
		print("test_fg_status_probe: ok")
		quit(0)
	else:
		print("test_fg_status_probe: %d failure(s)" % _failures)
		quit(1)


func _check(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	print("FAIL: " + message)


## RenderingServer, the first class searched.
func _test_enum_on_rendering_server() -> void:
	var value := ClassDB.class_get_integer_constant(&"RenderingServer", &"VIEWPORT_MSAA_4X")
	var found := Probe.enum_constant_name(&"ViewportMSAA", value)
	_check(found == "VIEWPORT_MSAA_4X", "ViewportMSAA %d is %s, expected VIEWPORT_MSAA_4X" % [value, found])


## An enum that only another class declares, as FrameGenerationReason may be on
## FrameGenerationExtension (#201).
func _test_enum_on_another_class() -> void:
	var owners: Array[String] = []
	for class_label in ClassDB.get_class_list():
		if ClassDB.class_has_enum(class_label, &"AnchorMode", true):
			owners.append(class_label)
	_check(owners == ["Camera2D"], "AnchorMode is declared by %s, the test expects Camera2D alone" % str(owners))
	var value := ClassDB.class_get_integer_constant(&"Camera2D", &"ANCHOR_MODE_DRAG_CENTER")
	var found := Probe.enum_constant_name(&"AnchorMode", value)
	_check(found == "ANCHOR_MODE_DRAG_CENTER", "AnchorMode %d is %s, expected ANCHOR_MODE_DRAG_CENTER" % [value, found])


func _test_missing_enum() -> void:
	_check(Probe.enum_constant_name(&"NoSuchEnumForTheProbe", 0) == "", "an unknown enum gives a name")
	_check(Probe.enum_constant_name(&"ViewportMSAA", 1000) == "", "an unknown value gives a name")
