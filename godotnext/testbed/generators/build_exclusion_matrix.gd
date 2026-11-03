extends SceneTree
## Exclusion Matrix generator (#74).
##
## Builds the labeled cells in `res://scenes/exclusion_matrix/`. Each case
## sits beside a non-virtualized twin. Routes and reasons come from
## `exclusion_rules.gd`. Two runs write byte-identical scene and JSON files.
##
## [code]godot --headless --path godotnext/testbed --script res://generators/build_exclusion_matrix.gd[/code]

const Scaffold := preload("res://generators/scaffold.gd")
const Rules := preload("res://scenes/exclusion_matrix/exclusion_rules.gd")

const SCENE_PATH := "res://scenes/exclusion_matrix/exclusion_matrix.tscn"
const CELLS_PATH := "res://scenes/exclusion_matrix/cells.json"
const TRACK_PATH := "res://scenes/exclusion_matrix/moving_track.json"
const SCRIPT_PATH := "res://scenes/exclusion_matrix/exclusion_matrix.gd"
const STREAM := 74
const COLUMNS := 6
const SPACING_X := 3.4
const SPACING_Z := 3.0
const BOX := Vector3(0.5, 0.5, 0.5)


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var contract := Rules.contract_error()
	if not contract.is_empty():
		push_error("build_exclusion_matrix: " + contract)
		quit(1)
		return
	var root := _build()
	var err := Scaffold.save_scene(root, SCENE_PATH)
	root.free()
	if err != OK:
		push_error("build_exclusion_matrix: save failed with error %d." % err)
		quit(1)
		return
	if not _write_sidecars():
		quit(1)
		return
	print("build_exclusion_matrix: wrote " + SCENE_PATH)
	quit(0)


func _build() -> Node3D:
	var root := Node3D.new()
	root.name = "ExclusionMatrix"
	root.set_script(load(SCRIPT_PATH))
	root.set_meta("seed", Scaffold.stream_seed(STREAM))
	root.set_meta("issue", 74)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.16, 0.17, 0.19)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.42, 0.43, 0.45)
	environment.ambient_light_energy = 1.0
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.tonemap_exposure = 1.0
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	world.environment = environment
	Scaffold.add_owned(root, world, root)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.light_energy = 1.2
	sun.shadow_enabled = false
	sun.rotation = Vector3(deg_to_rad(-48.0), deg_to_rad(28.0), 0.0)
	Scaffold.add_owned(root, sun, root)

	var floor_mesh := _box_mesh(_lambert("Floor", Color(0.22, 0.23, 0.24)), Vector3(24.0, 0.2, 16.0))
	var floor := _mesh_node(root, root, "Floor", floor_mesh, Vector3(8.5, -0.2, 4.5))

	var rows: Array = Rules.cells()
	for index in rows.size():
		var cell: Dictionary = rows[index]
		_add_cell(root, cell, index)
	floor.set_meta("cell_count", rows.size())

	var center := Vector3(float(COLUMNS - 1) * SPACING_X * 0.5, 0.5, SPACING_Z * 1.5)
	Scaffold.make_camera(root, root, "CaptureCam", center + Vector3(0.0, 18.0, 16.0), center, 120.0, 70.0, true)
	return root


func _add_cell(p_root: Node3D, p_cell: Dictionary, p_index: int) -> void:
	var cell := Node3D.new()
	cell.name = String(p_cell["id"])
	var column := p_index % COLUMNS
	var row := int(p_index / COLUMNS)
	cell.position = Vector3(float(column) * SPACING_X, 0.0, float(row) * SPACING_Z)
	cell.set_meta("cell_id", cell.name)
	Scaffold.add_owned(p_root, cell, p_root)

	var label := Label3D.new()
	label.name = "Label"
	label.text = String(p_cell["label"])
	label.font_size = 32
	label.position = Vector3(0.0, 1.45, 0.0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.modulate = Color(0.95, 0.95, 0.95)
	Scaffold.add_owned(cell, label, p_root)

	var tint := Color.from_hsv(float(p_index) / float(Rules.cells().size()), 0.45, 0.75)
	match String(p_cell["kind"]):
		"multimesh":
			_add_multimesh(p_root, cell, tint)
		"gridmap":
			_add_gridmap(p_root, cell, tint, false)
		"baked_gridmap":
			_add_gridmap(p_root, cell, tint, true)
		"particles":
			_add_particles(p_root, cell, tint)
		"heightfield":
			_add_heightfield(p_root, cell, tint)
		"msaa":
			_add_viewport(p_root, cell, tint, true)
		"multiview":
			_add_viewport(p_root, cell, tint, false)
		"moving":
			_add_moving(p_root, cell, tint)
		_:
			_add_mesh_case(p_root, cell, String(p_cell["id"]), tint)


func _add_mesh_case(p_root: Node3D, p_cell: Node3D, p_id: String, p_tint: Color) -> void:
	var mesh := _case_mesh(p_id, p_tint)
	var candidate := _place_candidate_mesh(p_root, p_cell, p_id, mesh, Vector3(-0.55, 0.25, 0.0))
	var twin_mesh := mesh.duplicate(true) as ArrayMesh
	_place_candidate_mesh(p_root, p_cell, p_id, twin_mesh, Vector3(0.55, 0.25, 0.0), "Twin")
	if p_id == "material_overlay":
		var overlay := _lambert("Overlay", Color(0.9, 0.85, 0.2))
		overlay.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		overlay.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		overlay.albedo_color.a = 0.45
		candidate.material_overlay = overlay
		var twin := p_cell.get_node("Twin") as MeshInstance3D
		twin.material_overlay = overlay.duplicate() as Material
	if p_id == "instance_uniforms":
		candidate.set_instance_shader_parameter("instance_tint", Color(0.2, 0.45, 0.75))
		var twin := p_cell.get_node("Twin") as MeshInstance3D
		twin.set_instance_shader_parameter("instance_tint", Color(0.2, 0.45, 0.75))
	# Screen texture and TIME stay visible. Under --fixed-fps their frames
	# follow the same clock as the capture window. CopyShaderRD from the
	# screen copy is a known #239 pipeline, not a reason to hide the cell.


func _place_candidate_mesh(p_root: Node3D, p_cell: Node3D, p_id: String, p_mesh: ArrayMesh, p_position: Vector3, p_name: String = "Candidate") -> MeshInstance3D:
	if p_id == "skinning":
		return _add_skinned(p_root, p_cell, p_mesh, p_position, p_name)
	return _mesh_node(p_root, p_cell, p_name, p_mesh, p_position)


func _case_mesh(p_id: String, p_tint: Color) -> ArrayMesh:
	match p_id:
		"mixed_opaque_glass":
			var opaque := _box_mesh(_lambert("Opaque", p_tint), BOX)
			var glass_mat := _lambert("Glass", Color(0.4, 0.65, 0.85))
			glass_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			glass_mat.albedo_color.a = 0.4
			var glass := _box_mesh(glass_mat, BOX * 0.72)
			return _combine([opaque, glass])
		"next_pass":
			var base := _lambert("NextPassBase", p_tint)
			var shell := _lambert("NextPassShell", Color(0.95, 0.9, 0.3))
			shell.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			shell.albedo_color.a = 0.55
			shell.grow = true
			shell.grow_amount = 0.02
			base.next_pass = shell
			return _box_mesh(base, BOX)
		"stencil_reader":
			var stencil := _lambert("StencilReader", p_tint)
			stencil.stencil_mode = BaseMaterial3D.STENCIL_MODE_CUSTOM
			stencil.stencil_flags = BaseMaterial3D.STENCIL_FLAG_READ
			stencil.stencil_compare = BaseMaterial3D.STENCIL_COMPARE_ALWAYS
			# A stencil read on an opaque material is rejected by the forward
			# renderer. Disabling depth draw puts the surface in the alpha
			# queue. The fallback reason stays NEXT_PASS_OR_OVERLAY.
			stencil.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
			return _box_mesh(stencil, BOX)
		"vertex_lighting":
			var vertex := _lambert("VertexLighting", p_tint)
			vertex.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
			return _box_mesh(vertex, BOX)
		"position_vertex":
			return _box_mesh(_shader_material("PositionVertex", _position_shader()), BOX)
		"screen_texture":
			return _box_mesh(_shader_material("ScreenTexture", _screen_shader()), BOX)
		"alpha_scissor":
			var scissor := _lambert("AlphaScissor", p_tint)
			scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
			scissor.alpha_scissor_threshold = 0.5
			scissor.albedo_color.a = 0.8
			return _box_mesh(scissor, BOX)
		"alpha_hash":
			var alpha_hash := _lambert("AlphaHash", p_tint)
			alpha_hash.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_HASH
			alpha_hash.albedo_color.a = 0.8
			return _box_mesh(alpha_hash, BOX)
		"unsupported_derivative":
			return _box_mesh(_shader_material("NonVaryingDerivative", _derivative_shader()), BOX)
		"skinning":
			return _skin_arrays(_box_mesh(_lambert("Skinned", p_tint), BOX))
		"instance_uniforms":
			return _box_mesh(_shader_material("InstanceUniform", _instance_shader()), BOX)
		"animated_time":
			return _box_mesh(_shader_material("AnimatedTime", _time_shader()), BOX)
		_:
			return _box_mesh(_lambert(p_id, p_tint), BOX)


func _add_multimesh(p_root: Node3D, p_cell: Node3D, p_tint: Color) -> void:
	var mesh := _box_mesh(_lambert("MultiMeshBox", p_tint), Vector3(0.22, 0.22, 0.22))
	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = mesh
	multi.instance_count = 4
	for index in 4:
		multi.set_instance_transform(index, Transform3D(Basis(), Vector3(float(index) * 0.24 - 0.36, 0.0, 0.0)))
	var instance := MultiMeshInstance3D.new()
	instance.name = "Candidate"
	instance.multimesh = multi
	instance.position = Vector3(-0.55, 0.25, 0.0)
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	Scaffold.add_owned(p_cell, instance, p_root)
	_mesh_node(p_root, p_cell, "Twin", mesh.duplicate(true) as ArrayMesh, Vector3(0.55, 0.25, 0.0))


func _add_gridmap(p_root: Node3D, p_cell: Node3D, p_tint: Color, p_bake: bool) -> void:
	var mesh := _box_mesh(_lambert("GridItem", p_tint), Vector3(0.4, 0.4, 0.4))
	var library := MeshLibrary.new()
	library.create_item(0)
	library.set_item_mesh(0, mesh)
	var grid := GridMap.new()
	grid.name = "Candidate"
	grid.mesh_library = library
	grid.cell_size = Vector3(0.5, 0.5, 0.5)
	grid.cell_scale = 1.0
	grid.set_cell_item(Vector3i(0, 0, 0), 0)
	grid.set_cell_item(Vector3i(1, 0, 0), 0)
	if p_bake:
		grid.make_baked_meshes(false)
	grid.position = Vector3(-0.55, 0.0, 0.0)
	Scaffold.add_owned(p_cell, grid, p_root)
	_mesh_node(p_root, p_cell, "Twin", mesh.duplicate(true) as ArrayMesh, Vector3(0.7, 0.25, 0.0))


func _add_particles(p_root: Node3D, p_cell: Node3D, p_tint: Color) -> void:
	var mesh := _box_mesh(_lambert("Particle", p_tint), Vector3(0.12, 0.12, 0.12))
	var process := ParticleProcessMaterial.new()
	process.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	process.emission_box_extents = Vector3(0.05, 0.05, 0.05)
	process.direction = Vector3(0.0, 1.0, 0.0)
	process.spread = 0.0
	process.initial_velocity_min = 0.35
	process.initial_velocity_max = 0.35
	process.gravity = Vector3.ZERO
	process.scale_min = 1.0
	process.scale_max = 1.0
	var particles := GPUParticles3D.new()
	particles.name = "Candidate"
	particles.process_material = process
	particles.draw_pass_1 = mesh
	particles.amount = 8
	particles.lifetime = 1.0
	particles.use_fixed_seed = true
	particles.seed = 1
	particles.fixed_fps = 60
	particles.interpolate = false
	particles.fract_delta = false
	particles.explosiveness = 0.0
	particles.randomness = 0.0
	particles.emitting = true
	particles.speed_scale = 1.0
	particles.visibility_aabb = AABB(Vector3(-0.5, -0.5, -0.5), Vector3(1.0, 1.0, 1.0))
	particles.position = Vector3(-0.55, 0.3, 0.0)
	particles.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	Scaffold.add_owned(p_cell, particles, p_root)
	_mesh_node(p_root, p_cell, "Twin", mesh.duplicate(true) as ArrayMesh, Vector3(0.55, 0.25, 0.0))


func _add_heightfield(p_root: Node3D, p_cell: Node3D, p_tint: Color) -> void:
	var field := GPUParticlesCollisionHeightField3D.new()
	field.name = "Candidate"
	field.size = Vector3(1.5, 1.5, 1.5)
	field.resolution = GPUParticlesCollisionHeightField3D.RESOLUTION_256
	field.update_mode = GPUParticlesCollisionHeightField3D.UPDATE_MODE_WHEN_MOVED
	field.position = Vector3(-0.55, 0.75, 0.0)
	Scaffold.add_owned(p_cell, field, p_root)
	var proxy := _box_mesh(_lambert("CoarseProxy", p_tint), Vector3(0.4, 0.4, 0.4))
	_mesh_node(p_root, p_cell, "CoarseProxy", proxy, Vector3(-0.55, 0.2, 0.0))
	_mesh_node(p_root, p_cell, "Twin", proxy.duplicate(true) as ArrayMesh, Vector3(0.55, 0.25, 0.0))


func _add_viewport(p_root: Node3D, p_cell: Node3D, p_tint: Color, p_msaa: bool) -> void:
	var viewport := SubViewport.new()
	viewport.name = "Candidate"
	viewport.size = Vector2i(128, 128)
	viewport.own_world_3d = true
	viewport.handle_input_locally = false
	# UPDATE_ALWAYS asks both viewports to draw. MSAA does. Multiview sets
	# use_xr; RendererViewport still skips that draw until a primary XR
	# interface exists. Rules.MULTIVIEW_DRAW_LIMIT records that.
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_2X if p_msaa else Viewport.MSAA_DISABLED
	viewport.use_xr = not p_msaa
	Scaffold.add_owned(p_cell, viewport, p_root)
	var camera := XRCamera3D.new() if not p_msaa else Camera3D.new()
	camera.name = "ViewCam"
	camera.current = true
	camera.position = Vector3(0.0, 0.4, 1.4)
	camera.fov = 50.0
	Scaffold.add_owned(viewport, camera, p_root)
	var mesh := _box_mesh(_lambert("ViewBox", p_tint), BOX)
	_mesh_node(p_root, viewport, "ViewBox", mesh, Vector3(0.0, 0.25, 0.0))
	_mesh_node(p_root, p_cell, "Twin", mesh.duplicate(true) as ArrayMesh, Vector3(0.55, 0.25, 0.0))


func _add_moving(p_root: Node3D, p_cell: Node3D, p_tint: Color) -> void:
	var mesh := _box_mesh(_lambert("Moving", p_tint), BOX)
	_mesh_node(p_root, p_cell, "Candidate", mesh, Rules.moving_offset(0))
	_mesh_node(p_root, p_cell, "Twin", mesh.duplicate(true) as ArrayMesh, Vector3(0.55, 0.25, 0.0))
	var camera := Camera3D.new()
	camera.name = "G57Cam"
	camera.fov = Rules.MOVING_FOV_DEG
	camera.current = false
	var direction := (Rules.MOVING_LOOK_AT - Rules.MOVING_CAMERA_POS).normalized()
	camera.transform = Transform3D(Basis.looking_at(direction, Vector3.UP), Rules.MOVING_CAMERA_POS)
	Scaffold.add_owned(p_cell, camera, p_root)


func _add_skinned(p_root: Node3D, p_cell: Node3D, p_mesh: ArrayMesh, p_position: Vector3, p_name: String) -> MeshInstance3D:
	var skeleton := Skeleton3D.new()
	skeleton.name = p_name + "Rig"
	skeleton.add_bone("root")
	skeleton.set_bone_rest(0, Transform3D.IDENTITY)
	Scaffold.add_owned(p_cell, skeleton, p_root)
	var instance := _mesh_node(p_root, skeleton, p_name, p_mesh, p_position)
	var skin := Skin.new()
	skin.add_named_bind("root", Transform3D.IDENTITY)
	instance.skin = skin
	instance.skeleton = NodePath("..")
	return instance


func _write_sidecars() -> bool:
	var cells_text := Scaffold.to_json({"schema": 1, "cells": Rules.cells()}) + "\n"
	var track := {
		"schema": 1,
		"seed": Scaffold.stream_seed(STREAM),
		"fov_deg": Rules.MOVING_FOV_DEG,
		"aspect": Rules.MOVING_ASPECT,
		"camera_position": [Rules.MOVING_CAMERA_POS.x, Rules.MOVING_CAMERA_POS.y, Rules.MOVING_CAMERA_POS.z],
		"look_at": [Rules.MOVING_LOOK_AT.x, Rules.MOVING_LOOK_AT.y, Rules.MOVING_LOOK_AT.z],
		"dlss": Rules.dlss_status(),
		"samples": Rules.track_samples(),
	}
	var cells_err := Scaffold.write_text_chunked(CELLS_PATH, cells_text)
	var track_err := Scaffold.write_text_chunked(TRACK_PATH, Scaffold.to_json(track) + "\n")
	if cells_err != OK or track_err != OK:
		push_error("build_exclusion_matrix: sidecar write failed.")
		return false
	return true


func _lambert(p_name: String, p_color: Color) -> StandardMaterial3D:
	return Scaffold.make_constant_material(p_name, p_color, 1.0, 0.0, 0.0)


func _shader_material(p_name: String, p_code: String) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = p_code
	var material := ShaderMaterial.new()
	material.resource_name = p_name
	material.shader = shader
	return material


func _position_shader() -> String:
	return "shader_type spatial;\nvoid vertex() {\n\tVERTEX.y += 0.02;\n}\nvoid fragment() {\n\tALBEDO = vec3(0.55, 0.22, 0.18);\n}\n"


func _screen_shader() -> String:
	return "shader_type spatial;\nuniform sampler2D screen_texture : hint_screen_texture, filter_linear_mipmap;\nvoid fragment() {\n\tALBEDO = texture(screen_texture, SCREEN_UV).rgb;\n}\n"


func _derivative_shader() -> String:
	return "shader_type spatial;\nuniform float factor = 1.0;\nvoid fragment() {\n\tfloat band = dFdx(factor);\n\tALBEDO = vec3(0.45, 0.25, 0.55) + vec3(band);\n}\n"


func _instance_shader() -> String:
	return "shader_type spatial;\ninstance uniform vec4 instance_tint : source_color = vec4(0.2, 0.45, 0.75, 1.0);\nvoid fragment() {\n\tALBEDO = instance_tint.rgb;\n}\n"


func _time_shader() -> String:
	return "shader_type spatial;\nvoid fragment() {\n\tALBEDO = vec3(0.15, 0.45, 0.25) * (0.75 + 0.25 * sin(TIME));\n}\n"


func _box_mesh(p_material: Material, p_size: Vector3) -> ArrayMesh:
	var builder = Scaffold.ChunkedSurfaceBuilder.new(p_material)
	builder.add_box(Vector3.ZERO, p_size)
	return builder.finish()


func _combine(p_parts: Array) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	for part_value in p_parts:
		var part := part_value as ArrayMesh
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, part.surface_get_arrays(0))
		mesh.surface_set_material(mesh.get_surface_count() - 1, part.surface_get_material(0))
	return mesh


func _skin_arrays(p_mesh: ArrayMesh) -> ArrayMesh:
	var arrays := p_mesh.surface_get_arrays(0)
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	bones.resize(vertices.size() * 4)
	weights.resize(vertices.size() * 4)
	for index in vertices.size():
		bones[index * 4] = 0
		weights[index * 4] = 1.0
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	var skinned := ArrayMesh.new()
	skinned.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	skinned.surface_set_material(0, p_mesh.surface_get_material(0))
	return skinned


func _mesh_node(p_root: Node, p_parent: Node, p_name: String, p_mesh: ArrayMesh, p_position: Vector3) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = p_name
	instance.mesh = p_mesh
	instance.position = p_position
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	Scaffold.add_owned(p_parent, instance, p_root)
	return instance
