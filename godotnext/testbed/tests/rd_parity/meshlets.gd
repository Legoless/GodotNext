extends Node

## Meshlet fixture for task and mesh shaders (#178 step 8; plan §21.1, GR-3).
##
## On a local RenderingDevice, a flat grid of about 1M triangles (--patches=N per side, default 102: 714 x 714
## quads) is split into meshlets of at most 64 vertices and 124 triangles (8 x 8 vertices, 98 triangles each). A
## perspective camera sees part of the grid. The task shader tests each meshlet's bounding sphere against the
## camera frustum and hands the visible ones to the mesh shader, which writes their vertices and triangles from
## the meshlet buffers. The same buffers feed the vertex-pulling vertex-shader path, which draws every triangle
## (the frustum clips the ones the task shader culls).
##
## Each path renders into two targets: the triangle's global id plus 1 (R32_UINT), and the number of fragments per
## pixel (R16F, additive). The grid is flat and seen from one side, so no pixel may get two fragments.
##
## Checks:
## - mesh_direct: draw_list_draw_mesh_tasks() covers part of the image, with at most one fragment per pixel.
## - culling: the task shader's count of visible meshlets matches a CPU count (the same plane test), and is
##   neither 0 nor every meshlet; and no meshlet the vertex path drew any pixel of was culled (the task shader
##   flags each meshlet it keeps, and the vertex path's ids name the meshlets on screen).
## - vertex_path: the vertex-pulling path has at most one fragment per pixel. Its ids are compared with the mesh
##   path's: no pixel may be covered by one path only (a hole); the pixels where the two wrote different
##   triangles are reported. The FLIP comparison is a PQ line (GR-3).
## - indirect: draw_list_draw_mesh_tasks_indirect() with packed (12-byte) and 32-byte records, also 3 records of
##   which 2 are empty, gives the direct image byte for byte (SUPPORTS_MESH_SHADER_INDIRECT).
## - indirect_count: draw_list_draw_mesh_tasks_indirect_count() with a count a compute pass writes on the GPU, with
##   16- and 32-byte records and counts 1, 3 and 0, gives the direct image (or an empty one for 0) byte for byte;
##   the records past the count are poison that would draw again (SUPPORTS_DRAW_INDIRECT_COUNT too).
##
## Without SUPPORTS_MESH_SHADER (Direct3D 12 until its compiler translates the stages) the vertex path still runs
## and the mesh checks read "unsupported". It prints one "GODOTNEXT_RESULT <json>" line and exits 0 on pass, 1 on
## failure. "digests" holds the SHA-256 of each case's read-back, which runs on different drivers can compare.
##
## User arguments: --patches=N, and --require=<checks> (a comma-separated list of mesh, indirect and
## indirect_count) to fail the run when one of them is "unsupported".
##
## The run also reports the eight mesh task group-count limits (LIMIT_MAX_TASK_WORKGROUP_* and
## LIMIT_MAX_MESH_WORKGROUP_*): each must be more than 0 with SUPPORTS_MESH_SHADER, and 0 without it.
##
## --negative runs none of the above. It makes the calls RenderingDevice must refuse, and checks that each logs
## exactly one error with the expected message (an engine Logger counts them) and that nothing is drawn: a render
## pipeline with a mesh shader and a vertex format, one for a multiview framebuffer format (with
## SUPPORTS_MULTIVIEW), draw_list_draw() with a mesh pipeline, draw_list_draw_mesh_tasks() with more task
## workgroups than LIMIT_MAX_TASK_WORKGROUP_COUNT_X (unless that limit is the most a draw can ask for), one whose
## counts each fit their axis but exceed LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT together (when such counts exist), and
## draw_list_draw_mesh_tasks() with a vertex pipeline.

const SceneArgs = preload("res://scenes/scene_args.gd")

const DEFAULT_PATCHES := 102
# Quads per meshlet side: 8 x 8 vertices and 98 triangles per meshlet.
const PATCH := 7
const MAX_VERTICES := 64
const MAX_TRIANGLES := 124
const TASK_GROUP := 32
const WIDTH := 768
const HEIGHT := 768
# Words read as a record, which draw 7 task workgroups again.
const POISON := 7
const RECORDS_OFFSET := 64
const COUNT_OFFSET := 12
const REQUIRE_CHECKS: Array[String] = ["mesh", "indirect", "indirect_count"]
const MESH_LIMITS := {
	"task_count_x": RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_X,
	"task_count_y": RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_Y,
	"task_count_z": RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_Z,
	"task_total": RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT,
	"mesh_count_x": RenderingDevice.LIMIT_MAX_MESH_WORKGROUP_COUNT_X,
	"mesh_count_y": RenderingDevice.LIMIT_MAX_MESH_WORKGROUP_COUNT_Y,
	"mesh_count_z": RenderingDevice.LIMIT_MAX_MESH_WORKGROUP_COUNT_Z,
	"mesh_total": RenderingDevice.LIMIT_MAX_MESH_WORKGROUP_TOTAL_COUNT,
}
# The most a draw can ask for in one dimension (uint32).
const MAX_GROUP_COUNT := 0xFFFFFFFF

const COMMON := """
struct Meshlet {
	uvec4 ranges; // First vertex index, vertex count, first triangle, triangle count.
	vec4 sphere; // Bounding sphere: center and radius.
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Positions {
	vec4 data[];
}
positions;

layout(set = 0, binding = 1, std430) restrict readonly buffer Meshlets {
	Meshlet data[];
}
meshlets;

layout(set = 0, binding = 2, std430) restrict readonly buffer MeshletVertices {
	uint data[];
}
meshlet_vertices;

layout(set = 0, binding = 3, std430) restrict readonly buffer MeshletTriangles {
	uint data[]; // Three 8-bit local vertex indices per triangle.
}
meshlet_triangles;

layout(set = 0, binding = 4, std140) uniform Frame {
	mat4 view_projection;
	vec4 planes[6];
	uvec4 counts; // Meshlets, then MAX_TRIANGLES.
}
frame;
"""

const TASK_SOURCE := """#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = %d, local_size_y = 1, local_size_z = 1) in;
%s
layout(set = 0, binding = 5, std430) restrict buffer Stats {
	uint visible;
	uint tested;
	uint pad0;
	uint pad1;
	uint kept[]; // 1 for each meshlet this shader hands to the mesh shader.
}
stats;

struct Payload {
	uint meshlets[%d];
};
taskPayloadSharedEXT Payload payload;

shared uint visible_count;

void main() {
	if (gl_LocalInvocationIndex == 0u) {
		visible_count = 0u;
	}
	barrier();

	uint index = gl_WorkGroupID.x * gl_WorkGroupSize.x + gl_LocalInvocationIndex;
	bool visible = false;
	if (index < frame.counts.x) {
		vec4 sphere = meshlets.data[index].sphere;
		visible = true;
		for (int i = 0; i < 6; i++) {
			if (dot(frame.planes[i].xyz, sphere.xyz) + frame.planes[i].w < -sphere.w) {
				visible = false;
			}
		}
	}
	if (visible) {
		payload.meshlets[atomicAdd(visible_count, 1u)] = index;
	}
	if (index < frame.counts.x) {
		stats.kept[index] = visible ? 1u : 0u;
	}
	barrier();

	if (gl_LocalInvocationIndex == 0u) {
		atomicAdd(stats.visible, visible_count);
		atomicAdd(stats.tested, min(frame.counts.x - gl_WorkGroupID.x * gl_WorkGroupSize.x, gl_WorkGroupSize.x));
	}
	EmitMeshTasksEXT(visible_count, 1u, 1u);
}
"""

const MESH_SOURCE := """#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = %d, max_primitives = %d) out;
%s
struct Payload {
	uint meshlets[%d];
};
taskPayloadSharedEXT Payload payload;

layout(location = 0) perprimitiveEXT out uint primitive_out[];

void main() {
	uint meshlet = payload.meshlets[gl_WorkGroupID.x];
	uvec4 ranges = meshlets.data[meshlet].ranges;
	SetMeshOutputsEXT(ranges.y, ranges.w);
	for (uint v = gl_LocalInvocationIndex; v < ranges.y; v += 32u) {
		uint vertex = meshlet_vertices.data[ranges.x + v];
		gl_MeshVerticesEXT[v].gl_Position = frame.view_projection * vec4(positions.data[vertex].xyz, 1.0);
	}
	for (uint t = gl_LocalInvocationIndex; t < ranges.w; t += 32u) {
		uint packed = meshlet_triangles.data[ranges.z + t];
		gl_PrimitiveTriangleIndicesEXT[t] = uvec3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu);
		// The id of the vertex path: the triangle's index in the draw (MAX_TRIANGLES per meshlet).
		primitive_out[t] = meshlet * frame.counts.y + t;
	}
}
"""

const MESH_FRAGMENT_SOURCE := """#version 460
#extension GL_EXT_mesh_shader : require

layout(location = 0) perprimitiveEXT flat in uint primitive_in;

layout(location = 0) out uint frag_id;
layout(location = 1) out float frag_count;

void main() {
	frag_id = primitive_in + 1u;
	frag_count = 1.0;
}
"""

# MAX_TRIANGLES triangles per meshlet, three vertices each; the triangles a meshlet does not have are degenerate.
const VERTEX_SOURCE := """#version 450
%s
layout(location = 0) flat out uint primitive_out;

void main() {
	uint triangle = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) %% 3u;
	uint meshlet = triangle / frame.counts.y;
	uint t = triangle %% frame.counts.y;
	uvec4 ranges = meshlets.data[meshlet].ranges;
	primitive_out = triangle;
	if (t >= ranges.w) {
		gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	uint local = (meshlet_triangles.data[ranges.z + t] >> (8u * corner)) & 0xffu;
	uint vertex = meshlet_vertices.data[ranges.x + local];
	gl_Position = frame.view_projection * vec4(positions.data[vertex].xyz, 1.0);
}
"""

const VERTEX_FRAGMENT_SOURCE := """#version 450

layout(location = 0) flat in uint primitive_in;

layout(location = 0) out uint frag_id;
layout(location = 1) out float frag_count;

void main() {
	frag_id = primitive_in + 1u;
	frag_count = 1.0;
}
"""

const COUNT_SOURCE := """#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict writeonly buffer Count {
	uint data[];
}
count_buffer;

layout(push_constant, std430) uniform Params {
	uint count;
	uint poison;
	uint pad0;
	uint pad1;
}
params;

void main() {
	count_buffer.data[0] = params.poison;
	count_buffer.data[1] = params.poison;
	count_buffer.data[2] = params.poison;
	count_buffer.data[3] = params.count;
}
"""

var _failures: PackedStringArray = PackedStringArray()


# The errors the engine logs while it is added (--negative), so each refused call checks its own error.
class ErrorLog extends Logger:
	var _mutex := Mutex.new()
	var _messages := PackedStringArray()

	func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String, _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		if error_type != ERROR_TYPE_ERROR:
			return
		_mutex.lock()
		_messages.append(code if rationale.is_empty() else rationale)
		_mutex.unlock()

	# The errors logged since the last call.
	func take() -> PackedStringArray:
		_mutex.lock()
		var messages := _messages
		_messages = PackedStringArray()
		_mutex.unlock()
		return messages


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var patches := clampi(SceneArgs.int_value("--patches", DEFAULT_PATCHES), 2, 256)
	var params := {
		"patches": patches,
		"meshlets": patches * patches,
		"triangles": patches * patches * PATCH * PATCH * 2,
		"require": _required_checks(),
	}
	var result := {
		"test": "meshlets",
		"driver": RenderingServer.get_current_rendering_driver_name(),
		"params": params,
	}

	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		result["pass"] = false
		result["reason"] = "NO_RENDERING_DEVICE"
		return result

	if OS.get_cmdline_user_args().has("--negative"):
		result["negative"] = _run_negative(rd)
		result["pass"] = _failures.is_empty()
		result["failures"] = _failures
		rd.free()
		return result

	var features := {
		"mesh_shader": rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER),
		"mesh_shader_indirect": rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER_INDIRECT),
		"draw_indirect_count": rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT),
	}
	result["adapter"] = rd.get_device_name()
	result["features"] = features
	var limits := {}
	for key: String in MESH_LIMITS:
		limits[key] = rd.limit_get(MESH_LIMITS[key])
		if features["mesh_shader"]:
			_check(limits[key] > 0, "the limit %s is more than 0 with mesh shaders" % key)
		else:
			_check(limits[key] == 0, "the limit %s is 0 without mesh shaders" % key)
	result["limits"] = limits
	var checks := {
		"mesh": "pass" if features["mesh_shader"] else "unsupported",
		"indirect": "pass" if features["mesh_shader"] and features["mesh_shader_indirect"] else "unsupported",
		"indirect_count": "pass" if features["mesh_shader"] and features["mesh_shader_indirect"] and features["draw_indirect_count"] else "unsupported",
	}
	var cases := {}
	var digests := {}
	var stats := {}

	var mesh := _build_mesh(patches)
	var frame := _build_frame(mesh)
	stats["cpu_visible_meshlets"] = frame["cpu_visible"]
	var resources := _create_resources(rd, params, mesh, frame, features["mesh_shader"])
	if not resources.is_empty():
		var vertex_images := _draw_vertex_path(rd, params, resources)
		digests["vertex"] = _digest(vertex_images["ids"] + vertex_images["counts"])
		var vertex_covered := _check_single_coverage(vertex_images, "vertex_path")
		stats["vertex_covered_pixels"] = vertex_covered
		cases["vertex_path"] = "pass" if vertex_covered > 0 else "fail"
		_check(vertex_covered > 0, "the vertex path covers part of the image")

		if features["mesh_shader"]:
			var direct := _draw_mesh_case(rd, params, resources, { "kind": "direct" })
			digests["mesh_direct"] = _digest(direct["ids"] + direct["counts"])
			var mesh_covered := _check_single_coverage(direct, "mesh_direct")
			stats["mesh_covered_pixels"] = mesh_covered
			stats["gpu_visible_meshlets"] = direct["visible"]
			stats["gpu_tested_meshlets"] = direct["tested"]
			cases["mesh_direct"] = "pass" if mesh_covered > 0 else "fail"
			_check(mesh_covered > 0, "the mesh path covers part of the image")

			# The task shader culled meshlets, and the ones it kept match the CPU's plane test.
			var culled_ok: bool = direct["tested"] == params["meshlets"] and absi(direct["visible"] - frame["cpu_visible"]) <= 2 and direct["visible"] > 0 and direct["visible"] < params["meshlets"]
			cases["culling"] = "pass" if culled_ok else "fail"
			_check(culled_ok, "the task shader tested %d of %d meshlets and kept %d; the CPU keeps %d (both must be more than 0 and fewer than all)" % [direct["tested"], params["meshlets"], direct["visible"], frame["cpu_visible"]])

			# No meshlet the vertex path drew any pixel of was culled.
			var false_culls := _false_culls(vertex_images["ids"], direct["kept"], params["meshlets"])
			stats["false_culls"] = false_culls
			if false_culls != 0:
				cases["culling"] = "fail"
				_check(false, "the task shader culled %d meshlets the vertex path drew pixels of" % false_culls)

			var comparison := _compare_ids(direct["ids"], vertex_images["ids"])
			stats["mesh_vs_vertex"] = comparison
			cases["mesh_vs_vertex"] = "pass" if comparison["holes"] == 0 else "fail"
			_check(comparison["holes"] == 0, "%d pixels are covered by only one of the mesh and vertex paths" % comparison["holes"])

			if checks["indirect"] != "unsupported":
				for case_info in [
					{ "kind": "indirect", "stride": 0, "draws": [1] },
					{ "kind": "indirect", "stride": 32, "draws": [1, 0, 0] },
				]:
					var key := "indirect_s%d_n%d" % [case_info["stride"], case_info["draws"].size()]
					var status := _check_same(_draw_mesh_case(rd, params, resources, case_info), direct, key, digests)
					cases[key] = status
					if status != "pass":
						checks["indirect"] = "fail"
			if checks["indirect_count"] != "unsupported":
				for case_info in [
					{ "kind": "count", "stride": 16, "draws": [1], "count": 1 },
					{ "kind": "count", "stride": 32, "draws": [1, 0, 0], "count": 3 },
					{ "kind": "count", "stride": 32, "draws": [1], "count": 0 },
				]:
					var key := "count_s%d_c%d" % [case_info["stride"], case_info["count"]]
					var drawn := _draw_mesh_case(rd, params, resources, case_info)
					var status := ""
					if case_info["count"] == 0:
						status = _check_empty(drawn, key, digests)
					else:
						status = _check_same(drawn, direct, key, digests)
					cases[key] = status
					if status != "pass":
						checks["indirect_count"] = "fail"
			if cases["mesh_direct"] != "pass" or cases["culling"] != "pass" or cases["mesh_vs_vertex"] != "pass":
				checks["mesh"] = "fail"
		_free_resources(rd, resources)
	rd.free()

	for required in params["require"]:
		if not REQUIRE_CHECKS.has(required):
			_check(false, "--require names an unknown check '%s' (known: %s)" % [required, ", ".join(REQUIRE_CHECKS)])
			continue
		_check(checks[required] != "unsupported", "the required check %s ran (--require=%s)" % [required, required])

	result["checks"] = checks
	result["cases"] = cases
	result["stats"] = stats
	result["digests"] = digests
	result["pass"] = _failures.is_empty()
	result["failures"] = _failures
	return result


# The --require list, without empty entries.
static func _required_checks() -> PackedStringArray:
	var required := PackedStringArray()
	for item in SceneArgs.value("--require", "").split(","):
		if not item.strip_edges().is_empty():
			required.append(item.strip_edges())
	return required


# The grid on z = 0 over [-1, 1] x [-1, 1], and its meshlets: one per PATCH x PATCH quads, in patch rows.
static func _build_mesh(patches: int) -> Dictionary:
	var quads := patches * PATCH
	var side := quads + 1
	var positions := PackedFloat32Array()
	positions.resize(side * side * 4)
	for j in side:
		var y := -1.0 + 2.0 * j / quads
		for i in side:
			var base := (j * side + i) * 4
			positions[base] = -1.0 + 2.0 * i / quads
			positions[base + 1] = y
			positions[base + 2] = 0.0
			positions[base + 3] = 1.0

	var meshlet_count := patches * patches
	var ranges := PackedInt32Array()
	ranges.resize(meshlet_count * 4)
	var spheres := PackedFloat32Array()
	spheres.resize(meshlet_count * 4)
	var vertices := PackedInt32Array()
	vertices.resize(meshlet_count * (PATCH + 1) * (PATCH + 1))
	var triangles := PackedInt32Array()
	triangles.resize(meshlet_count * PATCH * PATCH * 2)
	var patch_size := 2.0 * PATCH / quads
	# The 98 triangles of a meshlet, as packed local indices, the same for every meshlet.
	var local_triangles := PackedInt32Array()
	for qj in PATCH:
		for qi in PATCH:
			var a := qj * (PATCH + 1) + qi
			local_triangles.append(a | ((a + 1) << 8) | ((a + PATCH + 2) << 16))
			local_triangles.append(a | ((a + PATCH + 2) << 8) | ((a + PATCH + 1) << 16))
	var vertex_cursor := 0
	var triangle_cursor := 0
	for pj in patches:
		for pi in patches:
			var meshlet := pj * patches + pi
			ranges[meshlet * 4] = vertex_cursor
			ranges[meshlet * 4 + 1] = (PATCH + 1) * (PATCH + 1)
			ranges[meshlet * 4 + 2] = triangle_cursor
			ranges[meshlet * 4 + 3] = local_triangles.size()
			for vj in PATCH + 1:
				var row := (pj * PATCH + vj) * side + pi * PATCH
				for vi in PATCH + 1:
					vertices[vertex_cursor] = row + vi
					vertex_cursor += 1
			for t in local_triangles.size():
				triangles[triangle_cursor + t] = local_triangles[t]
			triangle_cursor += local_triangles.size()
			spheres[meshlet * 4] = -1.0 + patch_size * (pi + 0.5)
			spheres[meshlet * 4 + 1] = -1.0 + patch_size * (pj + 0.5)
			spheres[meshlet * 4 + 2] = 0.0
			spheres[meshlet * 4 + 3] = patch_size * 0.5 * sqrt(2.0) * 1.001
	return {
		"positions": positions,
		"ranges": ranges,
		"spheres": spheres,
		"vertices": vertices,
		"triangles": triangles,
		"meshlets": meshlet_count,
	}


# The camera, its frustum planes (the clip volume of Vulkan, Direct3D 12 and Metal: -w <= x, y <= w and
# 0 <= z <= w) and the meshlets the CPU keeps with the task shader's test.
static func _build_frame(mesh: Dictionary) -> Dictionary:
	var projection := Projection.create_perspective(55.0, float(WIDTH) / HEIGHT, 0.05, 2.2)
	var camera := Transform3D(Basis(), Vector3(0.25, -1.05, 0.55)).looking_at(Vector3(0.45, 0.05, 0.0), Vector3(0.0, 0.0, 1.0))
	var view_projection: Projection = projection * Projection(camera.affine_inverse())
	var rows: Array[Vector4] = []
	for i in 4:
		rows.append(Vector4(view_projection.x[i], view_projection.y[i], view_projection.z[i], view_projection.w[i]))
	var plane_rows: Array[Vector4] = [rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]]
	var planes: Array[Vector4] = []
	for plane in plane_rows:
		planes.append(plane / Vector3(plane.x, plane.y, plane.z).length())

	var spheres: PackedFloat32Array = mesh["spheres"]
	var visible := 0
	for m in mesh["meshlets"]:
		var center := Vector3(spheres[m * 4], spheres[m * 4 + 1], spheres[m * 4 + 2])
		var radius := spheres[m * 4 + 3]
		var inside := true
		for plane in planes:
			if Vector3(plane.x, plane.y, plane.z).dot(center) + plane.w < -radius:
				inside = false
		if inside:
			visible += 1

	var data := PackedFloat32Array()
	for column: Vector4 in [view_projection.x, view_projection.y, view_projection.z, view_projection.w]:
		data.append_array(PackedFloat32Array([column.x, column.y, column.z, column.w]))
	for plane in planes:
		data.append_array(PackedFloat32Array([plane.x, plane.y, plane.z, plane.w]))
	var bytes := data.to_byte_array()
	bytes.append_array(PackedInt32Array([mesh["meshlets"], MAX_TRIANGLES, 0, 0]).to_byte_array())
	return { "bytes": bytes, "cpu_visible": visible }


func _create_resources(rd: RenderingDevice, params: Dictionary, mesh: Dictionary, frame: Dictionary, mesh_shader: bool) -> Dictionary:
	var resources := {}

	var vertex_source := RDShaderSource.new()
	vertex_source.source_vertex = VERTEX_SOURCE % COMMON
	vertex_source.source_fragment = VERTEX_FRAGMENT_SOURCE
	var vertex_spirv := rd.shader_compile_spirv_from_source(vertex_source)
	_check(vertex_spirv.compile_error_vertex.is_empty() and vertex_spirv.compile_error_fragment.is_empty(), "the vertex-path shader compiles: %s %s" % [vertex_spirv.compile_error_vertex, vertex_spirv.compile_error_fragment])
	resources["vertex_shader"] = rd.shader_create_from_spirv(vertex_spirv, "meshlets_vertex_path")

	var count_source := RDShaderSource.new()
	count_source.source_compute = COUNT_SOURCE
	var count_spirv := rd.shader_compile_spirv_from_source(count_source)
	_check(count_spirv.compile_error_compute.is_empty(), "the count shader compiles: %s" % count_spirv.compile_error_compute)
	resources["count_shader"] = rd.shader_create_from_spirv(count_spirv, "meshlets_count")

	if mesh_shader:
		var mesh_source := RDShaderSource.new()
		mesh_source.source_task = TASK_SOURCE % [TASK_GROUP, COMMON, TASK_GROUP]
		mesh_source.source_mesh = MESH_SOURCE % [MAX_VERTICES, MAX_TRIANGLES, COMMON, TASK_GROUP]
		mesh_source.source_fragment = MESH_FRAGMENT_SOURCE
		var mesh_spirv := rd.shader_compile_spirv_from_source(mesh_source)
		_check(mesh_spirv.compile_error_task.is_empty() and mesh_spirv.compile_error_mesh.is_empty() and mesh_spirv.compile_error_fragment.is_empty(), "the mesh-path shader compiles: %s %s %s" % [mesh_spirv.compile_error_task, mesh_spirv.compile_error_mesh, mesh_spirv.compile_error_fragment])
		resources["mesh_shader"] = rd.shader_create_from_spirv(mesh_spirv, "meshlets_mesh_path")

	for key in resources:
		if not _valid(resources, key):
			_check(false, "%s is created" % key)
			_free_resources(rd, resources)
			return {}

	var positions: PackedByteArray = mesh["positions"].to_byte_array()
	resources["positions"] = rd.storage_buffer_create(positions.size(), positions)
	var meshlet_data := PackedByteArray()
	var ranges: PackedInt32Array = mesh["ranges"]
	var spheres: PackedFloat32Array = mesh["spheres"]
	meshlet_data.resize(mesh["meshlets"] * 32)
	for m in mesh["meshlets"]:
		for k in 4:
			meshlet_data.encode_u32(m * 32 + k * 4, ranges[m * 4 + k])
			meshlet_data.encode_float(m * 32 + 16 + k * 4, spheres[m * 4 + k])
	resources["meshlet_data"] = rd.storage_buffer_create(meshlet_data.size(), meshlet_data)
	var vertices: PackedByteArray = mesh["vertices"].to_byte_array()
	resources["meshlet_vertices"] = rd.storage_buffer_create(vertices.size(), vertices)
	var triangles: PackedByteArray = mesh["triangles"].to_byte_array()
	resources["meshlet_triangles"] = rd.storage_buffer_create(triangles.size(), triangles)
	resources["frame"] = rd.uniform_buffer_create(frame["bytes"].size(), frame["bytes"])
	resources["stats"] = rd.storage_buffer_create(16 + mesh["meshlets"] * 4, PackedByteArray())
	resources["count"] = rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)

	var shared_uniforms: Array[RDUniform] = [
		_buffer_uniform(0, resources["positions"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER),
		_buffer_uniform(1, resources["meshlet_data"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER),
		_buffer_uniform(2, resources["meshlet_vertices"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER),
		_buffer_uniform(3, resources["meshlet_triangles"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER),
		_buffer_uniform(4, resources["frame"], RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER),
	]
	resources["vertex_uniforms"] = rd.uniform_set_create(shared_uniforms, resources["vertex_shader"], 0)
	if mesh_shader:
		var mesh_uniforms: Array[RDUniform] = shared_uniforms.duplicate()
		mesh_uniforms.append(_buffer_uniform(5, resources["stats"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER))
		resources["mesh_uniforms"] = rd.uniform_set_create(mesh_uniforms, resources["mesh_shader"], 0)
	var count_uniforms: Array[RDUniform] = [_buffer_uniform(0, resources["count"], RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER)]
	resources["count_uniforms"] = rd.uniform_set_create(count_uniforms, resources["count_shader"], 0)
	resources["count_pipeline"] = rd.compute_pipeline_create(resources["count_shader"])

	resources["ids"] = _target(rd, RenderingDevice.DATA_FORMAT_R32_UINT)
	resources["counts"] = _target(rd, RenderingDevice.DATA_FORMAT_R16_SFLOAT)
	var framebuffer_textures: Array[RID] = [resources["ids"], resources["counts"]]
	resources["framebuffer"] = rd.framebuffer_create(framebuffer_textures)
	if _valid(resources, "framebuffer"):
		var add := RDPipelineColorBlendStateAttachment.new()
		add.enable_blend = true
		add.src_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.dst_color_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.color_blend_op = RenderingDevice.BLEND_OP_ADD
		add.src_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.dst_alpha_blend_factor = RenderingDevice.BLEND_FACTOR_ONE
		add.alpha_blend_op = RenderingDevice.BLEND_OP_ADD
		var blend := RDPipelineColorBlendState.new()
		var blend_attachments: Array[RDPipelineColorBlendStateAttachment] = [RDPipelineColorBlendStateAttachment.new(), add]
		blend.attachments = blend_attachments
		var format := rd.framebuffer_get_format(resources["framebuffer"])
		resources["vertex_pipeline"] = rd.render_pipeline_create(resources["vertex_shader"], format, RenderingDevice.INVALID_FORMAT_ID, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)
		if mesh_shader:
			resources["mesh_pipeline"] = rd.render_pipeline_create(resources["mesh_shader"], format, RenderingDevice.INVALID_FORMAT_ID, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)

	# Records for the indirect cases: RECORDS_OFFSET poison bytes, then the records with poison between them.
	for stride: int in [0, 16, 32]:
		var record_size := 12 if stride == 0 else stride
		var words := PackedInt32Array()
		words.resize((RECORDS_OFFSET + 8 * record_size + 64) / 4)
		words.fill(POISON)
		resources["records_%d" % stride] = words

	for key in resources:
		if resources[key] is RID and not _valid(resources, key):
			_check(false, "%s is created" % key)
			_free_resources(rd, resources)
			return {}
	return resources


func _target(rd: RenderingDevice, format_id: RenderingDevice.DataFormat) -> RID:
	var format := RDTextureFormat.new()
	format.format = format_id
	format.width = WIDTH
	format.height = HEIGHT
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	return rd.texture_create(format, RDTextureView.new())


func _draw_vertex_path(rd: RenderingDevice, params: Dictionary, resources: Dictionary) -> Dictionary:
	var draw_list := rd.draw_list_begin(resources["framebuffer"], RenderingDevice.DRAW_CLEAR_COLOR_ALL, PackedColorArray([Color(0, 0, 0, 0), Color(0, 0, 0, 0)]))
	rd.draw_list_bind_render_pipeline(draw_list, resources["vertex_pipeline"])
	rd.draw_list_bind_uniform_set(draw_list, resources["vertex_uniforms"], 0)
	rd.draw_list_draw(draw_list, false, 1, params["meshlets"] * MAX_TRIANGLES * 3)
	rd.draw_list_end()
	rd.submit()
	rd.sync()
	return { "ids": rd.texture_get_data(resources["ids"], 0), "counts": rd.texture_get_data(resources["counts"], 0) }


# One mesh path draw: direct, indirect (records "draws": 1 for every task workgroup, 0 for an empty record) or
# indirect count (the first "count" records run; the count is written by a compute pass first).
func _draw_mesh_case(rd: RenderingDevice, params: Dictionary, resources: Dictionary, case_info: Dictionary) -> Dictionary:
	var task_groups := ceili(params["meshlets"] / float(TASK_GROUP))
	rd.buffer_clear(resources["stats"], 0, 16 + params["meshlets"] * 4)
	var records := RID()
	if case_info["kind"] != "direct":
		var stride: int = case_info["stride"]
		var words: PackedInt32Array = resources["records_%d" % stride].duplicate()
		var record_size := 12 if stride == 0 else stride
		for r in case_info["draws"].size():
			var base: int = (RECORDS_OFFSET + r * record_size) / 4
			words[base] = task_groups if case_info["draws"][r] == 1 else 0
			words[base + 1] = 1
			words[base + 2] = 1
		var bytes := words.to_byte_array()
		records = rd.storage_buffer_create(bytes.size(), bytes, RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	if case_info["kind"] == "count":
		var count_push := PackedInt32Array([case_info["count"], 0x7fffffff, 0, 0]).to_byte_array()
		var compute_list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute_list, resources["count_pipeline"])
		rd.compute_list_bind_uniform_set(compute_list, resources["count_uniforms"], 0)
		rd.compute_list_set_push_constant(compute_list, count_push, count_push.size())
		rd.compute_list_dispatch(compute_list, 1, 1, 1)
		rd.compute_list_end()

	var draw_list := rd.draw_list_begin(resources["framebuffer"], RenderingDevice.DRAW_CLEAR_COLOR_ALL, PackedColorArray([Color(0, 0, 0, 0), Color(0, 0, 0, 0)]))
	rd.draw_list_bind_render_pipeline(draw_list, resources["mesh_pipeline"])
	rd.draw_list_bind_uniform_set(draw_list, resources["mesh_uniforms"], 0)
	match case_info["kind"]:
		"direct":
			rd.draw_list_draw_mesh_tasks(draw_list, task_groups, 1, 1)
		"indirect":
			rd.draw_list_draw_mesh_tasks_indirect(draw_list, records, RECORDS_OFFSET, case_info["draws"].size(), case_info["stride"])
		"count":
			rd.draw_list_draw_mesh_tasks_indirect_count(draw_list, records, RECORDS_OFFSET, resources["count"], COUNT_OFFSET, 8, case_info["stride"])
	rd.draw_list_end()
	rd.submit()
	rd.sync()

	var stats := rd.buffer_get_data(resources["stats"])
	var result := {
		"ids": rd.texture_get_data(resources["ids"], 0),
		"counts": rd.texture_get_data(resources["counts"], 0),
		"visible": stats.decode_u32(0),
		"tested": stats.decode_u32(4),
		"kept": stats.slice(16),
	}
	if records.is_valid():
		rd.free_rid(records)
	return result


# Every pixel has 0 or 1 fragments, and the covered pixels have an id; returns the covered pixel count, or -1.
func _check_single_coverage(images: Dictionary, key: String) -> int:
	var counts: PackedByteArray = images["counts"]
	var ids: PackedByteArray = images["ids"]
	var covered := 0
	var overdrawn := 0
	var unmatched := 0
	for pixel in range(0, WIDTH * HEIGHT):
		var count := counts.decode_half(pixel * 2)
		if count > 1.5:
			overdrawn += 1
		elif count > 0.5:
			covered += 1
		if (count > 0.5) != (ids.decode_u32(pixel * 4) != 0):
			unmatched += 1
	_check(overdrawn == 0, "%s: %d pixels got more than one fragment" % [key, overdrawn])
	_check(unmatched == 0, "%s: %d pixels have a fragment count that does not match their id" % [key, unmatched])
	return covered if overdrawn == 0 and unmatched == 0 else -1


# The meshlets with a pixel in the vertex path's ids (MAX_TRIANGLES ids per meshlet) that the task shader did not
# keep.
static func _false_culls(vertex_ids: PackedByteArray, kept: PackedByteArray, meshlets: int) -> int:
	var on_screen := PackedByteArray()
	on_screen.resize(meshlets)
	on_screen.fill(0)
	for pixel in range(0, WIDTH * HEIGHT):
		var id := vertex_ids.decode_u32(pixel * 4)
		if id != 0:
			on_screen[(id - 1) / MAX_TRIANGLES] = 1
	var culled := 0
	for m in meshlets:
		if on_screen[m] == 1 and kept.decode_u32(m * 4) == 0:
			culled += 1
	return culled


# The pixels where the mesh and vertex paths wrote different triangles, and the holes: pixels only one path covered.
static func _compare_ids(mesh_ids: PackedByteArray, vertex_ids: PackedByteArray) -> Dictionary:
	var different := 0
	var holes := 0
	for pixel in range(0, WIDTH * HEIGHT):
		var a := mesh_ids.decode_u32(pixel * 4)
		var b := vertex_ids.decode_u32(pixel * 4)
		if a != b:
			different += 1
			if a == 0 or b == 0:
				holes += 1
	return { "different_pixels": different, "holes": holes, "identical": different == 0 }


func _check_same(drawn: Dictionary, direct: Dictionary, key: String, digests: Dictionary) -> String:
	digests[key] = _digest(drawn["ids"] + drawn["counts"])
	if drawn["ids"] != direct["ids"] or drawn["counts"] != direct["counts"]:
		var compare := _compare_ids(drawn["ids"], direct["ids"])
		_check(false, "%s: the image differs from the direct draw in %d pixels (%d holes); counts %s" % [key, compare["different_pixels"], compare["holes"], "differ" if drawn["counts"] != direct["counts"] else "match"])
		return "fail"
	if drawn["visible"] != direct["visible"]:
		_check(false, "%s: the task shader kept %d meshlets, the direct draw %d" % [key, drawn["visible"], direct["visible"]])
		return "fail"
	return "pass"


func _check_empty(drawn: Dictionary, key: String, digests: Dictionary) -> String:
	digests[key] = _digest(drawn["ids"] + drawn["counts"])
	var empty := PackedByteArray()
	empty.resize(drawn["ids"].size())
	empty.fill(0)
	if drawn["ids"] != empty or drawn["visible"] != 0:
		_check(false, "%s: a count of 0 drew %d meshlets" % [key, drawn["visible"]])
		return "fail"
	return "pass"


# The calls RenderingDevice refuses, each with exactly one error that the ErrorLog checks. Returns what each call
# logged and whether anything was drawn, so a call that went through shows.
func _run_negative(rd: RenderingDevice) -> Dictionary:
	var result := { "mesh_shader": rd.has_feature(RenderingDevice.SUPPORTS_MESH_SHADER) }
	if not result["mesh_shader"]:
		_check(false, "--negative needs SUPPORTS_MESH_SHADER")
		return result
	var error_log := ErrorLog.new()
	OS.add_logger(error_log)
	var mesh := _build_mesh(2)
	var frame := _build_frame(mesh)
	var params := { "meshlets": mesh["meshlets"] }
	var resources := _create_resources(rd, params, mesh, frame, true)
	var setup_errors := error_log.take()
	_check(setup_errors.is_empty(), "the setup logged no error: %s" % [setup_errors])
	if resources.is_empty():
		OS.remove_logger(error_log)
		return result
	var refused := {}
	var attribute := RDVertexAttribute.new()
	attribute.location = 0
	attribute.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	attribute.stride = 16
	var attributes: Array[RDVertexAttribute] = [attribute]
	var vertex_format := rd.vertex_format_create(attributes)
	var blend := RDPipelineColorBlendState.new()
	var blend_attachments: Array[RDPipelineColorBlendStateAttachment] = [RDPipelineColorBlendStateAttachment.new(), RDPipelineColorBlendStateAttachment.new()]
	blend.attachments = blend_attachments
	error_log.take()
	# A mesh pipeline with a vertex format.
	var pipeline := rd.render_pipeline_create(resources["mesh_shader"], rd.framebuffer_get_format(resources["framebuffer"]), vertex_format, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)
	_expect_refused(error_log, refused, "mesh_pipeline_with_vertex_format", not pipeline.is_valid(), "A render pipeline with a mesh shader takes no vertex format")
	if pipeline.is_valid():
		rd.free_rid(pipeline)
	# A mesh pipeline for a multiview framebuffer format. RenderingDevice does not bind SUPPORTS_MULTIVIEW, the first
	# feature (0).
	if rd.has_feature(0 as RenderingDevice.Features):
		var attachment_formats: Array[RDAttachmentFormat] = []
		for data_format in [RenderingDevice.DATA_FORMAT_R32_UINT, RenderingDevice.DATA_FORMAT_R16_SFLOAT]:
			var attachment := RDAttachmentFormat.new()
			attachment.format = data_format
			attachment.usage_flags = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT
			attachment_formats.append(attachment)
		var multiview_format := rd.framebuffer_format_create(attachment_formats, 2)
		var format_errors := error_log.take()
		_check(format_errors.is_empty(), "the multiview framebuffer format is created: %s" % [format_errors])
		pipeline = rd.render_pipeline_create(resources["mesh_shader"], multiview_format, RenderingDevice.INVALID_FORMAT_ID, RenderingDevice.RENDER_PRIMITIVE_TRIANGLES, RDPipelineRasterizationState.new(), RDPipelineMultisampleState.new(), RDPipelineDepthStencilState.new(), blend)
		_expect_refused(error_log, refused, "mesh_pipeline_multiview", not pipeline.is_valid(), "A render pipeline with a mesh shader cannot draw to a multiview framebuffer format")
		if pipeline.is_valid():
			rd.free_rid(pipeline)
	else:
		refused["mesh_pipeline_multiview"] = "unsupported"
	# Draws that must draw nothing: with the mesh pipeline, a vertex draw and mesh task draws over the X group-count
	# limit and over the total; with the vertex pipeline, a mesh task draw.
	var task_count_x: int = rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_X)
	var draw_list := rd.draw_list_begin(resources["framebuffer"], RenderingDevice.DRAW_CLEAR_COLOR_ALL, PackedColorArray([Color(0, 0, 0, 0), Color(0, 0, 0, 0)]))
	rd.draw_list_bind_render_pipeline(draw_list, resources["mesh_pipeline"])
	rd.draw_list_bind_uniform_set(draw_list, resources["mesh_uniforms"], 0)
	error_log.take()
	rd.draw_list_draw(draw_list, false, 1, 3)
	_expect_refused(error_log, refused, "vertex_draw_with_mesh_pipeline", true, "The bound render pipeline's shader has a mesh stage")
	if task_count_x > 0 and task_count_x < MAX_GROUP_COUNT:
		rd.draw_list_draw_mesh_tasks(draw_list, task_count_x + 1, 1, 1)
		_expect_refused(error_log, refused, "mesh_draw_over_task_count_x", true, "The X group count of the mesh task draw (%d) is larger than the device limit for task workgroups (%d)." % [task_count_x + 1, task_count_x])
	else:
		refused["mesh_draw_over_task_count_x"] = "unbounded"
	var total_counts := _over_total_counts(rd)
	if total_counts.is_empty():
		refused["mesh_draw_over_task_total"] = "unbounded"
	else:
		rd.draw_list_draw_mesh_tasks(draw_list, total_counts[0], total_counts[1], total_counts[2])
		_expect_refused(error_log, refused, "mesh_draw_over_task_total", true, "The mesh task draw launches %d x %d x %d task workgroups, more than the device limit of %d in all." % [total_counts[0], total_counts[1], total_counts[2], rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT)])
	rd.draw_list_bind_render_pipeline(draw_list, resources["vertex_pipeline"])
	rd.draw_list_bind_uniform_set(draw_list, resources["vertex_uniforms"], 0)
	error_log.take()
	rd.draw_list_draw_mesh_tasks(draw_list, 1, 1, 1)
	_expect_refused(error_log, refused, "mesh_draw_with_vertex_pipeline", true, "draw_list_draw_mesh_tasks*() needs a render pipeline whose shader has a mesh stage")
	rd.draw_list_end()
	rd.submit()
	rd.sync()
	result["refused"] = refused
	# Nothing was drawn.
	var counts := rd.texture_get_data(resources["counts"], 0)
	var empty := PackedByteArray()
	empty.resize(counts.size())
	empty.fill(0)
	result["drew"] = counts != empty
	_check(counts == empty, "a refused draw drew fragments")
	_free_resources(rd, resources)
	var teardown_errors := error_log.take()
	_check(teardown_errors.is_empty(), "the teardown logged no error: %s" % [teardown_errors])
	OS.remove_logger(error_log)
	return result


# Task workgroup counts that each stay within their axis limit but together exceed LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT,
# or none when the axis limits cannot exceed it.
static func _over_total_counts(rd: RenderingDevice) -> Array[int]:
	var limit_x: int = rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_X)
	var limit_y: int = rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_Y)
	var limit_z: int = rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_COUNT_Z)
	var total: int = rd.limit_get(RenderingDevice.LIMIT_MAX_TASK_WORKGROUP_TOTAL_COUNT)
	if limit_x <= 0 or limit_y <= 0 or limit_z <= 0 or total <= 0:
		return []
	var x := mini(limit_x, total)
	var y := mini(limit_y, int(float(total) / float(x)) + 1)
	var z := mini(limit_z, int(float(total) / float(x * y)) + 1)
	if x * y * z <= total:
		return []
	return [x, y, z]


# One refused call: it returned nothing (was_refused) and logged exactly one error, which contains expected.
func _expect_refused(error_log: ErrorLog, refused: Dictionary, key: String, was_refused: bool, expected: String) -> void:
	var messages := error_log.take()
	var ok := was_refused and messages.size() == 1 and messages[0].contains(expected)
	refused[key] = "refused" if ok else "returned nothing: %s; errors: %s" % [was_refused, messages]
	_check(ok, "%s: expected nothing back and one error containing \"%s\", got %s" % [key, expected, messages])


func _free_resources(rd: RenderingDevice, resources: Dictionary) -> void:
	# Dependents first: pipelines, uniform sets and the framebuffer before what they use.
	for key in ["mesh_pipeline", "vertex_pipeline", "count_pipeline", "mesh_uniforms", "vertex_uniforms", "count_uniforms", "framebuffer"]:
		if _valid(resources, key):
			rd.free_rid(resources[key])
		resources.erase(key)
	for key in resources:
		if _valid(resources, key):
			rd.free_rid(resources[key])
	resources.clear()


static func _valid(resources: Dictionary, key: String) -> bool:
	return resources.has(key) and resources[key] is RID and (resources[key] as RID).is_valid()


func _buffer_uniform(binding: int, buffer: RID, type: RenderingDevice.UniformType) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = type
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


static func _digest(data: PackedByteArray) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(data)
	return context.finish().hex_encode()


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)
