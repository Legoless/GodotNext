extends SceneTree
## Dense City Blocks seeded generator (#63, plan §13.3, §2 row 1).
##
## Run from the repository root, one variant per run:
## [code]godot --headless --path godotnext/testbed --script res://generators/dense_city/dense_city_generator.gd -- --seed=1 --variant=virtual[/code]
##
## Variants: [code]virtual[/code] (the Dense City scene and its kit),
## [code]lod_baseline[/code] (the same transforms on discrete-LOD imports),
## [code]sweep_10m[/code]/[code]sweep_100m[/code]/[code]sweep_1b[/code]
## (instanced-triangle scale sweeps), [code]kit[/code] (the multi-material
## kit asset alone; [code]--materials=<n>[/code] sets its surface count) and
## [code]stress_10m[/code] (one 10M-triangle single-surface mesh as
## [code].glb[/code] and [code].obj[/code]).
##
## Seeded and byte-deterministic on one machine: every random draw comes
## from a [RandomNumberGenerator] derived from [code]--seed[/code]; there is
## no global RNG, no time and no unordered-dictionary iteration. Outputs go
## to [code]--out[/code] (default [code]res://generated/dense_city[/code],
## gitignored). Each run asserts its variant's scale counters, prints them,
## records them with the SHA-256 of every output in the tracked
## [code]expected.json[/code] next to this script, and exits non-zero on any
## failed assertion. Counts, not timing, are checked here (§22 build-first);
## gates G4-5/G5-1 run in PQ.
##
## Kit meshes are subdivided plane facades displaced in one vertex loop
## (window insets, floor ledges, low-amplitude noise; zero displacement on
## every panel edge so seams stay coincident), written as [code].glb[/code]
## through [GLTFDocument] with a pre-seeded [code].glb.import[/code] beside
## each: no authored LODs and no shadow meshes. City scenes instance those
## imported scenes under one [code]Buildings[/code] node; the scene text is
## produced with the scaffold's byte-stable scene saver over
## placeholder nodes, then the placeholders are rewritten to scene
## instances, so the [code].tscn[/code] stays small and byte-stable.

const Scaffold := preload("res://generators/scaffold.gd")

const EXPECTED_PATH := "res://generators/dense_city/expected.json"
const CFG_PATH := "res://generators/dense_city/variants.cfg"
const VARIANTS: Array[String] = ["virtual", "lod_baseline", "sweep_10m", "sweep_100m", "sweep_1b", "kit", "stress_10m"]

## Mesh recipes for the kit. Subdivisions are (width, height) quad counts of
## each facade plane; the generator counts the actual triangles from the
## built arrays and asserts the totals, so the table only has to be close.
## Order matters: placement and scene node names index into this table.
const RECIPES: Array[Dictionary] = [
	{"name": "building_a", "kind": "building", "w": 24.0, "d": 16.0, "h": 60.0,
		"front": Vector2i(352, 704), "side": Vector2i(256, 704), "roof": Vector2i(160, 112),
		"cols": 24, "rows": 40, "instances": 12},
	{"name": "building_b", "kind": "building", "w": 20.0, "d": 18.0, "h": 72.0,
		"front": Vector2i(320, 768), "side": Vector2i(288, 768), "roof": Vector2i(144, 128),
		"cols": 20, "rows": 48, "instances": 12},
	{"name": "building_c", "kind": "building", "w": 28.0, "d": 14.0, "h": 48.0,
		"front": Vector2i(384, 576), "side": Vector2i(224, 576), "roof": Vector2i(176, 96),
		"cols": 28, "rows": 32, "instances": 12},
	{"name": "building_d", "kind": "building", "w": 18.0, "d": 18.0, "h": 84.0,
		"front": Vector2i(288, 832), "side": Vector2i(288, 832), "roof": Vector2i(128, 128),
		"cols": 18, "rows": 56, "instances": 12},
	{"name": "building_e", "kind": "building", "w": 26.0, "d": 20.0, "h": 54.0,
		"front": Vector2i(368, 640), "side": Vector2i(288, 640), "roof": Vector2i(160, 128),
		"cols": 26, "rows": 36, "instances": 12},
	{"name": "tower_slim", "kind": "building", "w": 12.0, "d": 12.0, "h": 96.0,
		"front": Vector2i(128, 512), "side": Vector2i(128, 512), "roof": Vector2i(64, 64),
		"cols": 12, "rows": 64, "instances": 20},
	{"name": "building_small", "kind": "building", "w": 16.0, "d": 12.0, "h": 24.0,
		"front": Vector2i(192, 256), "side": Vector2i(160, 256), "roof": Vector2i(112, 80),
		"cols": 16, "rows": 16, "instances": 20},
	{"name": "kit_facade", "kind": "kit", "w": 24.0, "d": 16.0, "h": 42.0,
		"perimeter_subdiv": 512, "height_subdiv": 512, "roof": Vector2i(128, 96),
		"cols": 32, "rows": 28, "instances": 40},
	{"name": "prop_rooftop", "kind": "prop", "w": 2.2, "d": 1.6, "h": 1.1, "instances": 4900},
]
const KIT_RECIPE_INDEX := 7

## Linear-space constant albedos, one per kit surface slot (cycled).
const PALETTE: Array[Color] = [
	Color(0.55, 0.50, 0.46), Color(0.42, 0.44, 0.50), Color(0.60, 0.38, 0.30),
	Color(0.38, 0.48, 0.42), Color(0.65, 0.58, 0.40), Color(0.48, 0.40, 0.55),
	Color(0.35, 0.52, 0.58), Color(0.58, 0.45, 0.38), Color(0.44, 0.56, 0.34),
	Color(0.52, 0.34, 0.44), Color(0.62, 0.62, 0.58), Color(0.40, 0.46, 0.62),
]

const LOT_SPACING := 72.0
const LOT_GRID_RADIUS := 10
const MAIN_STREET_HALF_WIDTH := 26.0
const TILE_SPAN := 1800.0
const STRESS_SUBDIV := 2236

var _seed := 1
var _variant := "virtual"
var _materials := 8
var _out := "res://generated/dense_city"
var _cfg := {}
var _failures: Array[String] = []
var _outputs: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _fail(p_message: String) -> void:
	_failures.append(p_message)
	printerr("dense_city FAIL: " + p_message)


func _check(p_condition: bool, p_message: String) -> bool:
	if not p_condition:
		_fail(p_message)
	return p_condition


func _run() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--seed="):
			_seed = int(arg.trim_prefix("--seed="))
		elif arg.begins_with("--variant="):
			_variant = arg.trim_prefix("--variant=")
		elif arg.begins_with("--materials="):
			_materials = maxi(int(arg.trim_prefix("--materials=")), 1)
		elif arg.begins_with("--out="):
			_out = arg.trim_prefix("--out=")
		else:
			_fail("unknown argument '%s'." % arg)
	if not VARIANTS.has(_variant):
		_fail("unknown variant '%s' (expected one of %s)." % [_variant, ", ".join(VARIANTS)])
	_load_cfg()
	print("dense_city: seed=%d variant=%s out=%s" % [_seed, _variant, _out])
	var entry := {}
	if _failures.is_empty():
		match _variant:
			"virtual":
				entry = _variant_virtual()
			"lod_baseline":
				entry = _variant_lod_baseline()
			"sweep_10m", "sweep_100m", "sweep_1b":
				entry = _variant_sweep()
			"kit":
				entry = _variant_kit()
			"stress_10m":
				entry = _variant_stress()
	if not entry.is_empty() and _failures.is_empty():
		_write_expected(entry)
	if _failures.is_empty():
		print("dense_city: variant %s done, %d outputs." % [_variant, _outputs.size()])
		quit(0)
	else:
		for failure in _failures:
			push_error("dense_city: " + failure)
		quit(1)


func _load_cfg() -> void:
	_cfg = {
		"instances_min": 5000,
		"unique_triangles_min": 10000000,
		"reachable_clusters_min": 1000000,
		"geometry_cache_budget_bytes": 128 * 1024 * 1024,
		"working_set_ratio_min": 2.0,
		"kit_materials": 8,
		"kit_triangles_min": 500000,
		"sweep_10m": 10000000,
		"sweep_100m": 100000000,
		"sweep_1b": 1000000000,
		"stress_triangles": 10000000,
	}
	var cfg := ConfigFile.new()
	if cfg.load(CFG_PATH) != OK:
		return
	for key in _cfg:
		for section in ["virtual", "kit", "sweeps", "stress"]:
			if cfg.has_section_key(section, key):
				_cfg[key] = cfg.get_value(section, key)


## One seeded stream: the same (seed, stream) pair always draws the same
## sequence, independent of variant and build order.
func _rng(p_stream: int) -> RandomNumberGenerator:
	var rng := RandomNumberGenerator.new()
	var s := absi(_seed) % 1000000007
	rng.seed = s * 1000003 + p_stream * 97 + 12345
	return rng


## Fisher-Yates with the seeded generator (Array.shuffle uses the global RNG).
func _shuffle(p_array: Array, p_rng: RandomNumberGenerator) -> void:
	for i in range(p_array.size() - 1, 0, -1):
		var j := p_rng.randi_range(0, i)
		var tmp = p_array[i]
		p_array[i] = p_array[j]
		p_array[j] = tmp


func _material(p_index: int, p_suffix: String = "") -> StandardMaterial3D:
	var color: Color = PALETTE[p_index % PALETTE.size()]
	return Scaffold.make_constant_material("DenseCityMat%d%s" % [p_index, p_suffix], color, 0.9)


## Facade displacement along the plane normal: window insets on a
## (cols x rows) grid, a ledge at every floor line, and small deterministic
## noise. Exactly zero on the panel border so neighboring panels keep
## coincident seam vertices.
func _displacement(p_x: float, p_z: float, p_w: float, p_h: float, p_params: Dictionary) -> float:
	if absf(p_x) >= p_w * 0.5 - 0.001 or absf(p_z) >= p_h * 0.5 - 0.001:
		return 0.0
	var u := (p_x / p_w + 0.5) * float(p_params["cols"])
	var v := (p_z / p_h + 0.5) * float(p_params["rows"])
	var fu := u - floorf(u)
	var fv := v - floorf(v)
	var d := 0.0
	if fu > 0.28 and fu < 0.72 and fv > 0.30 and fv < 0.78:
		d -= float(p_params["inset"])
	if fv < 0.07:
		d += float(p_params["ledge"])
	d += float(p_params["noise"]) * sin(p_x * 37.0 + float(p_params["phase"])) * sin(p_z * 41.0 + float(p_params["phase"]) * 0.7)
	return d


## One displaced, transformed facade surface from a subdivided [PlaneMesh]:
## the plane's indexed arrays are displaced along +Y and mapped through
## [param p_transform] in a single vertex loop. Tangents are dropped (the
## constant materials have no normal maps; the importer can re-derive them).
func _facade_arrays(p_w: float, p_h: float, p_sub: Vector2i, p_transform: Transform3D, p_params: Dictionary) -> Array:
	var plane := PlaneMesh.new()
	plane.size = Vector2(p_w, p_h)
	plane.subdivide_width = p_sub.x
	plane.subdivide_depth = p_sub.y
	var arrays := plane.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var basis := p_transform.basis
	var origin := p_transform.origin
	for i in range(verts.size()):
		var vertex := verts[i]
		vertex.y += _displacement(vertex.x, vertex.z, p_w, p_h, p_params)
		verts[i] = basis * vertex + origin
		normals[i] = (basis * normals[i]).normalized()
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TANGENT] = null
	return arrays


## Merges indexed surface arrays into one surface (vertex, normal, UV and
## index only), offsetting indices. Used for the kit surfaces that span a
## corner and for the prop mesh.
func _merge_arrays(p_parts: Array) -> Array:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for part_value in p_parts:
		var part: Array = part_value
		var p_verts: PackedVector3Array = part[Mesh.ARRAY_VERTEX]
		var p_normals: PackedVector3Array = part[Mesh.ARRAY_NORMAL]
		var p_uvs: PackedVector2Array = part[Mesh.ARRAY_TEX_UV]
		var p_indices: PackedInt32Array = part[Mesh.ARRAY_INDEX]
		var offset := verts.size()
		verts.append_array(p_verts)
		normals.append_array(p_normals)
		uvs.append_array(p_uvs)
		indices.resize(indices.size() + p_indices.size())
		var base := indices.size() - p_indices.size()
		for i in range(p_indices.size()):
			indices[base + i] = p_indices[i] + offset
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	return arrays


func _surface_stats(p_arrays: Array) -> Dictionary:
	var verts: PackedVector3Array = p_arrays[Mesh.ARRAY_VERTEX]
	var indices: PackedInt32Array = p_arrays[Mesh.ARRAY_INDEX]
	var triangles := indices.size() / 3 if not indices.is_empty() else verts.size() / 3
	return {"vertices": verts.size(), "triangles": triangles}


func _mesh_stats(p_mesh: ArrayMesh) -> Dictionary:
	var vertices := 0
	var triangles := 0
	for surface in range(p_mesh.get_surface_count()):
		var stats := _surface_stats(p_mesh.surface_get_arrays(surface))
		vertices += int(stats["vertices"])
		triangles += int(stats["triangles"])
	var aabb := p_mesh.get_aabb()
	return {
		"vertices": vertices,
		"triangles": triangles,
		"surfaces": p_mesh.get_surface_count(),
		"aabb": [aabb.position.x, aabb.position.y, aabb.position.z, aabb.size.x, aabb.size.y, aabb.size.z],
	}


## Counts, per surface, whether it shares at least one coincident vertex
## position (within 0.1 mm quantization) with a different surface.
func _seam_check(p_surfaces: Array) -> Dictionary:
	var owner := {}
	var shared := {}
	for surface in range(p_surfaces.size()):
		var arrays: Array = p_surfaces[surface]
		var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for vertex in verts:
			var key := Vector3i(roundi(vertex.x * 10000.0), roundi(vertex.y * 10000.0), roundi(vertex.z * 10000.0))
			if owner.has(key):
				if int(owner[key]) != surface:
					shared[int(owner[key])] = true
					shared[surface] = true
			else:
				owner[key] = surface
	return {"surfaces_with_seam": shared.size(), "surfaces": p_surfaces.size()}


func _facade_transform(p_yaw: float, p_center: Vector3) -> Transform3D:
	return Transform3D(Basis(Vector3.UP, p_yaw) * Basis(Vector3.RIGHT, -PI * 0.5), p_center)


## A building: four displaced facade surfaces plus a displaced roof, all in
## one material. Local origin at ground center, +Y up.
func _build_building_mesh(p_recipe: Dictionary, p_material: Material, p_rng: RandomNumberGenerator) -> ArrayMesh:
	var w := float(p_recipe["w"])
	var d := float(p_recipe["d"])
	var h := float(p_recipe["h"])
	var params := {
		"cols": int(p_recipe["cols"]), "rows": int(p_recipe["rows"]),
		"inset": p_rng.randf_range(0.10, 0.16), "ledge": p_rng.randf_range(0.04, 0.07),
		"noise": 0.006, "phase": p_rng.randf_range(0.0, 6.28318),
	}
	var side_params := params.duplicate()
	side_params["cols"] = maxi(roundi(float(params["cols"]) * d / w), 4)
	var front: Vector2i = p_recipe["front"]
	var side: Vector2i = p_recipe["side"]
	var roof: Vector2i = p_recipe["roof"]
	var surfaces: Array = [
		_facade_arrays(w, h, front, _facade_transform(0.0, Vector3(0.0, h * 0.5, -d * 0.5)), params),
		_facade_arrays(w, h, front, _facade_transform(PI, Vector3(0.0, h * 0.5, d * 0.5)), params),
		_facade_arrays(d, h, side, _facade_transform(-PI * 0.5, Vector3(w * 0.5, h * 0.5, 0.0)), side_params),
		_facade_arrays(d, h, side, _facade_transform(PI * 0.5, Vector3(-w * 0.5, h * 0.5, 0.0)), side_params),
	]
	var roof_params := params.duplicate()
	roof_params["inset"] = -0.35
	roof_params["ledge"] = 0.0
	surfaces.append(_facade_arrays(w, d, roof, Transform3D(Basis.IDENTITY, Vector3(0.0, h, 0.0)), roof_params))
	var mesh := ArrayMesh.new()
	for arrays in surfaces:
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	for surface in range(mesh.get_surface_count()):
		mesh.surface_set_material(surface, p_material)
	mesh.resource_name = String(p_recipe["name"])
	return mesh


## The multi-material kit building: the rectangle perimeter is split into
## one piece per surface (corner-spanning pieces merge when there are fewer
## than four materials), each surface with its own material, neighbors
## sharing a full-height coincident seam loop. Surface 0 also carries the
## roof. Returns {"mesh", "surfaces" (arrays per surface), "seams"}.
func _build_kit_mesh(p_recipe: Dictionary, p_materials: int, p_rng: RandomNumberGenerator) -> Dictionary:
	var w := float(p_recipe["w"])
	var d := float(p_recipe["d"])
	var h := float(p_recipe["h"])
	var height_subdiv := int(p_recipe["height_subdiv"])
	var perimeter_subdiv := int(p_recipe["perimeter_subdiv"])
	var params := {
		"cols": int(p_recipe["cols"]), "rows": int(p_recipe["rows"]),
		"inset": p_rng.randf_range(0.10, 0.16), "ledge": p_rng.randf_range(0.04, 0.07),
		"noise": 0.006, "phase": p_rng.randf_range(0.0, 6.28318),
	}
	# Pieces per side (front, east, back, west), then piece -> surface map.
	var counts: Array[int] = [1, 1, 1, 1]
	if p_materials >= 4:
		var remaining := p_materials - 4
		var order: Array[int] = [0, 2, 1, 3]
		var oi := 0
		while remaining > 0:
			counts[order[oi % 4]] += 1
			oi += 1
			remaining -= 1
	var piece_surface: Array[int] = []
	var piece_index := 0
	for side in range(4):
		for piece in range(counts[side]):
			piece_surface.append(piece_index % p_materials if p_materials < 4 else piece_index)
			piece_index += 1
	var side_lengths: Array[float] = [w, d, w, d]
	var perimeter := 2.0 * (w + d)
	var pieces: Array = []
	for side in range(4):
		var length := side_lengths[side]
		var side_subdiv := maxi(roundi(float(perimeter_subdiv) * length / perimeter), counts[side])
		for piece in range(counts[side]):
			var piece_width := length / float(counts[side])
			var piece_subdiv := maxi(side_subdiv / counts[side], 1)
			var center_along := -length * 0.5 + (float(piece) + 0.5) * piece_width
			var transform: Transform3D
			match side:
				0:
					transform = _facade_transform(0.0, Vector3(center_along, h * 0.5, -d * 0.5))
				1:
					transform = _facade_transform(-PI * 0.5, Vector3(w * 0.5, h * 0.5, center_along))
				2:
					transform = _facade_transform(PI, Vector3(-center_along, h * 0.5, d * 0.5))
				_:
					transform = _facade_transform(PI * 0.5, Vector3(-w * 0.5, h * 0.5, -center_along))
			var piece_params := params.duplicate()
			piece_params["cols"] = maxi(roundi(float(params["cols"]) * piece_width / w), 2)
			pieces.append(_facade_arrays(piece_width, h, Vector2i(piece_subdiv, height_subdiv), transform, piece_params))
	var roof: Vector2i = p_recipe["roof"]
	var roof_params := params.duplicate()
	roof_params["inset"] = -0.35
	roof_params["ledge"] = 0.0
	var roof_arrays := _facade_arrays(w, d, roof, Transform3D(Basis.IDENTITY, Vector3(0.0, h, 0.0)), roof_params)
	var surfaces: Array = []
	surfaces.resize(p_materials)
	for surface in range(p_materials):
		var parts: Array = []
		for piece in range(pieces.size()):
			if piece_surface[piece] == surface:
				parts.append(pieces[piece])
		if surface == 0:
			parts.append(roof_arrays)
		surfaces[surface] = parts[0] if parts.size() == 1 else _merge_arrays(parts)
	var mesh := ArrayMesh.new()
	for surface in range(p_materials):
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, surfaces[surface])
		mesh.surface_set_material(surface, _material(surface, "Kit"))
	mesh.resource_name = String(p_recipe["name"])
	return {"mesh": mesh, "surfaces": surfaces, "seams": _seam_check(surfaces)}


## A rooftop prop: two subdivided boxes and a vent cylinder in one surface.
func _build_prop_mesh(p_recipe: Dictionary, p_material: Material) -> ArrayMesh:
	var parts: Array = []
	var box1 := BoxMesh.new()
	box1.size = Vector3(float(p_recipe["w"]), float(p_recipe["h"]), float(p_recipe["d"]))
	box1.subdivide_width = 6
	box1.subdivide_height = 4
	box1.subdivide_depth = 6
	parts.append(_transformed_arrays(box1, Transform3D(Basis.IDENTITY, Vector3(0.0, float(p_recipe["h"]) * 0.5, 0.0))))
	var box2 := BoxMesh.new()
	box2.size = Vector3(1.0, 0.6, 1.0)
	box2.subdivide_width = 4
	box2.subdivide_height = 3
	box2.subdivide_depth = 4
	parts.append(_transformed_arrays(box2, Transform3D(Basis.IDENTITY, Vector3(0.4, float(p_recipe["h"]) + 0.3, 0.2))))
	var vent := CylinderMesh.new()
	vent.top_radius = 0.32
	vent.bottom_radius = 0.38
	vent.height = 0.9
	vent.radial_segments = 20
	vent.rings = 3
	parts.append(_transformed_arrays(vent, Transform3D(Basis.IDENTITY, Vector3(-0.6, float(p_recipe["h"]) + 0.45, -0.3))))
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, _merge_arrays(parts))
	mesh.surface_set_material(0, p_material)
	mesh.resource_name = String(p_recipe["name"])
	return mesh


func _transformed_arrays(p_primitive: PrimitiveMesh, p_transform: Transform3D) -> Array:
	var arrays := p_primitive.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	for i in range(verts.size()):
		verts[i] = p_transform * verts[i]
		normals[i] = (p_transform.basis * normals[i]).normalized()
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TANGENT] = null
	return arrays


# --- File output -------------------------------------------------------------

func _record(p_res_path: String) -> void:
	_outputs.append(p_res_path)


func _ensure_dir(p_res_dir: String) -> bool:
	var err := DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(p_res_dir))
	return _check(err == OK or err == ERR_ALREADY_EXISTS, "%s: cannot create directory (error %d)." % [p_res_dir, err])


func _sha256(p_res_path: String) -> String:
	var file := FileAccess.open(p_res_path, FileAccess.READ)
	if file == null:
		_fail("%s: cannot open for hashing." % p_res_path)
		return ""
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	while not file.eof_reached():
		context.update(file.get_buffer(1 << 20))
	file.close()
	return context.finish().hex_encode()


## Writes one kit mesh as a [code].glb[/code] scene (a Node3D root with one
## MeshInstance3D child), with no LODs in the source.
func _write_glb(p_mesh: ArrayMesh, p_name: String, p_res_path: String) -> bool:
	if not _ensure_dir(p_res_path.get_base_dir()):
		return false
	var root := Node3D.new()
	root.name = p_name
	var instance := MeshInstance3D.new()
	instance.name = p_name + "_mesh"
	instance.mesh = p_mesh
	root.add_child(instance)
	instance.owner = root
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var err := document.append_from_scene(root, state)
	if err == OK:
		err = document.write_to_filesystem(state, ProjectSettings.globalize_path(p_res_path))
	root.free()
	if not _check(err == OK, "%s: glTF write failed with error %d." % [p_res_path, err]):
		return false
	_record(p_res_path)
	print("dense_city: wrote " + p_res_path)
	return true


## Pre-seeds the [code].glb.import[/code] next to a kit mesh. The editor
## importer fills in the remap/deps sections on the first [code]--import[/code]
## and keeps these params. [code]meshes/generate_virtual_geometry[/code] is
## the #67 option name from the plan; until #67 lands the importer drops the
## unknown key on rewrite, so re-run this generator before a
## virtual-geometry import once #67 is in.
func _write_glb_import(p_glb_res_path: String, p_virtual: bool) -> bool:
	var text := "[remap]\n\nimporter=\"scene\"\nimporter_version=1\ntype=\"PackedScene\"\n\n[deps]\n\nsource_file=\"%s\"\n\n[params]\n\n" % p_glb_res_path
	if p_virtual:
		text += "meshes/generate_lods=false\nmeshes/create_shadow_meshes=false\nmeshes/generate_virtual_geometry=true\n"
	else:
		text += "meshes/generate_lods=true\nmeshes/create_shadow_meshes=true\nmeshes/generate_virtual_geometry=false\n"
	var path := p_glb_res_path + ".import"
	var err := Scaffold.write_text_chunked(path, text)
	if not _check(err == OK, "%s: write failed with error %d." % [path, err]):
		return false
	_record(path)
	return true


func _write_obj_import(p_obj_res_path: String) -> bool:
	var text := "[remap]\n\nimporter=\"wavefront_obj\"\nimporter_version=1\ntype=\"Mesh\"\n\n[deps]\n\nsource_file=\"%s\"\n\n[params]\n\ngenerate_lods=false\ngenerate_shadow_mesh=false\n" % p_obj_res_path
	var path := p_obj_res_path + ".import"
	var err := Scaffold.write_text_chunked(path, text)
	if not _check(err == OK, "%s: write failed with error %d." % [path, err]):
		return false
	_record(path)
	return true


# --- Placement ---------------------------------------------------------------
#
# One placement list backs every city variant: buildings on a shuffled lot
# grid (kept off the main street along +X at z = 0), then rooftop props on
# those buildings. Only seeded draws are used, in a fixed order, so the
# virtual and lod_baseline scenes carry identical transforms.

func _make_placements() -> Array:
	var rng := _rng(7)
	var lots: Array = []
	for gx in range(-LOT_GRID_RADIUS, LOT_GRID_RADIUS + 1):
		for gz in range(-LOT_GRID_RADIUS, LOT_GRID_RADIUS + 1):
			var center := Vector3(gx * LOT_SPACING, 0.0, gz * LOT_SPACING)
			if absf(center.z) < MAIN_STREET_HALF_WIDTH + LOT_SPACING * 0.5:
				continue
			lots.append(center)
	_shuffle(lots, rng)
	var assignment: Array = []
	for mesh_index in range(RECIPES.size()):
		if String(RECIPES[mesh_index]["kind"]) == "prop":
			continue
		for i in range(int(RECIPES[mesh_index]["instances"])):
			assignment.append(mesh_index)
	if not _check(assignment.size() <= lots.size(), "not enough lots (%d) for %d buildings." % [lots.size(), assignment.size()]):
		return []
	_shuffle(assignment, rng)
	var placements: Array = []
	var buildings: Array = []
	for i in range(assignment.size()):
		var mesh_index := int(assignment[i])
		var recipe := RECIPES[mesh_index]
		var lot: Vector3 = lots[i]
		var origin := lot + Vector3(rng.randf_range(-8.0, 8.0), 0.0, rng.randf_range(-8.0, 8.0))
		var yaw := PI * 0.5 * float(rng.randi_range(0, 3)) + rng.randf_range(-0.03, 0.03)
		var scale_xz := rng.randf_range(0.85, 1.35)
		var scale_y := rng.randf_range(0.75, 1.7)
		var basis := Basis(Vector3.UP, yaw).scaled(Vector3(scale_xz, scale_y, scale_xz))
		placements.append({"mesh": mesh_index, "transform": Transform3D(basis, origin)})
		buildings.append({"recipe": recipe, "basis": basis, "origin": origin, "yaw": yaw})
	var prop_rng := _rng(8)
	var prop_index := RECIPES.size() - 1
	var prop_count := int(RECIPES[prop_index]["instances"])
	for i in range(prop_count):
		var building: Dictionary = buildings[i % buildings.size()]
		var recipe: Dictionary = building["recipe"]
		var basis: Basis = building["basis"]
		var local := Vector3(
				prop_rng.randf_range(-1.0, 1.0) * (float(recipe["w"]) * 0.5 - 1.6),
				float(recipe["h"]),
				prop_rng.randf_range(-1.0, 1.0) * (float(recipe["d"]) * 0.5 - 1.6))
		var origin: Vector3 = building["origin"]
		var prop_yaw: float = building["yaw"] + prop_rng.randf_range(-0.4, 0.4)
		var prop_scale := prop_rng.randf_range(0.7, 1.5)
		var prop_basis := Basis(Vector3.UP, prop_yaw).scaled(Vector3.ONE * prop_scale)
		placements.append({"mesh": prop_index, "transform": Transform3D(prop_basis, origin + basis * local)})
	return placements


## SHA-256 over the placement transforms at 1e-6 precision: the compact
## proof, recorded per city variant in expected.json, that the baseline and
## virtual transform sets are identical.
func _transform_fingerprint(p_placements: Array) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	var lines := PackedStringArray()
	for placement_value in p_placements:
		var placement: Dictionary = placement_value
		var t: Transform3D = placement["transform"]
		lines.append("%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n" % [
			int(placement["mesh"]),
			t.basis.x.x, t.basis.x.y, t.basis.x.z,
			t.basis.y.x, t.basis.y.y, t.basis.y.z,
			t.basis.z.x, t.basis.z.y, t.basis.z.z,
			t.origin.x, t.origin.y, t.origin.z])
		if lines.size() >= 4096:
			context.update("".join(lines).to_utf8_buffer())
			lines.clear()
	if not lines.is_empty():
		context.update("".join(lines).to_utf8_buffer())
	return context.finish().hex_encode()


# --- City scene --------------------------------------------------------------

## Writes a city scene: the fixed rig (sky, sun with 4 PSSM splits, shadowed
## street lights, cameras, flight path, ground) plus one scene instance per
## placement under [code]Buildings[/code]. [param p_mesh_paths] maps recipe
## index to the [code].glb[/code] res path for this variant.
func _write_city_scene(p_path: String, p_scene_name: String, p_placements: Array, p_mesh_paths: Array, p_instanced_triangles: int) -> bool:
	var root := Node3D.new()
	root.name = p_scene_name
	root.set_meta("seed", _seed)
	root.set_meta("variant", _variant)
	root.set_meta("instance_count", p_placements.size())
	root.set_meta("instanced_triangles", p_instanced_triangles)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.shadow_enabled = true
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_4_SPLITS
	sun.directional_shadow_max_distance = 1600.0
	sun.light_energy = 1.2
	sun.transform = Transform3D(Basis.from_euler(Vector3(deg_to_rad(-48.0), deg_to_rad(-132.0), 0.0)), Vector3.ZERO)
	Scaffold.add_owned(root, sun, root)

	# Shadowed positional lights along the cam_main street (G5-1 faces).
	var street_x: Array[float] = [-450.0, -150.0, 150.0, 450.0]
	for i in range(street_x.size()):
		var omni := OmniLight3D.new()
		omni.name = "StreetOmni%d" % i
		omni.position = Vector3(street_x[i], 14.0, 12.0 if i % 2 == 0 else -12.0)
		omni.shadow_enabled = true
		omni.omni_range = 70.0
		omni.light_energy = 1.5
		Scaffold.add_owned(root, omni, root)
		var spot := SpotLight3D.new()
		spot.name = "StreetSpot%d" % i
		var spot_pos := Vector3(street_x[i] - 40.0, 16.0, -12.0 if i % 2 == 0 else 12.0)
		spot.transform = Transform3D(Basis.looking_at((Vector3(street_x[i] + 60.0, 0.0, 0.0) - spot_pos).normalized(), Vector3.UP), spot_pos)
		spot.shadow_enabled = true
		spot.spot_range = 120.0
		spot.spot_angle = 32.0
		spot.light_energy = 2.0
		Scaffold.add_owned(root, spot, root)

	var ground_mesh := PlaneMesh.new()
	ground_mesh.size = Vector2(4200.0, 4200.0)
	ground_mesh.material = Scaffold.make_constant_material("DenseCityGround", Color(0.16, 0.16, 0.17), 1.0)
	var ground := MeshInstance3D.new()
	ground.name = "Ground"
	ground.mesh = ground_mesh
	Scaffold.add_owned(root, ground, root)

	Scaffold.make_camera(root, root, "cam_main", Vector3(-260.0, 2.0, 0.0), Vector3(200.0, 14.0, 0.0), 3000.0, 70.0, true)
	Scaffold.make_camera(root, root, "cam_aerial", Vector3(-560.0, 430.0, -560.0), Vector3(0.0, 0.0, 0.0), 5000.0, 60.0, false)

	var flight := Path3D.new()
	flight.name = "FlightPath"
	var curve := Curve3D.new()
	for point in [Vector3(-700.0, 22.0, 0.0), Vector3(-260.0, 3.0, 0.0), Vector3(120.0, 18.0, 8.0),
			Vector3(520.0, 55.0, -30.0), Vector3(760.0, 140.0, -260.0), Vector3(300.0, 260.0, -620.0),
			Vector3(-320.0, 240.0, -540.0), Vector3(-720.0, 90.0, -180.0)]:
		curve.add_point(point)
	flight.curve = curve
	Scaffold.add_owned(root, flight, root)

	var buildings := Node3D.new()
	buildings.name = "Buildings"
	Scaffold.add_owned(root, buildings, root)
	for i in range(p_placements.size()):
		var placement: Dictionary = p_placements[i]
		var placeholder := Node3D.new()
		placeholder.name = "Inst%05d_m%d" % [i, int(placement["mesh"])]
		placeholder.transform = placement["transform"]
		Scaffold.add_owned(buildings, placeholder, root)

	var err := Scaffold.save_scene(root, p_path)
	root.free()
	if not _check(err == OK, "%s: scene save failed with error %d." % [p_path, err]):
		return false

	# Rewrite the placeholders as instances of the imported kit scenes.
	var text := FileAccess.get_file_as_string(p_path)
	var ext_lines := PackedStringArray()
	for mesh_index in range(p_mesh_paths.size()):
		ext_lines.append("[ext_resource type=\"PackedScene\" path=\"%s\" id=\"%d_dc%d\"]" % [String(p_mesh_paths[mesh_index]), mesh_index + 1, mesh_index])
	var header_end := text.find("\n")
	text = text.substr(0, header_end + 1) + "\n" + "\n".join(ext_lines) + text.substr(header_end)
	for mesh_index in range(p_mesh_paths.size()):
		var per_mesh := RegEx.new()
		per_mesh.compile("(?m)^(\\[node name=\"Inst\\d+_m%d\" type=\"Node3D\" parent=\"Buildings\")\\]$" % mesh_index)
		text = per_mesh.sub(text, "$1 instance=ExtResource(\"%d_dc%d\")]" % [mesh_index + 1, mesh_index], true)
	if not _check(text.count("instance=ExtResource") == p_placements.size(),
			"%s: rewrote %d of %d instance nodes." % [p_path, text.count("instance=ExtResource"), p_placements.size()]):
		return false
	err = Scaffold.write_text_chunked(p_path, text)
	if not _check(err == OK, "%s: scene rewrite failed with error %d." % [p_path, err]):
		return false
	_record(p_path)
	print("dense_city: wrote " + p_path)
	return true


# --- Kit assembly and counters -----------------------------------------------

## Builds every recipe mesh in recipe order. Returns {"meshes", "stats"
## (per-mesh dictionaries), "kit_seams"}.
func _build_kit_meshes(p_kit_materials: int) -> Dictionary:
	var meshes: Array = []
	var stats: Array = []
	var kit_seams := {}
	for index in range(RECIPES.size()):
		var recipe := RECIPES[index]
		var rng := _rng(100 + index)
		var mesh: ArrayMesh
		match String(recipe["kind"]):
			"building":
				mesh = _build_building_mesh(recipe, _material(index), rng)
			"kit":
				var built := _build_kit_mesh(recipe, p_kit_materials, rng)
				mesh = built["mesh"]
				kit_seams = built["seams"]
			_:
				mesh = _build_prop_mesh(recipe, _material(index))
		meshes.append(mesh)
		stats.append(_mesh_stats(mesh))
		print("dense_city: mesh %s triangles=%d vertices=%d surfaces=%d" % [
			String(recipe["name"]), int(stats[index]["triangles"]), int(stats[index]["vertices"]), int(stats[index]["surfaces"])])
	return {"meshes": meshes, "stats": stats, "kit_seams": kit_seams}


func _write_kit(p_dir_name: String, p_virtual: bool, p_meshes: Array) -> Array:
	var paths: Array = []
	for index in range(p_meshes.size()):
		var name := String(RECIPES[index]["name"])
		var path := "%s/%s/%s.glb" % [_out, p_dir_name, name]
		if not _write_glb(p_meshes[index], name, path) or not _write_glb_import(path, p_virtual):
			return []
		paths.append(path)
	return paths


## Scale counters for one city variant, shared by the assertions, the
## console report and expected.json. Unique bytes count position (12) +
## normal (12) + UV (8) per vertex plus 4 bytes per index, over the unique
## kit meshes; reachable clusters are the finest-level lower bound
## sum over instances of ceil(mesh triangles / 128) from the issue.
func _city_counters(p_stats: Array, p_placements: Array) -> Dictionary:
	var unique_triangles := 0
	var unique_vertices := 0
	for stat_value in p_stats:
		var stat: Dictionary = stat_value
		unique_triangles += int(stat["triangles"])
		unique_vertices += int(stat["vertices"])
	var unique_bytes := unique_vertices * 32 + unique_triangles * 12
	var instanced_triangles := 0
	var clusters := 0
	var per_mesh_instances: Array[int] = []
	per_mesh_instances.resize(p_stats.size())
	for placement_value in p_placements:
		var placement: Dictionary = placement_value
		var mesh := int(placement["mesh"])
		var tris := int(p_stats[mesh]["triangles"])
		instanced_triangles += tris
		clusters += (tris + 127) / 128
		per_mesh_instances[mesh] += 1
	var meshes: Array = []
	for index in range(p_stats.size()):
		meshes.append({
			"name": String(RECIPES[index]["name"]),
			"triangles": int(p_stats[index]["triangles"]),
			"vertices": int(p_stats[index]["vertices"]),
			"surfaces": int(p_stats[index]["surfaces"]),
			"aabb": p_stats[index]["aabb"],
			"instances": per_mesh_instances[index],
		})
	return {
		"instance_count": p_placements.size(),
		"unique_triangles": unique_triangles,
		"unique_vertices": unique_vertices,
		"unique_bytes": unique_bytes,
		"working_set_ratio": float(unique_bytes) / float(_cfg["geometry_cache_budget_bytes"]),
		"instanced_triangles": instanced_triangles,
		"reachable_clusters_lower_bound": clusters,
		"authored_lods": 0,
		"meshes": meshes,
	}


func _print_counters(p_counters: Dictionary) -> void:
	print("dense_city: instances=%d unique_triangles=%d unique_vertices=%d unique_bytes=%d ratio=%.3f instanced_triangles=%d clusters>=%d" % [
		int(p_counters["instance_count"]), int(p_counters["unique_triangles"]), int(p_counters["unique_vertices"]),
		int(p_counters["unique_bytes"]), float(p_counters["working_set_ratio"]),
		int(p_counters["instanced_triangles"]), int(p_counters["reachable_clusters_lower_bound"])])


func _variant_virtual() -> Dictionary:
	var built := _build_kit_meshes(int(_cfg["kit_materials"]))
	var paths := _write_kit("kit", true, built["meshes"])
	if paths.is_empty():
		return {}
	var placements := _make_placements()
	var counters := _city_counters(built["stats"], placements)
	_print_counters(counters)
	_check(int(counters["instance_count"]) >= int(_cfg["instances_min"]),
			"virtual: %d instances, expected at least %d." % [int(counters["instance_count"]), int(_cfg["instances_min"])])
	_check(int(counters["unique_triangles"]) >= int(_cfg["unique_triangles_min"]),
			"virtual: %d unique triangles, expected at least %d." % [int(counters["unique_triangles"]), int(_cfg["unique_triangles_min"])])
	_check(int(counters["reachable_clusters_lower_bound"]) >= int(_cfg["reachable_clusters_min"]),
			"virtual: %d reachable clusters, expected at least %d." % [int(counters["reachable_clusters_lower_bound"]), int(_cfg["reachable_clusters_min"])])
	_check(float(counters["working_set_ratio"]) >= float(_cfg["working_set_ratio_min"]),
			"virtual: working set ratio %.3f, expected at least %.3f." % [float(counters["working_set_ratio"]), float(_cfg["working_set_ratio_min"])])
	var kit_stats: Dictionary = built["stats"][KIT_RECIPE_INDEX]
	var seams: Dictionary = built["kit_seams"]
	_check(int(kit_stats["surfaces"]) >= int(_cfg["kit_materials"]),
			"virtual: kit has %d surfaces, expected at least %d." % [int(kit_stats["surfaces"]), int(_cfg["kit_materials"])])
	_check(int(kit_stats["triangles"]) >= int(_cfg["kit_triangles_min"]),
			"virtual: kit has %d triangles, expected at least %d." % [int(kit_stats["triangles"]), int(_cfg["kit_triangles_min"])])
	_check(int(seams["surfaces_with_seam"]) == int(seams["surfaces"]),
			"virtual: kit seams on %d of %d surfaces." % [int(seams["surfaces_with_seam"]), int(seams["surfaces"])])
	if not _write_city_scene(_out + "/dense_city.tscn", "DenseCity", placements, paths, int(counters["instanced_triangles"])):
		return {}
	counters["scene"] = "dense_city.tscn"
	counters["kit_seams"] = seams
	counters["transform_fingerprint"] = _transform_fingerprint(placements)
	return counters


func _variant_lod_baseline() -> Dictionary:
	var built := _build_kit_meshes(int(_cfg["kit_materials"]))
	var paths := _write_kit("kit_lod", false, built["meshes"])
	if paths.is_empty():
		return {}
	var placements := _make_placements()
	var counters := _city_counters(built["stats"], placements)
	_print_counters(counters)
	if not _write_city_scene(_out + "/dense_city_lod_baseline.tscn", "DenseCityLodBaseline", placements, paths, int(counters["instanced_triangles"])):
		return {}
	counters["scene"] = "dense_city_lod_baseline.tscn"
	counters["kit_seams"] = built["kit_seams"]
	counters["transform_fingerprint"] = _transform_fingerprint(placements)
	counters["import"] = {"generate_lods": true, "create_shadow_meshes": true, "generate_virtual_geometry": false}
	return counters


func _variant_sweep() -> Dictionary:
	var target := int(_cfg[_variant])
	var built := _build_kit_meshes(int(_cfg["kit_materials"]))
	var paths := _write_kit("kit", true, built["meshes"])
	if paths.is_empty():
		return {}
	var base := _make_placements()
	var stats: Array = built["stats"]
	var base_triangles := 0
	for placement_value in base:
		base_triangles += int(stats[int(placement_value["mesh"])]["triangles"])
	# Tile the city until the pool can reach the target, then take a greedy
	# seeded subset. Rooftop props are the fine grain that lands the total
	# inside 5% for every target.
	var tiles: Array[Vector2i] = [Vector2i(0, 0), Vector2i(1, 0), Vector2i(-1, 0), Vector2i(0, 1), Vector2i(0, -1),
		Vector2i(1, 1), Vector2i(-1, -1), Vector2i(1, -1), Vector2i(-1, 1), Vector2i(2, 0), Vector2i(-2, 0), Vector2i(0, 2)]
	var pool: Array = []
	var tile_count := 1
	while tile_count < tiles.size() and base_triangles * tile_count < int(float(target) * 1.05):
		tile_count += 1
	for tile in range(tile_count):
		var offset := Vector3(float(tiles[tile].x) * TILE_SPAN, 0.0, float(tiles[tile].y) * TILE_SPAN)
		for placement_value in base:
			var placement: Dictionary = placement_value
			var t: Transform3D = placement["transform"]
			pool.append({"mesh": int(placement["mesh"]), "transform": Transform3D(t.basis, t.origin + offset)})
	_shuffle(pool, _rng(9))
	var selected: Array = []
	var total := 0
	for placement_value in pool:
		var tris := int(stats[int(placement_value["mesh"])]["triangles"])
		if total + tris <= target:
			selected.append(placement_value)
			total += tris
	var counters := _city_counters(stats, selected)
	_print_counters(counters)
	var ratio := float(total) / float(target)
	_check(absf(ratio - 1.0) <= 0.05,
			"%s: instanced triangles %d vs target %d (%.4f), outside 5%%." % [_variant, total, target, ratio])
	var scene_name := "DenseCity_" + _variant
	if not _write_city_scene("%s/dense_city_%s.tscn" % [_out, _variant], scene_name, selected, paths, total):
		return {}
	counters["scene"] = "dense_city_%s.tscn" % _variant
	counters["target_instanced_triangles"] = target
	counters["transform_fingerprint"] = _transform_fingerprint(selected)
	counters.erase("meshes")
	return counters


func _variant_kit() -> Dictionary:
	var recipe := RECIPES[KIT_RECIPE_INDEX]
	var built := _build_kit_mesh(recipe, _materials, _rng(100 + KIT_RECIPE_INDEX))
	var mesh: ArrayMesh = built["mesh"]
	var stats := _mesh_stats(mesh)
	var seams: Dictionary = built["seams"]
	print("dense_city: kit materials=%d triangles=%d surfaces=%d seams=%d/%d" % [
		_materials, int(stats["triangles"]), int(stats["surfaces"]),
		int(seams["surfaces_with_seam"]), int(seams["surfaces"])])
	_check(int(stats["surfaces"]) == _materials,
			"kit: %d surfaces, expected %d (one material each)." % [int(stats["surfaces"]), _materials])
	_check(int(stats["triangles"]) >= int(_cfg["kit_triangles_min"]),
			"kit: %d triangles, expected at least %d." % [int(stats["triangles"]), int(_cfg["kit_triangles_min"])])
	if _materials >= 2:
		_check(int(seams["surfaces_with_seam"]) == int(seams["surfaces"]),
				"kit: seams on %d of %d surfaces." % [int(seams["surfaces_with_seam"]), int(seams["surfaces"])])
	var path := _out + "/kit_asset/kit_facade.glb"
	if not _write_glb(mesh, "kit_facade", path) or not _write_glb_import(path, true):
		return {}
	return {
		"materials": _materials,
		"triangles": int(stats["triangles"]),
		"vertices": int(stats["vertices"]),
		"surfaces": int(stats["surfaces"]),
		"aabb": stats["aabb"],
		"seams": seams,
		"asset": "kit_asset/kit_facade.glb",
	}


func _variant_stress() -> Dictionary:
	var rng := _rng(200)
	var phase := rng.randf_range(0.0, 6.28318)
	var plane := PlaneMesh.new()
	plane.size = Vector2(220.0, 220.0)
	plane.subdivide_width = STRESS_SUBDIV
	plane.subdivide_depth = STRESS_SUBDIV
	var arrays := plane.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	for i in range(verts.size()):
		var v := verts[i]
		v.y += 1.8 * sin(v.x * 0.11 + phase) * cos(v.z * 0.13) + 0.4 * sin(v.x * 0.53) * sin(v.z * 0.47 + phase)
		verts[i] = v
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TANGENT] = null
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, _material(0, "Stress"))
	mesh.resource_name = "stress_10m"
	var stats := _surface_stats(arrays)
	var target := int(_cfg["stress_triangles"])
	var triangles := int(stats["triangles"])
	print("dense_city: stress triangles=%d vertices=%d" % [triangles, int(stats["vertices"])])
	_check(mesh.get_surface_count() == 1, "stress: %d surfaces, expected 1." % mesh.get_surface_count())
	_check(absf(float(triangles) / float(target) - 1.0) <= 0.01,
			"stress: %d triangles vs %d, outside 1%%." % [triangles, target])
	var glb_path := _out + "/stress/stress_10m.glb"
	if not _write_glb(mesh, "stress_10m", glb_path) or not _write_glb_import(glb_path, true):
		return {}
	var obj_path := _out + "/stress/stress_10m.obj"
	if not _write_obj(arrays, obj_path) or not _write_obj_import(obj_path):
		return {}
	var aabb := mesh.get_aabb()
	return {
		"triangles": triangles,
		"vertices": int(stats["vertices"]),
		"surfaces": 1,
		"aabb": [aabb.position.x, aabb.position.y, aabb.position.z, aabb.size.x, aabb.size.y, aabb.size.z],
		"assets": ["stress/stress_10m.glb", "stress/stress_10m.obj"],
	}


## Streams one surface as Wavefront OBJ in fixed-size line chunks so the
## 10M-triangle stress asset never builds one giant string.
func _write_obj(p_arrays: Array, p_res_path: String) -> bool:
	if not _ensure_dir(p_res_path.get_base_dir()):
		return false
	var file := FileAccess.open(p_res_path, FileAccess.WRITE)
	if not _check(file != null, "%s: cannot open for writing (error %d)." % [p_res_path, FileAccess.get_open_error()]):
		return false
	var verts: PackedVector3Array = p_arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = p_arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = p_arrays[Mesh.ARRAY_INDEX]
	var lines := PackedStringArray()
	lines.append("# Dense City Blocks stress_10m, seed %d. Generated by dense_city_generator.gd; do not edit.\n" % _seed)
	lines.append("o stress_10m\n")
	for v in verts:
		lines.append("v %.4f %.4f %.4f\n" % [v.x, v.y, v.z])
		if lines.size() >= 8192:
			file.store_buffer("".join(lines).to_utf8_buffer())
			lines.clear()
	for n in normals:
		lines.append("vn %.4f %.4f %.4f\n" % [n.x, n.y, n.z])
		if lines.size() >= 8192:
			file.store_buffer("".join(lines).to_utf8_buffer())
			lines.clear()
	var i := 0
	while i < indices.size():
		var a := indices[i] + 1
		var b := indices[i + 1] + 1
		var c := indices[i + 2] + 1
		lines.append("f %d//%d %d//%d %d//%d\n" % [a, a, b, b, c, c])
		i += 3
		if lines.size() >= 8192:
			file.store_buffer("".join(lines).to_utf8_buffer())
			lines.clear()
	if not lines.is_empty():
		file.store_buffer("".join(lines).to_utf8_buffer())
	file.close()
	_record(p_res_path)
	print("dense_city: wrote " + p_res_path)
	return true


# --- expected.json -----------------------------------------------------------
#
# Each variant keeps its full entry as a fragment under the output folder;
# the tracked expected.json is the deterministic assembly of the fragments
# present. Fragments are spliced as text so integers survive re-assembly
# (a JSON re-parse would turn them into floats).

func _write_expected(p_entry: Dictionary) -> void:
	var outputs := {}
	for path in _outputs:
		var relative := path.trim_prefix(_out + "/") if path.begins_with(_out + "/") else path.trim_prefix("res://")
		outputs[relative] = _sha256(path)
	p_entry["outputs"] = outputs
	p_entry["seed"] = _seed
	var fragment_dir := _out + "/expected_fragments"
	if not _ensure_dir(fragment_dir):
		return
	var fragment_path := "%s/%s.json" % [fragment_dir, _variant]
	var err := Scaffold.write_text_chunked(fragment_path, Scaffold.to_json(p_entry) + "\n")
	if not _check(err == OK, "%s: write failed with error %d." % [fragment_path, err]):
		return
	var names: Array[String] = []
	for variant in VARIANTS:
		if FileAccess.file_exists("%s/%s.json" % [fragment_dir, variant]):
			names.append(variant)
	names.sort()
	var parts := PackedStringArray()
	for name in names:
		var fragment := FileAccess.get_file_as_string("%s/%s.json" % [fragment_dir, name]).strip_edges()
		parts.append("\t\t\"%s\": %s" % [name, fragment])
	var text := "{\n\t\"geometry_cache_budget_bytes\": %d,\n\t\"seed\": %d,\n\t\"variants\": {\n%s\n\t},\n\t\"version\": 1\n}\n" % [
		int(_cfg["geometry_cache_budget_bytes"]), _seed, ",\n".join(parts)]
	err = Scaffold.write_text_chunked(EXPECTED_PATH, text)
	_check(err == OK, "%s: write failed with error %d." % [EXPECTED_PATH, err])
	print("dense_city: wrote " + EXPECTED_PATH)
