extends Node
## Frame-generation status probe for GU+-7 (#202, plan §21.5.3).
##
## Shows the static-UI scene (the PU+ phase smoke scene) for [constant FRAMES] frames, then prints
## [method RenderingServer.get_frame_generation_status] as one line,
## [code]GODOTNEXT_FG_STATUS {json}[/code], and quits. The JSON carries [code]available[/code],
## the returned [code]status[/code] and, when the status has an integer [code]reason[/code], its
## constant name from ClassDB ([code]reason_name[/code]), so the checker does not copy the enum.
## #203 names [code]RenderingServer.get_frame_generation_status()[/code] and #201 puts
## [code]FrameGenerationReason[/code] in [code]frame_generation_extension.h[/code], so the enum is
## looked up on RenderingServer, then FrameGenerationExtension, then every other class.
## Until #203 adds the method, [code]available[/code] is false and
## [code]godotnext_metrics.fallback_log_check[/code] fails the run. The configuration (FG provider,
## multiplier) comes from an override.cfg written for the launch.

const SCENE := "res://scenes/static_ui/static_ui.tscn"
const FRAMES := 120
const STATUS_METHOD := &"get_frame_generation_status"
const REASON_ENUM := &"FrameGenerationReason"
## The classes searched first for [constant REASON_ENUM], in order.
const REASON_CLASSES: Array[StringName] = [&"RenderingServer", &"FrameGenerationExtension"]

var _frames := 0


func _ready() -> void:
	add_child((load(SCENE) as PackedScene).instantiate())


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < FRAMES:
		return
	set_process(false)
	var line := {"available": RenderingServer.has_method(STATUS_METHOD)}
	if bool(line["available"]):
		var status: Variant = RenderingServer.call(STATUS_METHOD)
		line["status"] = status
		if status is Dictionary:
			var reason: Variant = (status as Dictionary).get("reason")
			if reason is int:
				line["reason_name"] = reason_name(reason)
			elif reason is String or reason is StringName:
				line["reason_name"] = String(reason)
	print("GODOTNEXT_FG_STATUS " + JSON.stringify(line))
	get_tree().quit(0)


## The [constant REASON_ENUM] constant whose value is [param value], or "".
static func reason_name(value: int) -> String:
	return enum_constant_name(REASON_ENUM, value)


## The name of the constant of [param enum_name] whose value is [param value], from the classes that
## declare that enum: [constant REASON_CLASSES] first, then every class in ClassDB. "" when none has it.
static func enum_constant_name(enum_name: StringName, value: int) -> String:
	var classes: Array[StringName] = []
	classes.append_array(REASON_CLASSES)
	for class_label in ClassDB.get_class_list():
		classes.append(StringName(class_label))
	for owner_class in classes:
		if not ClassDB.class_exists(owner_class) or not ClassDB.class_has_enum(owner_class, enum_name, true):
			continue
		for constant in ClassDB.class_get_enum_constants(owner_class, enum_name, true):
			if ClassDB.class_get_integer_constant(owner_class, constant) == value:
				return constant
	return ""
