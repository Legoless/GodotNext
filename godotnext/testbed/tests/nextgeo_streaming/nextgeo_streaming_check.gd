extends Node3D
## NextGeo page residency and streaming end-to-end GPU check (#78):
## 1. Non-main SubViewport renders pinned coarse root pages without emitting
##    page requests (page_uploads == 0, non_main_view_requests == 0).
## 2. Under wall-time IO delay injection (debug_io_delay_ms), main viewport
##    renders virtual geometry immediately from pinned coarse root pages with
##    zero legacy fallback, then streams fine pages parents-first with bit-exact
##    CPU/GPU cut_hash parity on every intermediate and final frame.
## 3. Under a tight 2 MiB (16-slot) page cache, flying the main camera to the
##    second mesh evicts unused pages children-first (page_evictions > 0) while
##    preserving child_resident_without_parent == 0 and evictions_with_resident_child == 0.
## 4. Verifies virtual_geometry_get_memory_report() (all 10 Q15 keys, page_cache == arena)
##    and virtual_geometry_get_routing_counters() (page_upload_ms + residency counters).

const MEMORY_KEYS: Array[String] = [
	"authored_remainder",
	"lods",
	"shadow_mesh",
	"coarse_fallback",
	"rt_arena",
	"blas",
	"tlas",
	"as_scratch",
	"dag_headers",
	"page_cache",
]

const CACHE_MB := 2

var _phase := ""
var _phase_frame := 0
var _temp_files: Array[String] = []
var _ngm_counter := 0
var _failures: Array[String] = []
var _summary: Dictionary = {}

var _world_root: Node3D
var _main_cam: Camera3D
var _main_mesh_inst: MeshInstance3D
var _aux_mesh_inst: MeshInstance3D
var _sub_vp: SubViewport
var _aux_cam: Camera3D

var _uploads_after_main := 0
var _saw_coarse_under_delay := false
var _coarse_checked := false


func _ready() -> void:
	ProjectSettings.set_setting("rendering/nextgeo/visibility_only", false)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats", true)
	ProjectSettings.set_setting("rendering/nextgeo/debug_stats_interval", 1)
	ProjectSettings.set_setting("rendering/nextgeo/max_pixel_error", 0.0)
	ProjectSettings.set_setting("rendering/limits/nextgeo/geometry_cache_mb", CACHE_MB)
	ProjectSettings.set_setting("rendering/limits/nextgeo/page_upload_kib_per_frame", 256)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(0.0, 0.0))

	var vp := get_viewport()
	vp.use_taa = false
	vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	vp.scaling_3d_scale = 1.0

	_build_scene()
	if not _failures.is_empty():
		_finish()
		return

	_phase = "subviewport_only"
	_phase_frame = 0


func _alloc_temp_ngm(p_tag: String) -> String:
	_ngm_counter += 1
	var path := "user://ng_stream_%s_%d.ngm" % [p_tag, _ngm_counter]
	_temp_files.append(path)
	return path


func _make_dense_virtual_quad(p_tag: String, p_color: Color) -> ArrayMesh:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = p_color
	mat.roughness = 0.85
	mat.metallic = 0.0

	var qmesh := QuadMesh.new()
	qmesh.size = Vector2(3.0, 3.0)
	qmesh.subdivide_width = 72
	qmesh.subdivide_depth = 72
	qmesh.material = mat

	var imp := ImporterMesh.from_mesh(qmesh)
	var err := imp.build_virtual_geometry(_alloc_temp_ngm(p_tag))
	if err != OK:
		_failures.append("%s: build_virtual_geometry failed (err=%d)" % [p_tag, err])
		return null
	var vmesh := imp.get_mesh()
	if vmesh == null or vmesh.get_virtual_geometry() == null:
		_failures.append("%s: virtual geometry missing on built mesh" % p_tag)
		return null
	var vg := vmesh.get_virtual_geometry()
	var total_pages := vg.get_page_count()
	var root_pages := vg.get_root_pages().size()
	var cache_slots := (CACHE_MB * 1024 * 1024) / maxi(vg.get_page_size(), 1)
	if total_pages <= root_pages or total_pages >= cache_slots or total_pages * 2 <= cache_slots:
		_failures.append("%s: expected 1 mesh < %d slots < 2 meshes (page_count=%d, root_pages=%d, page_size=%d)" % [
			p_tag, cache_slots, total_pages, root_pages, vg.get_page_size()])
	return vmesh


func _build_scene() -> void:
	_world_root = Node3D.new()
	_world_root.name = "StreamingWorld"
	add_child(_world_root)

	var main_vmesh := _make_dense_virtual_quad("main_quad", Color(0.15, 0.90, 0.25))
	var aux_vmesh := _make_dense_virtual_quad("aux_quad", Color(0.20, 0.45, 0.95))
	if main_vmesh == null or aux_vmesh == null:
		return

	_main_mesh_inst = MeshInstance3D.new()
	_main_mesh_inst.name = "MainVirtualQuad"
	_main_mesh_inst.mesh = main_vmesh
	_main_mesh_inst.position = Vector3(0.0, 0.0, 0.0)
	_world_root.add_child(_main_mesh_inst)

	_aux_mesh_inst = MeshInstance3D.new()
	_aux_mesh_inst.name = "AuxVirtualQuad"
	_aux_mesh_inst.mesh = aux_vmesh
	_aux_mesh_inst.position = Vector3(200.0, 0.0, 0.0)
	_world_root.add_child(_aux_mesh_inst)

	# Orthogonal camera framing the entire 3x3 quad cleanly in frustum.
	# Start pointed at empty space (-200, 0, 4) so only SubViewport sees a mesh in Phase 1.
	_main_cam = Camera3D.new()
	_main_cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	_main_cam.size = 3.8
	_main_cam.near = 0.1
	_main_cam.far = 50.0
	_main_cam.position = Vector3(-200.0, 0.0, 4.0)
	_main_cam.current = true
	_world_root.add_child(_main_cam)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.03, 0.03, 0.04)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1.0, 1.0, 1.0)
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	_world_root.add_child(we)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-25.0, 20.0, 0.0)
	_world_root.add_child(sun)

	# Active non-main SubViewport sharing World3D, framing AuxVirtualQuad.
	_sub_vp = SubViewport.new()
	_sub_vp.name = "AuxSubViewport"
	_sub_vp.size = Vector2i(256, 256)
	_sub_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_sub_vp.world_3d = get_viewport().find_world_3d()
	_world_root.add_child(_sub_vp)

	_aux_cam = Camera3D.new()
	_aux_cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	_aux_cam.size = 3.8
	_aux_cam.near = 0.1
	_aux_cam.far = 50.0
	_aux_cam.position = Vector3(200.0, 0.0, 4.0)
	_aux_cam.current = true
	_sub_vp.add_child(_aux_cam)


func _check_frame_invariants(p_label: String, p_require_cut_match: bool) -> Dictionary:
	var rc: Dictionary = RenderingServer.virtual_geometry_get_routing_counters()
	var vp_stats: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(get_viewport().get_viewport_rid())

	if int(rc.get("child_resident_without_parent", -1)) != 0:
		_failures.append("%s: child_resident_without_parent=%s" % [p_label, str(rc.get("child_resident_without_parent"))])
	if int(rc.get("evictions_with_resident_child", -1)) != 0:
		_failures.append("%s: evictions_with_resident_child=%s" % [p_label, str(rc.get("evictions_with_resident_child"))])
	if int(rc.get("non_main_view_requests", -1)) != 0:
		_failures.append("%s: non_main_view_requests=%s" % [p_label, str(rc.get("non_main_view_requests"))])
	if int(vp_stats.get("double_dispatch", 0)) != 0:
		_failures.append("%s: double_dispatch=%s" % [p_label, str(vp_stats.get("double_dispatch"))])
	if int(vp_stats.get("missing", 0)) != 0:
		_failures.append("%s: missing=%s" % [p_label, str(vp_stats.get("missing"))])
	if p_require_cut_match and vp_stats.has("cut_match") and not bool(vp_stats["cut_match"]):
		_failures.append("%s: GPU/CPU cut_hash mismatch (%s)" % [p_label, JSON.stringify(vp_stats)])
	return rc


func _sample_center_pixel(p_vp: Viewport) -> Color:
	var img := p_vp.get_texture().get_image()
	if img == null or img.is_empty():
		return Color(-1.0, -1.0, -1.0, 0.0)
	return img.get_pixel(img.get_width() / 2, img.get_height() / 2)


func _process(_delta: float) -> void:
	_phase_frame += 1
	match _phase:
		"subviewport_only":
			_tick_subviewport_only()
		"main_delayed":
			_tick_main_delayed()
		"aux_on_main":
			_tick_aux_on_main()


func _tick_subviewport_only() -> void:
	OS.delay_msec(6)
	var rc := _check_frame_invariants("p1_f%d" % _phase_frame, _phase_frame >= 3)
	if _phase_frame < 10:
		return

	var aux_px := _sample_center_pixel(_sub_vp)
	if aux_px.b < 0.25 or aux_px.b <= aux_px.r * 1.5:
		_failures.append("p1_subviewport_only: AuxSubViewport did not render coarse virtual quad (center=%s)" % str(aux_px))

	var uploads := int(rc.get("page_uploads", -1))
	var resident := int(rc.get("resident_pages", -1))
	var pinned := int(rc.get("pinned_pages", -1))
	if pinned < 2:
		_failures.append("p1_subviewport_only: expected >= 2 pinned root pages, got %d" % pinned)
	if uploads != 0 or resident != pinned:
		_failures.append("p1_subviewport_only: non-main SubViewport triggered page uploads (uploads=%d resident=%d pinned=%d)" % [
			uploads, resident, pinned])

	_summary["p1_subviewport_only"] = {
		"pinned_pages": pinned,
		"resident_pages": resident,
		"page_uploads": uploads,
		"aux_center_b": snappedf(aux_px.b, 0.001),
	}

	# Phase 2: inject 30..50 ms wall-time IO delay and point main camera at MainVirtualQuad.
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(30.0, 50.0))
	_main_cam.position = Vector3(0.0, 0.0, 4.0)
	_coarse_checked = false
	_saw_coarse_under_delay = false
	_phase = "main_delayed"
	_phase_frame = 0


func _tick_main_delayed() -> void:
	var rc := _check_frame_invariants("p2_f%d" % _phase_frame, _phase_frame >= 3)

	# On frame 2 (before the 30..50 ms IO delay can complete), verify the main
	# viewport already renders MainVirtualQuad via pinned coarse root pages with
	# route == "virtual" (no legacy fallback).
	if _phase_frame == 2 and not _coarse_checked:
		_coarse_checked = true
		var main_px := _sample_center_pixel(get_viewport())
		var inst_st: Dictionary = RenderingServer.virtual_geometry_get_debug_stats(_main_mesh_inst.get_instance())
		var surfaces: Array = inst_st.get("surfaces", [])
		var route := String((surfaces[0] as Dictionary).get("route", "")) if not surfaces.is_empty() else ""
		if route != "virtual":
			_failures.append("p2_coarse: expected route='virtual' while fine pages stream, got '%s'" % route)
		if main_px.g > 0.25 and main_px.g > main_px.r * 1.5:
			_saw_coarse_under_delay = true
		else:
			_failures.append("p2_coarse: main viewport did not render coarse parent cut under IO delay (center=%s)" % str(main_px))
		return

	if _phase_frame > 2:
		OS.delay_msec(12)

	var main_vg := (_main_mesh_inst.mesh as ArrayMesh).get_virtual_geometry()
	var expected_main_non_root := main_vg.get_page_count() - main_vg.get_root_pages().size()
	var uploads := int(rc.get("page_uploads", 0))
	var resident := int(rc.get("resident_pages", 0))

	if uploads >= expected_main_non_root and _phase_frame >= 6:
		var main_px_fin := _sample_center_pixel(get_viewport())
		if main_px_fin.g < 0.25:
			_failures.append("p2_refined: main viewport lost virtual quad after streaming (center=%s)" % str(main_px_fin))
		_uploads_after_main = uploads
		_summary["p2_main_delayed"] = {
			"frames": _phase_frame,
			"saw_coarse_under_delay": _saw_coarse_under_delay,
			"main_page_count": main_vg.get_page_count(),
			"page_uploads": uploads,
			"resident_pages": resident,
		}
		# Phase 3: fly main camera to AuxVirtualQuad; under 16-slot cache, unused MainVirtualQuad
		# pages evict children-first so AuxVirtualQuad's fine pages can stream in.
		ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(5.0, 10.0))
		_main_cam.position = Vector3(200.0, 0.0, 4.0)
		_phase = "aux_on_main"
		_phase_frame = 0
		return

	if _phase_frame >= 90:
		_failures.append("p2_main_delayed: timed out waiting for fine pages (uploads=%d expected>=%d resident=%d)" % [
			uploads, expected_main_non_root, resident])
		_finish()


func _tick_aux_on_main() -> void:
	OS.delay_msec(10)
	var rc := _check_frame_invariants("p3_f%d" % _phase_frame, _phase_frame >= 3)

	var main_vg := (_main_mesh_inst.mesh as ArrayMesh).get_virtual_geometry()
	var aux_vg := (_aux_mesh_inst.mesh as ArrayMesh).get_virtual_geometry()
	var expected_aux_non_root := aux_vg.get_page_count() - aux_vg.get_root_pages().size()
	var total_combined_pages := main_vg.get_page_count() + aux_vg.get_page_count()
	var uploads := int(rc.get("page_uploads", 0))
	var evictions := int(rc.get("page_evictions", 0))
	var resident := int(rc.get("resident_pages", 0))

	if uploads >= _uploads_after_main + expected_aux_non_root and _phase_frame >= 6:
		var main_px := _sample_center_pixel(get_viewport())
		if main_px.b < 0.25 or main_px.b <= main_px.r * 1.5:
			_failures.append("p3_aux_on_main: main viewport did not render AuxVirtualQuad (center=%s)" % str(main_px))
		var cache_slots := (CACHE_MB * 1024 * 1024) / maxi(aux_vg.get_page_size(), 1)
		if total_combined_pages > cache_slots and evictions <= 0:
			_failures.append("p3_aux_on_main: expected child-first LRU evictions (> 0) for %d total pages in %d slots, got %d" % [
				total_combined_pages, cache_slots, evictions])
		_verify_reports(rc)
		_summary["p3_aux_on_main"] = {
			"frames": _phase_frame,
			"total_combined_pages": total_combined_pages,
			"total_page_uploads": uploads,
			"page_evictions": evictions,
			"resident_pages": resident,
			"page_upload_ms": float(rc.get("page_upload_ms", -1.0)),
		}
		_finish()
		return

	if _phase_frame >= 90:
		_failures.append("p3_aux_on_main: timed out waiting for aux pages (uploads=%d base=%d aux_non_root=%d evictions=%d)" % [
			uploads, _uploads_after_main, expected_aux_non_root, evictions])
		_verify_reports(rc)
		_finish()


func _verify_reports(p_rc: Dictionary) -> void:
	if not p_rc.has("page_upload_ms") or float(p_rc["page_upload_ms"]) < 0.0:
		_failures.append("reports: invalid page_upload_ms in routing_counters (%s)" % JSON.stringify(p_rc))

	var mem: Dictionary = RenderingServer.virtual_geometry_get_memory_report()
	for key in MEMORY_KEYS:
		if not mem.has(key):
			_failures.append("reports: virtual_geometry_get_memory_report missing key '%s'" % key)
		elif int(mem[key]) < 0:
			_failures.append("reports: key '%s' is negative (%s)" % [key, str(mem[key])])

	var expected_arena := CACHE_MB * 1024 * 1024
	if int(mem.get("page_cache", 0)) != expected_arena:
		_failures.append("reports: expected page_cache == %d, got %s" % [expected_arena, str(mem.get("page_cache"))])
	if int(mem.get("dag_headers", 0)) <= 0:
		_failures.append("reports: expected dag_headers > 0, got %s" % str(mem.get("dag_headers")))

	_summary["memory_report"] = {
		"page_cache": int(mem.get("page_cache", 0)),
		"dag_headers": int(mem.get("dag_headers", 0)),
		"authored_remainder": int(mem.get("authored_remainder", 0)),
	}


func _finish() -> void:
	set_process(false)
	ProjectSettings.set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(0.0, 0.0))
	if _world_root != null:
		remove_child(_world_root)
		_world_root.free()
		_world_root = null
	for path in _temp_files:
		var abs_p := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs_p):
			DirAccess.remove_absolute(abs_p)
	_temp_files.clear()

	var ok := _failures.is_empty()
	print("NEXTGEO_STREAMING_SUMMARY pass=%s summary=%s failures=%s" % [
		str(ok),
		JSON.stringify(_summary),
		JSON.stringify(_failures),
	])
	get_tree().quit(0 if ok else 1)
