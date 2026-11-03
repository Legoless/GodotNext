extends RefCounted
## Runtime procedural mesh contract (#181, gate GS-6): a 20,000-triangle
## [ArrayMesh] (a 100 x 100 segment torus, 2.0 m across) built at
## t = 1 s and moved at t = 3 s. Time is [code]frame / 60[/code], never
## wall-clock time, so a [code]--fixed-fps 60[/code] capture is a pure
## function of the frame index and [code]--hold-frame=k[/code] samples
## the same state as frame k of a moving run. The mesh is built in code
## because GS-6 measures how fast a mesh with no baked SDF enters the
## global SDF (30 frames at most).

const FPS := 60.0
const BUILD_FRAME := 60
const MOVE_FRAME := 180
const END_FRAME := 240
const TUBULAR_SEGMENTS := 100
const RADIAL_SEGMENTS := 100
const MAJOR_RADIUS := 0.65
const MINOR_RADIUS := 0.35
const POSITION_A := Vector3(-1.8, 1.1, 0.0)
const POSITION_B := Vector3(1.8, 1.1, 0.0)


static func triangle_count() -> int:
	return TUBULAR_SEGMENTS * RADIAL_SEGMENTS * 2


static func is_built(p_frame: int) -> bool:
	return p_frame >= BUILD_FRAME


static func position_for_frame(p_frame: int) -> Vector3:
	return POSITION_B if p_frame >= MOVE_FRAME else POSITION_A


static func contract_error() -> String:
	if triangle_count() != 20000:
		return "runtime torus has %d triangles, expected 20000" % triangle_count()
	if not is_equal_approx(float(BUILD_FRAME) / FPS, 1.0):
		return "build frame is not t = 1 s"
	if not is_equal_approx(float(MOVE_FRAME) / FPS, 3.0):
		return "move frame is not t = 3 s"
	if is_built(BUILD_FRAME - 1):
		return "mesh exists before the build frame"
	if not is_built(BUILD_FRAME):
		return "mesh is missing at the build frame"
	if position_for_frame(MOVE_FRAME - 1) != POSITION_A:
		return "mesh is not at position A before the move"
	if position_for_frame(MOVE_FRAME) != POSITION_B:
		return "mesh is not at position B at the move frame"
	if position_for_frame(END_FRAME) != POSITION_B:
		return "mesh does not hold position B"
	var diameter := 2.0 * (MAJOR_RADIUS + MINOR_RADIUS)
	if not is_equal_approx(diameter, 2.0):
		return "runtime mesh is %0.3f m across, expected 2 m" % diameter
	return ""


## Closed torus with analytic normals. Triangles are indexed clockwise
## seen from outside (Godot's front-face winding): for this
## parametrization [code](b - a).cross(d - a)[/code] points inward, so
## the plain grid order [code](a, b, c)[/code] is already clockwise
## from outside. Verified against a capture (the reversed order
## renders the tube inside-out).
static func build_mesh(p_material: Material) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	vertices.resize(TUBULAR_SEGMENTS * RADIAL_SEGMENTS)
	normals.resize(TUBULAR_SEGMENTS * RADIAL_SEGMENTS)
	for i in TUBULAR_SEGMENTS:
		var u := TAU * float(i) / float(TUBULAR_SEGMENTS)
		var center := Vector3(cos(u) * MAJOR_RADIUS, 0.0, sin(u) * MAJOR_RADIUS)
		for j in RADIAL_SEGMENTS:
			var v := TAU * float(j) / float(RADIAL_SEGMENTS)
			var normal := Vector3(cos(u) * cos(v), sin(v), sin(u) * cos(v))
			var index := i * RADIAL_SEGMENTS + j
			vertices[index] = center + normal * MINOR_RADIUS
			normals[index] = normal
	var indices := PackedInt32Array()
	indices.resize(triangle_count() * 3)
	var cursor := 0
	for i in TUBULAR_SEGMENTS:
		var ni := (i + 1) % TUBULAR_SEGMENTS
		for j in RADIAL_SEGMENTS:
			var nj := (j + 1) % RADIAL_SEGMENTS
			var a := i * RADIAL_SEGMENTS + j
			var b := ni * RADIAL_SEGMENTS + j
			var c := ni * RADIAL_SEGMENTS + nj
			var d := i * RADIAL_SEGMENTS + nj
			# (b - a) x (d - a) points inward for this grid, so
			# (a, b, c) and (a, c, d) are clockwise from outside.
			indices[cursor] = a
			indices[cursor + 1] = b
			indices[cursor + 2] = c
			indices[cursor + 3] = a
			indices[cursor + 4] = c
			indices[cursor + 5] = d
			cursor += 6
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	if p_material != null:
		mesh.surface_set_material(0, p_material)
	return mesh
