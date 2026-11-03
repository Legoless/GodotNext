extends RefCounted
## Expected routes and reasons for the Exclusion Matrix (#74).
##
## This is the content contract. Issue #79 fills the runtime fallback debug
## view; until `RenderingServer.virtual_geometry_get_debug_stats` exists, the
## checker reports `counters_source: absent` and still requires every cell
## below to be present once (0 double-drawn, 0 missing).
##
## Reason strings match plan §6.3 / §7.4. `alpha` is the importer's
## NEXTGEO_FALLBACK_ALPHA spelling. `VGD_IGNORED` is the §6.3 counted warning
## for octant GridMap cells (there is no separate Fallback enum value).
## `COARSE_OR_RT_PROXY` is the §7.3 auxiliary-view policy for
## GPUParticlesCollisionHeightField3D, not a Fallback enum value.


const ROUTE_VIRTUAL := "virtual"
const ROUTE_LEGACY := "legacy"
const ROUTE_AUXILIARY := "auxiliary_proxy"

const REASON_NONE := "NONE"
const REASON_ALPHA := "alpha"
const REASON_NEXT_PASS := "NEXT_PASS_OR_OVERLAY"
const REASON_VERTEX := "VERTEX_LIGHTING"
const REASON_CUSTOM := "UNSUPPORTED_CUSTOM_SHADER"
const REASON_DERIVATIVE := "UNSUPPORTED_DERIVATIVE"
const REASON_SKINNING := "SKINNING"
const REASON_INSTANCE := "INSTANCE_UNIFORMS"
const REASON_MULTIMESH := "MULTIMESH_OR_PARTICLES"
const REASON_VGD := "VGD_IGNORED"
const REASON_BAKED := "BAKED_GRIDMAP"
const REASON_PROXY := "COARSE_OR_RT_PROXY"
const REASON_MUTATED := "MUTATED_MESH"
const REASON_MSAA := "MSAA"
const REASON_MULTIVIEW := "MULTIVIEW"

## RendererViewport draws a use_xr SubViewport only when XRServer has a
## primary interface. This machine has none, so the multiview cell does not
## rasterize. The SubViewport stays in the matrix with use_xr set and
## render_target_update_mode UPDATE_ALWAYS. See renderer_viewport.cpp.
const MULTIVIEW_DRAW_LIMIT := "No primary XR interface. Viewport._check_xr_size sets view_count to 0 and an unallocated size when XRServer has no initialized primary interface, and the stored size clamps to 2x2. RendererViewport then skips the draw. The cell stays in the matrix with use_xr and UPDATE_ALWAYS."

const MUTATE_FRAME := 60
const MUTATED_BY_FRAME := 61

const MOVING_FRAMES := 121
const MOVING_FOV_DEG := 50.0
const MOVING_ASPECT := 16.0 / 9.0
const MOVING_CAMERA_POS := Vector3(0.0, 0.8, 2.2)
const MOVING_LOOK_AT := Vector3(0.0, 0.4, 0.0)
const DLSS_QUALITY_SCALE := 0.667

const DEBUG_STATS_METHOD := "virtual_geometry_get_debug_stats"


static func cells() -> Array:
	return [
		_cell("mixed_opaque_glass", "Opaque + glass", "mesh", [
			_surface(0, "opaque", ROUTE_VIRTUAL, REASON_NONE),
			_surface(1, "glass", ROUTE_LEGACY, REASON_ALPHA),
		]),
		_cell("next_pass", "next_pass", "mesh", [
			_surface(0, "chain", ROUTE_LEGACY, REASON_NEXT_PASS, {"whole_chain": true}),
		]),
		_cell("material_overlay", "material_overlay", "mesh", [
			_surface(0, "base", ROUTE_LEGACY, REASON_NEXT_PASS, {"whole_chain": true}),
		]),
		_cell("stencil_reader", "Stencil reader", "mesh", [
			_surface(0, "reader", ROUTE_LEGACY, REASON_NEXT_PASS, {"whole_chain": true}),
		]),
		_cell("vertex_lighting", "Vertex lighting", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_VERTEX),
		]),
		_cell("position_vertex", "Position-altering vertex()", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_CUSTOM),
		]),
		_cell("screen_texture", "Screen texture", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_ALPHA),
		]),
		_cell("alpha_scissor", "Alpha scissor", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_ALPHA),
		]),
		_cell("alpha_hash", "Alpha hash", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_ALPHA),
		]),
		_cell("unsupported_derivative", "Non-varying dFdx", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_DERIVATIVE),
		]),
		_cell("skinning", "Skinning", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_SKINNING),
		]),
		_cell("instance_uniforms", "Instance uniforms", "mesh", [
			_surface(0, "surface", ROUTE_LEGACY, REASON_INSTANCE),
		]),
		_cell("multimesh", "MultiMesh", "multimesh", [
			_surface(0, "instances", ROUTE_LEGACY, REASON_MULTIMESH, {"counted_warning": true}),
		]),
		_cell("gridmap_octant", "Octant GridMap", "gridmap", [
			_surface(0, "octant", ROUTE_LEGACY, REASON_VGD, {"counted_warning": true}),
		]),
		_cell("particles", "Particles", "particles", [
			_surface(0, "draw", ROUTE_LEGACY, REASON_MULTIMESH, {"counted_warning": true}),
		]),
		_cell("baked_gridmap", "Baked GridMap", "baked_gridmap", [
			_surface(0, "baked", ROUTE_LEGACY, REASON_BAKED),
		]),
		_cell("heightfield", "Heightfield capture", "heightfield", [
			_surface(0, "capture", ROUTE_AUXILIARY, REASON_PROXY),
		]),
		_cell("animated_time", "Animated TIME", "mesh", [
			_surface(0, "surface", ROUTE_VIRTUAL, REASON_NONE, {"src_static_cache": "excluded"}),
		]),
		_cell("mutated_mesh", "Mutated mesh", "mesh", [
			_surface(0, "surface", ROUTE_VIRTUAL, REASON_NONE, {
				"mutates_at_frame": MUTATE_FRAME,
				"reason_by_frame": MUTATED_BY_FRAME,
				"route_after": ROUTE_LEGACY,
				"reason_after": REASON_MUTATED,
			}),
		]),
		_cell("msaa_viewport", "MSAA viewport", "msaa", [
			_surface(0, "view", ROUTE_LEGACY, REASON_MSAA, {"whole_view": true}),
		]),
		_cell("multiview_viewport", "Multiview viewport", "multiview", [
			_surface(0, "view", ROUTE_LEGACY, REASON_MULTIVIEW, {"whole_view": true}),
		], {"draw_limit": MULTIVIEW_DRAW_LIMIT}),
		_cell("moving_instance", "Moving virtual instance", "moving", [
			_surface(0, "surface", ROUTE_VIRTUAL, REASON_NONE, {"g57": true}),
		]),
	]


static func cell_by_id(p_id: String) -> Dictionary:
	for cell_value in cells():
		var cell: Dictionary = cell_value
		if String(cell["id"]) == p_id:
			return cell
	return {}


static func mutated_expectation(p_frame: int) -> Dictionary:
	if p_frame >= MUTATED_BY_FRAME:
		return {"route": ROUTE_LEGACY, "reason": REASON_MUTATED}
	return {"route": ROUTE_VIRTUAL, "reason": REASON_NONE}


static func moving_offset(p_frame: int) -> Vector3:
	# The box center rests at 0.25 m. Only X moves, on a 4-second sine.
	var t := float(p_frame) / 60.0
	return Vector3(sin(t * TAU / 4.0) * 0.35, 0.25, 0.0)


static func project_uv(p_point: Vector3) -> Vector2:
	var direction := (MOVING_LOOK_AT - MOVING_CAMERA_POS).normalized()
	var xform := Transform3D(Basis.looking_at(direction, Vector3.UP), MOVING_CAMERA_POS)
	var local := xform.affine_inverse() * p_point
	var depth := -local.z
	var half_height := depth * tan(deg_to_rad(MOVING_FOV_DEG) * 0.5)
	var half_width := half_height * MOVING_ASPECT
	var x_ndc := local.x / half_width
	var y_ndc := local.y / half_height
	return Vector2((x_ndc + 1.0) * 0.5, (1.0 - y_ndc) * 0.5)


static func track_sample(p_frame: int) -> Dictionary:
	var current := moving_offset(p_frame)
	var previous := moving_offset(maxi(p_frame - 1, 0))
	var current_uv := project_uv(current)
	var previous_uv := project_uv(previous)
	return {
		"frame": p_frame,
		"position": [current.x, current.y, current.z],
		"previous_position": [previous.x, previous.y, previous.z],
		"uv": [current_uv.x, current_uv.y],
		"previous_uv": [previous_uv.x, previous_uv.y],
	}


static func track_samples() -> Array:
	var rows: Array = []
	for frame in MOVING_FRAMES:
		rows.append(track_sample(frame))
	return rows


## DLSS is not a viewport scaling mode in this build. Other builds report N/A
## and fall back to FSR2. The scene references no NVIDIA file.
static func dlss_status() -> Dictionary:
	var available := false
	for constant in ClassDB.class_get_enum_constants(&"Viewport", &"Scaling3DMode"):
		if String(constant).contains("DLSS"):
			available = true
			break
	return {
		"requested": "DLSS Quality",
		"scale": DLSS_QUALITY_SCALE,
		"available": available,
		"status": "ready" if available else "N/A",
		"fallback": "" if available else "FSR2",
		"dll_sha256": "",
		"preset": "",
		"tracks": ["TAA", "FSR2", "MetalFX", "DLSS Quality"],
	}


static func contract_error() -> String:
	var seen := {}
	var rows := cells()
	if rows.size() < 22:
		return "exclusion matrix needs every §13.3 case, found %d." % rows.size()
	for cell_value in rows:
		var cell: Dictionary = cell_value
		var id := String(cell["id"])
		if seen.has(id):
			return "duplicate exclusion cell %s." % id
		seen[id] = true
		var surfaces: Array = cell["surfaces"]
		if surfaces.is_empty():
			return "cell %s has no surfaces." % id
	if cell_by_id("moving_instance").is_empty():
		return "missing moving_instance cell."
	var multiview := cell_by_id("multiview_viewport")
	if String(multiview.get("draw_limit", "")) != MULTIVIEW_DRAW_LIMIT:
		return "multiview cell must record why it does not rasterize without an XR interface."
	var frame_60 := mutated_expectation(MUTATE_FRAME)
	var frame_61 := mutated_expectation(MUTATED_BY_FRAME)
	if String(frame_60["reason"]) != REASON_NONE or String(frame_61["reason"]) != REASON_MUTATED:
		return "mutated mesh reason must switch at frame 61."
	var first := track_sample(0)
	var second := track_sample(1)
	if first["previous_uv"] != first["uv"]:
		return "frame 0 previous UV must equal the current UV."
	if second["previous_position"] != first["position"]:
		return "frame 1 previous position must be frame 0."
	return ""


static func _cell(p_id: String, p_label: String, p_kind: String, p_surfaces: Array, p_extra: Dictionary = {}) -> Dictionary:
	var row := {"id": p_id, "label": p_label, "kind": p_kind, "surfaces": p_surfaces}
	for key in p_extra:
		row[key] = p_extra[key]
	return row


static func _surface(p_index: int, p_name: String, p_route: String, p_reason: String, p_extra: Dictionary = {}) -> Dictionary:
	var row := {"index": p_index, "name": p_name, "route": p_route, "reason": p_reason}
	for key in p_extra:
		row[key] = p_extra[key]
	return row
