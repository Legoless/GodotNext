extends RefCounted
## User arguments shared by the dynamic testbed scenes (#33).
##
## The harness stays idle unless `--mode` is present, so these flags are read
## from the raw user arguments here. Animation is frame-indexed: `--hold-frame`
## freezes that index, and a driving harness publishes the index the capture
## or timing window is on.


static func value(p_flag: String, p_default: String = "") -> String:
	var args := OS.get_cmdline_user_args()
	var index := 0
	while index < args.size():
		var token := String(args[index])
		if token == p_flag:
			if index + 1 < args.size() and not String(args[index + 1]).begins_with("--"):
				return String(args[index + 1])
			return p_default
		var prefix := p_flag + "="
		if token.begins_with(prefix):
			return token.substr(prefix.length())
		index += 1
	return p_default


static func int_value(p_flag: String, p_default: int) -> int:
	var text := value(p_flag, "")
	if text.is_empty() or not text.is_valid_int():
		return p_default
	return int(text)


## One animation step. The first call (saw = false) stays on [param p_local_frame].
## Later calls advance it, unless the frame is held or the harness is driving.
static func step_frame(p_node: Node, p_local_frame: int, p_saw_process: bool, p_hold_frame: int) -> Dictionary:
	var harness := p_node.get_node_or_null("/root/Harness")
	var driving := harness != null and harness.has_method("is_driving_animation") and bool(harness.call("is_driving_animation"))
	var local := p_local_frame
	if p_saw_process and p_hold_frame < 0 and not driving:
		local += 1
	var frame := local
	if p_hold_frame >= 0:
		frame = p_hold_frame
	elif driving:
		frame = int(harness.call("get_animation_frame"))
	return {"frame": frame, "local_frame": local, "saw": true}
