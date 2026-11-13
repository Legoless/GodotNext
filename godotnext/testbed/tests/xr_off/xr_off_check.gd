extends Node3D

## XR keeps NextGI off on both tiers (#255; plan §21.2.6, as UE's Lumen does not support VR).
##
## A SubViewport with use_xr renders a lit NextGI scene in stereo through the MobileVR interface
## (two views, multiview). Pass/fail: the view counts in xr_views_skipped, no NextGI gather
## dispatch runs for it (gather_diffuse_dispatches does not grow), and the tier is active. The
## runner (run_xr_off_check.sh) runs it on HWRT and SWRT under validation.

const WARMUP_FRAMES := 10
const MEASURE_FRAMES := 30

var _failures := PackedStringArray()


func _ready() -> void:
	var xr := XRServer.find_interface("Native mobile")
	if xr == null or not xr.initialize():
		_finish({ "pass": false, "failures": ["the MobileVR interface is not available"] })
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(640, 360)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.use_xr = true
	add_child(viewport)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = Sky.new()
	environment.sky.sky_material = ProceduralSkyMaterial.new()
	environment.nextgi_enabled = true
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	viewport.add_child(world_environment)

	var origin := XROrigin3D.new()
	viewport.add_child(origin)
	var camera := XRCamera3D.new()
	origin.add_child(camera)
	origin.position = Vector3(0, -1.85 + 1.6, 4)

	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(10, 10)
	floor_mesh.mesh = plane
	viewport.add_child(floor_mesh)
	var box := MeshInstance3D.new()
	box.mesh = BoxMesh.new()
	box.position = Vector3(0, 0.5, 0)
	viewport.add_child(box)
	var sun := DirectionalLight3D.new()
	sun.rotation = Vector3(-0.9, 0.4, 0)
	viewport.add_child(sun)

	for _frame in WARMUP_FRAMES:
		await RenderingServer.frame_post_draw
	var before := RenderingServer.nextgi_get_tier_info()
	for _frame in MEASURE_FRAMES:
		await RenderingServer.frame_post_draw
	var after := RenderingServer.nextgi_get_tier_info()

	var tier := String(after.get("tier", ""))
	var skipped := int(after.get("xr_views_skipped", 0)) - int(before.get("xr_views_skipped", 0))
	var gathers := int(after.get("gather_diffuse_dispatches", 0)) - int(before.get("gather_diffuse_dispatches", 0))
	if tier != "HWRT" and tier != "SWRT":
		_failures.append("tier %s is not active; run through run_xr_off_check.sh" % tier)
	if skipped < MEASURE_FRAMES:
		_failures.append("xr_views_skipped grew by %d over %d frames" % [skipped, MEASURE_FRAMES])
	if gathers != 0:
		_failures.append("the NextGI gather ran %d times for the XR view" % gathers)
	if after.get("xr_reason") != "XR_UNSUPPORTED":
		_failures.append("xr_reason is %s" % after.get("xr_reason"))
	xr.uninitialize()
	_finish({
		"tier": tier,
		"xr_views_skipped": skipped,
		"gather_diffuse_dispatches": gathers,
		"pass": _failures.is_empty(),
		"failures": _failures,
	})


func _finish(result: Dictionary) -> void:
	result["test"] = "xr_off"
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)
