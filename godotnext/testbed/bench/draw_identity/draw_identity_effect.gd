@tool
extends CompositorEffect
class_name DrawIdentityEffect

# GodotNext: #69 draw-identity benchmark of paths (c), (a), and (b) at Dense City scale.
# Runs 4 GPU-timestamped stages before the opaque pass:
#   1. Instance frustum cull
#   2. Cluster frustum + backface cone cull
#   3. Compaction + indirect args write (path c, a, or b)
#   4. Visibility-buffer raster (R32_UINT = visible_index << 7 | triangle, D32_SFLOAT reverse-Z)

const MAX_VIS_CLUSTERS := 1048576
const MAX_VIS_TRIS := 70000000 # 267 MiB buffer covers all ~66.6M visible finest-level Dense City triangles; full worst-case MiB also reported.

var path_mode: String = "c" # "c", "a", or "b"
var warmup_frames: int = 120
var measure_frames: int = 600
var target_width: int = 1920
var target_height: int = 1080
var done: bool = false
var failed: bool = false
var fail_reason: String = ""
var result_summary: Dictionary = {}

# Geometry & instance data prepared on main thread before rendering.
var _ready_data: bool = false
var _initialized_gpu: bool = false
var _instance_count: int = 0
var _pair_count: int = 0
var _unique_cluster_count: int = 0
var _total_reachable_tris: int = 0
var _instances_bytes := PackedByteArray()
var _pairs_bytes := PackedByteArray()
var _clusters_bytes := PackedByteArray()
var _verts_bytes := PackedByteArray()
var _tris_bytes := PackedByteArray()

# GPU resources (created on render thread).
var _rd: RenderingDevice
var _buf_instances: RID
var _buf_inst_vis: RID
var _buf_pairs: RID
var _buf_clusters: RID
var _buf_cluster_vis: RID
var _buf_verts: RID
var _buf_tris: RID
var _buf_vis_clusters: RID
var _buf_vis_tris: RID
var _buf_indirect: RID
var _buf_count: RID

var _tex_vis: RID
var _tex_depth: RID
var _framebuffer: RID

var _sh_inst_cull: RID
var _sh_cluster_cull: RID
var _sh_compact: RID
var _sh_raster: RID
var _pipe_inst_cull: RID
var _pipe_cluster_cull: RID
var _pipe_compact: RID
var _pipe_raster: RID

var _us_inst_cull: RID
var _us_cluster_cull: RID
var _us_compact: RID
var _us_raster: RID

# Timing collection (frames indexed from first active render).
var _frame_counter: int = 0
var _last_stamp_frame: int = -1
var _s1_samples: Array[float] = []
var _s2_samples: Array[float] = []
var _s3_samples: Array[float] = []
var _s4_samples: Array[float] = []
var _s34_samples: Array[float] = []
var _cpu_submit_samples: Array[float] = []
var _last_vis_clusters: int = 0
var _last_vis_tris: int = 0
var _readback_done: bool = false

func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_PRE_OPAQUE

func prepare_dense_city_data() -> bool:
	var kit_names := [
		"building_a", "building_b", "building_c", "building_d", "building_e",
		"tower_slim", "building_small", "kit_facade", "prop_rooftop",
	]
	var mesh_cluster_offset := PackedInt32Array()
	var mesh_cluster_count := PackedInt32Array()
	var mesh_tri_count := PackedInt32Array()
	var mesh_bounds_center: Array[Vector3] = []
	var mesh_bounds_radius := PackedFloat32Array()
	mesh_cluster_offset.resize(kit_names.size())
	mesh_cluster_count.resize(kit_names.size())
	mesh_tri_count.resize(kit_names.size())
	mesh_bounds_center.resize(kit_names.size())
	mesh_bounds_radius.resize(kit_names.size())

	var all_clusters := PackedByteArray()
	var all_verts := PackedByteArray()
	var all_tris := PackedByteArray()
	var running_clusters: int = 0
	var running_verts: int = 0
	var running_tris: int = 0

	for mi in range(kit_names.size()):
		var glb_path := ProjectSettings.globalize_path("res://generated/dense_city/kit/%s.glb" % kit_names[mi])
		if not FileAccess.file_exists(glb_path):
			fail_reason = "Missing Dense City kit GLB: " + glb_path
			failed = true
			return false
		var doc := GLTFDocument.new()
		var state := GLTFState.new()
		var err := doc.append_from_file(glb_path, state)
		if err != OK:
			fail_reason = "Failed loading GLB %s (err=%d)" % [glb_path, err]
			failed = true
			return false
		var root := doc.generate_scene(state)
		var mesh := _find_mesh(root)
		root.free()
		if mesh == null:
			fail_reason = "No Mesh in GLB: " + glb_path
			failed = true
			return false

		var aabb := mesh.get_aabb()
		mesh_bounds_center[mi] = aabb.get_center()
		mesh_bounds_radius[mi] = aabb.size.length() * 0.5

		var blob := NextGeoDebug.export_clusters(mesh)
		if blob.size() < 16:
			fail_reason = "export_clusters failed on " + kit_names[mi]
			failed = true
			return false
		var magic := blob.decode_u32(0)
		if magic != 0x4C43474E:
			fail_reason = "Invalid NGCL magic on " + kit_names[mi]
			failed = true
			return false
		var c_cnt := blob.decode_u32(4)
		var v_cnt := blob.decode_u32(8)
		var t_cnt := blob.decode_u32(12)

		var c_off := 16
		var v_off := c_off + c_cnt * 48
		var t_off := v_off + v_cnt * 16

		# Patch cluster vertex_offset and triangle_offset to global offsets.
		var patched := blob.slice(c_off, v_off)
		for ci in range(c_cnt):
			var base := ci * 48
			patched.encode_u32(base + 32, patched.decode_u32(base + 32) + running_verts)
			patched.encode_u32(base + 36, patched.decode_u32(base + 36) + running_tris)

		all_clusters.append_array(patched)
		all_verts.append_array(blob.slice(v_off, t_off))
		all_tris.append_array(blob.slice(t_off, t_off + t_cnt * 4))

		mesh_cluster_offset[mi] = running_clusters
		mesh_cluster_count[mi] = c_cnt
		mesh_tri_count[mi] = t_cnt
		running_clusters += c_cnt
		running_verts += v_cnt
		running_tris += t_cnt

	# Parse instance transforms from res://generated/dense_city/dense_city.tscn directly (fast, zero scene load).
	var tscn_path := ProjectSettings.globalize_path("res://generated/dense_city/dense_city.tscn")
	var txt := FileAccess.get_file_as_string(tscn_path)
	if txt.is_empty():
		fail_reason = "Missing dense_city.tscn"
		failed = true
		return false

	var re_node := RegEx.new()
	re_node.compile("\\[node name=\"Inst\\d+_m(\\d+)\"[^\\]]*\\]\\s*(?:transform = Transform3D\\(([^\\)]+)\\))?")
	var matches := re_node.search_all(txt)
	if matches.is_empty():
		fail_reason = "No building instances parsed from dense_city.tscn"
		failed = true
		return false

	# Instance struct = 96 B: mat4 model (64 B) + vec4 world_bounds (16 B) + uvec4 meta (16 B).
	var inst_count := matches.size()
	var inst_buf := PackedByteArray()
	inst_buf.resize(inst_count * 96)

	var total_pairs: int = 0
	var total_tris: int = 0
	var inst_mesh_ids := PackedInt32Array()
	inst_mesh_ids.resize(inst_count)

	for ii in range(inst_count):
		var m := matches[ii]
		var mesh_id := int(m.get_string(1))
		inst_mesh_ids[ii] = mesh_id
		var t := Transform3D.IDENTITY
		var tf_str := m.get_string(2)
		if not tf_str.is_empty():
			var nums := tf_str.split(",")
			if nums.size() == 12:
				var b := Basis(
					Vector3(float(nums[0]), float(nums[3]), float(nums[6])),
					Vector3(float(nums[1]), float(nums[4]), float(nums[7])),
					Vector3(float(nums[2]), float(nums[5]), float(nums[8]))
				)
				t = Transform3D(b, Vector3(float(nums[9]), float(nums[10]), float(nums[11])))
		var base := ii * 96
		# GLSL column-major mat4: col0 = (basis.x, 0), col1 = (basis.y, 0), col2 = (basis.z, 0), col3 = (origin, 1)
		inst_buf.encode_float(base + 0, t.basis.x.x);  inst_buf.encode_float(base + 4, t.basis.x.y);  inst_buf.encode_float(base + 8, t.basis.x.z);  inst_buf.encode_float(base + 12, 0.0)
		inst_buf.encode_float(base + 16, t.basis.y.x); inst_buf.encode_float(base + 20, t.basis.y.y); inst_buf.encode_float(base + 24, t.basis.y.z); inst_buf.encode_float(base + 28, 0.0)
		inst_buf.encode_float(base + 32, t.basis.z.x); inst_buf.encode_float(base + 36, t.basis.z.y); inst_buf.encode_float(base + 40, t.basis.z.z); inst_buf.encode_float(base + 44, 0.0)
		inst_buf.encode_float(base + 48, t.origin.x);  inst_buf.encode_float(base + 52, t.origin.y);  inst_buf.encode_float(base + 56, t.origin.z);  inst_buf.encode_float(base + 60, 1.0)
		var wc := t * mesh_bounds_center[mesh_id]
		var max_scale := maxf(t.basis.x.length(), maxf(t.basis.y.length(), t.basis.z.length()))
		var wr := mesh_bounds_radius[mesh_id] * max_scale
		inst_buf.encode_float(base + 64, wc.x)
		inst_buf.encode_float(base + 68, wc.y)
		inst_buf.encode_float(base + 72, wc.z)
		inst_buf.encode_float(base + 76, wr)
		inst_buf.encode_u32(base + 80, mesh_cluster_offset[mesh_id])
		inst_buf.encode_u32(base + 84, mesh_cluster_count[mesh_id])
		inst_buf.encode_u32(base + 88, 0)
		inst_buf.encode_u32(base + 92, 0)
		total_pairs += mesh_cluster_count[mesh_id]
		total_tris += mesh_tri_count[mesh_id]

	# Build flat (inst_id, cluster_id) pair list for GPU cluster culling & compaction.
	var pairs_buf := PackedByteArray()
	pairs_buf.resize(total_pairs * 8)
	var cursor: int = 0
	for ii in range(inst_count):
		var mid := inst_mesh_ids[ii]
		var c0 := mesh_cluster_offset[mid]
		var cc := mesh_cluster_count[mid]
		for local_c in range(cc):
			pairs_buf.encode_u32(cursor, ii)
			pairs_buf.encode_u32(cursor + 4, c0 + local_c)
			cursor += 8

	_instance_count = inst_count
	_pair_count = total_pairs
	_unique_cluster_count = running_clusters
	_total_reachable_tris = total_tris
	_instances_bytes = inst_buf
	_pairs_bytes = pairs_buf
	_clusters_bytes = all_clusters
	_verts_bytes = all_verts
	_tris_bytes = all_tris
	_ready_data = true
	print("DRAW_IDENTITY_DATA instances=%d unique_clusters=%d reachable_clusters=%d reachable_tris=%d" % [
		_instance_count, _unique_cluster_count, _pair_count, _total_reachable_tris])
	return true

func _find_mesh(p_node: Node) -> Mesh:
	if p_node is MeshInstance3D:
		var mi := p_node as MeshInstance3D
		if mi.mesh != null:
			return mi.mesh
	for child in p_node.get_children():
		var m := _find_mesh(child)
		if m != null:
			return m
	return null

func _compile_compute(p_code: String) -> RID:
	var src := RDShaderSource.new()
	src.set_stage_source(RenderingDevice.SHADER_STAGE_COMPUTE, p_code)
	var spirv := _rd.shader_compile_spirv_from_source(src)
	var err_msg := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
	if not err_msg.is_empty():
		fail_reason = "Compute compile error: " + err_msg
		failed = true
		return RID()
	return _rd.shader_create_from_spirv(spirv)

func _compile_graphics(p_vert: String, p_frag: String) -> RID:
	var src := RDShaderSource.new()
	src.set_stage_source(RenderingDevice.SHADER_STAGE_VERTEX, p_vert)
	src.set_stage_source(RenderingDevice.SHADER_STAGE_FRAGMENT, p_frag)
	var spirv := _rd.shader_compile_spirv_from_source(src)
	var verr := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_VERTEX)
	var ferr := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_FRAGMENT)
	if not verr.is_empty() or not ferr.is_empty():
		fail_reason = "Graphics compile error: V=%s F=%s" % [verr, ferr]
		failed = true
		return RID()
	return _rd.shader_create_from_spirv(spirv)

func _make_storage_uniform(p_binding: int, p_rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = p_binding
	u.add_id(p_rid)
	return u

func _init_gpu() -> void:
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		fail_reason = "No RenderingDevice"
		failed = true
		return
	if path_mode == "a" and not _rd.has_feature(RenderingDevice.SUPPORTS_DRAW_INDIRECT_COUNT):
		fail_reason = "Path (a) requires SUPPORTS_DRAW_INDIRECT_COUNT"
		failed = true
		return

	_buf_instances = _rd.storage_buffer_create(_instances_bytes.size(), _instances_bytes)
	_buf_inst_vis = _rd.storage_buffer_create(maxi(_instance_count * 4, 16))
	_buf_pairs = _rd.storage_buffer_create(_pairs_bytes.size(), _pairs_bytes)
	_buf_clusters = _rd.storage_buffer_create(_clusters_bytes.size(), _clusters_bytes)
	_buf_cluster_vis = _rd.storage_buffer_create(maxi(_pair_count * 4, 16))
	_buf_verts = _rd.storage_buffer_create(_verts_bytes.size(), _verts_bytes)
	_buf_tris = _rd.storage_buffer_create(_tris_bytes.size(), _tris_bytes)
	_buf_vis_clusters = _rd.storage_buffer_create(MAX_VIS_CLUSTERS * 8)

	var vis_tris_bytes := (MAX_VIS_TRIS * 4) if path_mode == "b" else 16
	_buf_vis_tris = _rd.storage_buffer_create(vis_tris_bytes)

	var indirect_bytes := (MAX_VIS_CLUSTERS * 16) if path_mode == "a" else 16
	_buf_indirect = _rd.storage_buffer_create(indirect_bytes, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	_buf_count = _rd.storage_buffer_create(16, PackedByteArray(), RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)

	# Visibility (R32_UINT) + Depth (D32_SFLOAT) attachments.
	var tf_vis := RDTextureFormat.new()
	tf_vis.width = target_width
	tf_vis.height = target_height
	tf_vis.format = RenderingDevice.DATA_FORMAT_R32_UINT
	tf_vis.usage_bits = RenderingDevice.TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	_tex_vis = _rd.texture_create(tf_vis, RDTextureView.new())

	var tf_depth := RDTextureFormat.new()
	tf_depth.width = target_width
	tf_depth.height = target_height
	tf_depth.format = RenderingDevice.DATA_FORMAT_D32_SFLOAT
	tf_depth.usage_bits = RenderingDevice.TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
	_tex_depth = _rd.texture_create(tf_depth, RDTextureView.new())

	_framebuffer = _rd.framebuffer_create([_tex_vis, _tex_depth])

	# Stage 1: Instance frustum cull.
	var glsl_inst_cull := """#version 460
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
layout(set = 0, binding = 0, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 1, std430) writeonly buffer InstVis { uint inst_vis[]; };
layout(push_constant) uniform PC { vec4 planes[6]; uvec4 params; } pc;
void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= pc.params.x) return;
	vec4 wb = insts[id].world_bounds;
	bool vis = true;
	for (int i = 0; i < 6; i++) {
		if (dot(pc.planes[i].xyz, wb.xyz) + pc.planes[i].w < -wb.w) {
			vis = false;
			break;
		}
	}
	inst_vis[id] = vis ? 1u : 0u;
}
"""
	# Stage 2: Cluster frustum + backface cone cull.
	var glsl_cluster_cull := """#version 460
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Pairs { uvec2 pairs[]; };
layout(set = 0, binding = 1, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 2, std430) readonly buffer InstVis { uint inst_vis[]; };
layout(set = 0, binding = 3, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 4, std430) writeonly buffer ClusterVis { uint cluster_vis[]; };
layout(push_constant) uniform PC { vec4 planes[6]; vec4 cam_pos_count; } pc;
void main() {
	uint id = gl_GlobalInvocationID.x;
	uint pair_count = floatBitsToUint(pc.cam_pos_count.w);
	if (id >= pair_count) return;
	uvec2 pair = pairs[id];
	if (inst_vis[pair.x] == 0u) {
		cluster_vis[id] = 0u;
		return;
	}
	mat4 m = insts[pair.x].model;
	Cluster cl = clusters[pair.y];
	vec3 wc = (m * vec4(cl.bounds.xyz, 1.0)).xyz;
	float sx = length(m[0].xyz);
	float sy = length(m[1].xyz);
	float sz = length(m[2].xyz);
	float wr = cl.bounds.w * max(sx, max(sy, sz));
	for (int i = 0; i < 6; i++) {
		if (dot(pc.planes[i].xyz, wc) + pc.planes[i].w < -wr) {
			cluster_vis[id] = 0u;
			return;
		}
	}
	if (cl.cone.w < 1.0) {
		vec3 w_axis = normalize(mat3(m) * cl.cone.xyz);
		vec3 to_c = wc - pc.cam_pos_count.xyz;
		float dist = length(to_c);
		if (dist > wr && dot(to_c / dist, w_axis) >= cl.cone.w + wr / dist) {
			cluster_vis[id] = 0u;
			return;
		}
	}
	cluster_vis[id] = 1u;
}
"""
	# Stage 3: Compaction + indirect args write (mode: 0=c, 1=a, 2=b).
	var glsl_compact := """#version 460
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Pairs { uvec2 pairs[]; };
layout(set = 0, binding = 1, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 2, std430) readonly buffer ClusterVis { uint cluster_vis[]; };
layout(set = 0, binding = 3, std430) writeonly buffer VisClusters { uvec2 vis_clusters[]; };
layout(set = 0, binding = 4, std430) buffer IndirectBuf { uvec4 indirect[]; };
layout(set = 0, binding = 5, std430) buffer CountBuf { uint counts[]; };
layout(set = 0, binding = 6, std430) writeonly buffer VisTris { uint vis_tris[]; };
layout(push_constant) uniform PC { uvec4 params; } pc;
void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= pc.params.x) return;
	if (cluster_vis[id] == 0u) return;
	uvec2 pair = pairs[id];
	uint tri_cnt = clusters[pair.y].counts.w;
	uint mode = pc.params.y;
	uint max_c = pc.params.z;
	uint max_t = pc.params.w;
	uint idx = atomicAdd(counts[0], 1u);
	if (idx >= max_c) return;
	vis_clusters[idx] = pair;
	atomicAdd(counts[1], tri_cnt);
	if (mode == 0u) {
		// Path (c): single indirect instanced draw (vertexCount = 384, instanceCount = visible clusters).
		atomicAdd(indirect[0].y, 1u);
	} else if (mode == 1u) {
		// Path (a): one VkDrawIndirectCommand per visible cluster + count buffer (firstInstance = idx for Metal).
		indirect[idx] = uvec4(tri_cnt * 3u, 1u, 0u, idx);
	} else {
		// Path (b): per-triangle compacted list, single draw with vertexCount = 3N.
		uint base_vert = atomicAdd(indirect[0].x, tri_cnt * 3u);
		uint base_tri = base_vert / 3u;
		if (base_tri + tri_cnt <= max_t) {
			uint prefix = idx << 7u;
			for (uint t = 0u; t < tri_cnt; t++) {
				vis_tris[base_tri + t] = prefix | t;
			}
		} else {
			atomicMin(indirect[0].x, max_t * 3u);
		}
	}
}
"""
	# Stage 4: Visibility raster (path c / a / b).
	var glsl_vert := ""
	if path_mode == "c":
		glsl_vert = """#version 460
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 1, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 2, std430) readonly buffer Verts { vec4 verts[]; };
layout(set = 0, binding = 3, std430) readonly buffer Tris { uint tris[]; };
layout(set = 0, binding = 4, std430) readonly buffer VisClusters { uvec2 vis_clusters[]; };
layout(push_constant) uniform PC { mat4 vp; } pc;
layout(location = 0) flat out uint v_vis_id;
void main() {
	uint vis_idx = uint(gl_InstanceIndex);
	uint tri = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	uvec2 pair = vis_clusters[vis_idx];
	Cluster cl = clusters[pair.y];
	if (tri >= cl.counts.w) {
		gl_Position = vec4(uintBitsToFloat(0x7fc00000u));
		v_vis_id = 0xffffffffu;
		return;
	}
	uint packed = tris[cl.counts.y + tri];
	uint local_v = (packed >> (corner * 8u)) & 0xffu;
	vec3 pos = verts[cl.counts.x + local_v].xyz;
	gl_Position = pc.vp * (insts[pair.x].model * vec4(pos, 1.0));
	v_vis_id = (vis_idx << 7u) | tri;
}
"""
	elif path_mode == "a":
		var is_metal := RenderingServer.get_current_rendering_driver_name().to_lower() == "metal"
		if is_metal:
			# Metal/MSL has no DrawIndex (gl_DrawID); Stage 3 writes firstInstance = idx, read via gl_InstanceIndex.
			glsl_vert = """#version 460
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 1, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 2, std430) readonly buffer Verts { vec4 verts[]; };
layout(set = 0, binding = 3, std430) readonly buffer Tris { uint tris[]; };
layout(set = 0, binding = 4, std430) readonly buffer VisClusters { uvec2 vis_clusters[]; };
layout(push_constant) uniform PC { mat4 vp; } pc;
layout(location = 0) flat out uint v_vis_id;
void main() {
	uint vis_idx = uint(gl_InstanceIndex);
	uint tri = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	uvec2 pair = vis_clusters[vis_idx];
	Cluster cl = clusters[pair.y];
	uint packed = tris[cl.counts.y + tri];
	uint local_v = (packed >> (corner * 8u)) & 0xffu;
	vec3 pos = verts[cl.counts.x + local_v].xyz;
	gl_Position = pc.vp * (insts[pair.x].model * vec4(pos, 1.0));
	v_vis_id = (vis_idx << 7u) | tri;
}
"""
		else:
			glsl_vert = """#version 460
#extension GL_ARB_shader_draw_parameters : require
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 1, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 2, std430) readonly buffer Verts { vec4 verts[]; };
layout(set = 0, binding = 3, std430) readonly buffer Tris { uint tris[]; };
layout(set = 0, binding = 4, std430) readonly buffer VisClusters { uvec2 vis_clusters[]; };
layout(push_constant) uniform PC { mat4 vp; } pc;
layout(location = 0) flat out uint v_vis_id;
void main() {
	uint vis_idx = uint(gl_DrawIDARB);
	uint tri = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	uvec2 pair = vis_clusters[vis_idx];
	Cluster cl = clusters[pair.y];
	uint packed = tris[cl.counts.y + tri];
	uint local_v = (packed >> (corner * 8u)) & 0xffu;
	vec3 pos = verts[cl.counts.x + local_v].xyz;
	gl_Position = pc.vp * (insts[pair.x].model * vec4(pos, 1.0));
	v_vis_id = (vis_idx << 7u) | tri;
}
"""
	else:
		glsl_vert = """#version 460
struct Instance { mat4 model; vec4 world_bounds; uvec4 meta; };
struct Cluster { vec4 bounds; vec4 cone; uvec4 counts; };
layout(set = 0, binding = 0, std430) readonly buffer Instances { Instance insts[]; };
layout(set = 0, binding = 1, std430) readonly buffer Clusters { Cluster clusters[]; };
layout(set = 0, binding = 2, std430) readonly buffer Verts { vec4 verts[]; };
layout(set = 0, binding = 3, std430) readonly buffer Tris { uint tris[]; };
layout(set = 0, binding = 4, std430) readonly buffer VisClusters { uvec2 vis_clusters[]; };
layout(set = 0, binding = 5, std430) readonly buffer VisTris { uint vis_tris[]; };
layout(push_constant) uniform PC { mat4 vp; } pc;
layout(location = 0) flat out uint v_vis_id;
void main() {
	uint tri_slot = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	uint vis_id = vis_tris[tri_slot];
	uint vis_idx = vis_id >> 7u;
	uint tri = vis_id & 127u;
	uvec2 pair = vis_clusters[vis_idx];
	Cluster cl = clusters[pair.y];
	uint packed = tris[cl.counts.y + tri];
	uint local_v = (packed >> (corner * 8u)) & 0xffu;
	vec3 pos = verts[cl.counts.x + local_v].xyz;
	gl_Position = pc.vp * (insts[pair.x].model * vec4(pos, 1.0));
	v_vis_id = vis_id;
}
"""
	var glsl_frag := """#version 460
layout(location = 0) flat in uint v_vis_id;
layout(location = 0) out uint out_vis_id;
void main() {
	out_vis_id = v_vis_id;
}
"""

	_sh_inst_cull = _compile_compute(glsl_inst_cull)
	_sh_cluster_cull = _compile_compute(glsl_cluster_cull)
	_sh_compact = _compile_compute(glsl_compact)
	_sh_raster = _compile_graphics(glsl_vert, glsl_frag)
	if failed:
		return

	_pipe_inst_cull = _rd.compute_pipeline_create(_sh_inst_cull)
	_pipe_cluster_cull = _rd.compute_pipeline_create(_sh_cluster_cull)
	_pipe_compact = _rd.compute_pipeline_create(_sh_compact)

	var rast := RDPipelineRasterizationState.new()
	rast.cull_mode = RenderingDevice.POLYGON_CULL_BACK
	rast.front_face = RenderingDevice.POLYGON_FRONT_FACE_COUNTER_CLOCKWISE
	var ds := RDPipelineDepthStencilState.new()
	ds.enable_depth_test = true
	ds.enable_depth_write = true
	ds.depth_compare_operator = RenderingDevice.COMPARE_OP_GREATER_OR_EQUAL
	var blend := RDPipelineColorBlendState.new()
	var att := RDPipelineColorBlendStateAttachment.new()
	att.enable_blend = false
	blend.attachments = [att]
	var ms := RDPipelineMultisampleState.new()

	_pipe_raster = _rd.render_pipeline_create(
		_sh_raster,
		_rd.framebuffer_get_format(_framebuffer),
		RenderingDevice.INVALID_ID,
		RenderingDevice.RENDER_PRIMITIVE_TRIANGLES,
		rast, ms, ds, blend
	)

	_us_inst_cull = _rd.uniform_set_create([
		_make_storage_uniform(0, _buf_instances),
		_make_storage_uniform(1, _buf_inst_vis),
	], _sh_inst_cull, 0)

	_us_cluster_cull = _rd.uniform_set_create([
		_make_storage_uniform(0, _buf_pairs),
		_make_storage_uniform(1, _buf_instances),
		_make_storage_uniform(2, _buf_inst_vis),
		_make_storage_uniform(3, _buf_clusters),
		_make_storage_uniform(4, _buf_cluster_vis),
	], _sh_cluster_cull, 0)

	_us_compact = _rd.uniform_set_create([
		_make_storage_uniform(0, _buf_pairs),
		_make_storage_uniform(1, _buf_clusters),
		_make_storage_uniform(2, _buf_cluster_vis),
		_make_storage_uniform(3, _buf_vis_clusters),
		_make_storage_uniform(4, _buf_indirect),
		_make_storage_uniform(5, _buf_count),
		_make_storage_uniform(6, _buf_vis_tris),
	], _sh_compact, 0)

	var raster_uniforms: Array[RDUniform] = [
		_make_storage_uniform(0, _buf_instances),
		_make_storage_uniform(1, _buf_clusters),
		_make_storage_uniform(2, _buf_verts),
		_make_storage_uniform(3, _buf_tris),
		_make_storage_uniform(4, _buf_vis_clusters),
	]
	if path_mode == "b":
		raster_uniforms.append(_make_storage_uniform(5, _buf_vis_tris))
	_us_raster = _rd.uniform_set_create(raster_uniforms, _sh_raster, 0)

	_initialized_gpu = true

func _extract_planes(p_vp: Projection) -> Array[Vector4]:
	# Gribb-Hartmann planes from clip-space VP matrix (Vulkan Z in [0, 1], reverse-Z near=1 far=0).
	var r0 := Vector4(p_vp.x.x, p_vp.y.x, p_vp.z.x, p_vp.w.x)
	var r1 := Vector4(p_vp.x.y, p_vp.y.y, p_vp.z.y, p_vp.w.y)
	var r2 := Vector4(p_vp.x.z, p_vp.y.z, p_vp.z.z, p_vp.w.z)
	var r3 := Vector4(p_vp.x.w, p_vp.y.w, p_vp.z.w, p_vp.w.w)
	var raw: Array[Vector4] = [
		r3 + r0, # Left
		r3 - r0, # Right
		r3 + r1, # Bottom
		r3 - r1, # Top
		r2,      # Far (z >= 0)
		r3 - r2, # Near (z <= w)
	]
	var out: Array[Vector4] = []
	for pl in raw:
		var l := Vector3(pl.x, pl.y, pl.z).length()
		if l > 1e-6:
			out.append(pl / l)
		else:
			out.append(Vector4(0, 1, 0, 1e6))
	return out

func _harvest_timestamps() -> void:
	if _rd == null:
		return
	var sf := _rd.get_captured_timestamps_frame()
	if sf == _last_stamp_frame:
		return
	_last_stamp_frame = sf
	var cnt := _rd.get_captured_timestamps_count()
	if cnt <= 0:
		return
	var stamps := {}
	for i in range(cnt):
		var nm := _rd.get_captured_timestamp_name(i)
		if nm.begins_with("DI_"):
			stamps[nm] = _rd.get_captured_timestamp_gpu_time(i)
	if stamps.has("DI_S1_B") and stamps.has("DI_S1_E") and stamps.has("DI_S2_B") and stamps.has("DI_S2_E") and stamps.has("DI_S3_B") and stamps.has("DI_S3_E") and stamps.has("DI_S4_B") and stamps.has("DI_S4_E"):
		if _frame_counter > warmup_frames and _s1_samples.size() < measure_frames:
			var s1 := float(int(stamps["DI_S1_E"]) - int(stamps["DI_S1_B"])) / 1000000.0
			var s2 := float(int(stamps["DI_S2_E"]) - int(stamps["DI_S2_B"])) / 1000000.0
			var s3 := float(int(stamps["DI_S3_E"]) - int(stamps["DI_S3_B"])) / 1000000.0
			var s4 := float(int(stamps["DI_S4_E"]) - int(stamps["DI_S4_B"])) / 1000000.0
			_s1_samples.append(s1)
			_s2_samples.append(s2)
			_s3_samples.append(s3)
			_s4_samples.append(s4)
			_s34_samples.append(s3 + s4)

func _percentile(p_vals: Array[float], p_q: float) -> float:
	if p_vals.is_empty():
		return 0.0
	var s := p_vals.duplicate()
	s.sort()
	var idx := clampi(int(ceil(p_q * float(s.size()))) - 1, 0, s.size() - 1)
	return s[idx]

func _render_callback(_effect_callback_type: int, p_render_data: RenderData) -> void:
	if failed or done or not _ready_data:
		return
	if not _initialized_gpu:
		_init_gpu()
		if failed:
			return

	_harvest_timestamps()

	if _s1_samples.size() >= measure_frames:
		if not _readback_done:
			var cnt_bytes := _rd.buffer_get_data(_buf_count, 0, 8)
			if cnt_bytes.size() >= 8:
				_last_vis_clusters = cnt_bytes.decode_u32(0)
				_last_vis_tris = cnt_bytes.decode_u32(4)
			_readback_done = true
		var bytes_per_vis := 8 if path_mode == "c" else (24 if path_mode == "a" else (8 + int(round(float(_last_vis_tris) / maxf(float(_last_vis_clusters), 1.0) * 4.0))))
		var s3_p95 := _percentile(_s3_samples, 0.95)
		var s4_p95 := _percentile(_s4_samples, 0.95)
		var s34_p95 := _percentile(_s34_samples, 0.95)
		var cpu_p95 := _percentile(_cpu_submit_samples, 0.95)
		result_summary = {
			"path": path_mode,
			"resolution": "%dx%d" % [target_width, target_height],
			"frames_measured": _s1_samples.size(),
			"instances": _instance_count,
			"reachable_clusters": _pair_count,
			"reachable_triangles": _total_reachable_tris,
			"visible_clusters": _last_vis_clusters,
			"visible_triangles": _last_vis_tris,
			"s1_inst_cull_p95_ms": _percentile(_s1_samples, 0.95),
			"s2_cluster_cull_p95_ms": _percentile(_s2_samples, 0.95),
			"s3_compact_p95_ms": s3_p95,
			"s4_raster_p95_ms": s4_p95,
			"s3_plus_s4_p95_ms": s34_p95,
			"cpu_submit_p95_ms": cpu_p95,
			"g4_5_span_gpu_p95_ms": s3_p95,
			"bytes_per_visible_cluster": bytes_per_vis,
			"path_b_worst_case_mib": float(_total_reachable_tris) * 4.0 / (1024.0 * 1024.0),
		}
		done = true
		return

	var scene_data := p_render_data.get_render_scene_data()
	var cam_tf := scene_data.get_cam_transform()
	var proj := scene_data.get_cam_projection()
	var vp := proj * Projection(cam_tf.affine_inverse())
	var planes := _extract_planes(vp)

	# Reset indirect + counters BEFORE Stage 3 timestamp.
	var zero_cnt := PackedByteArray()
	zero_cnt.resize(16)
	_rd.buffer_update(_buf_count, 0, 16, zero_cnt)
	var init_ind := PackedByteArray()
	init_ind.resize(16)
	if path_mode == "c":
		init_ind.encode_u32(0, 384) # vertexCount = 3 * 128
		init_ind.encode_u32(4, 0)   # instanceCount = atomicAdd in compact
	elif path_mode == "b":
		init_ind.encode_u32(0, 0)   # vertexCount = atomicAdd in compact
		init_ind.encode_u32(4, 1)   # instanceCount = 1
	if path_mode != "a":
		_rd.buffer_update(_buf_indirect, 0, 16, init_ind)

	# Stage 1: Instance cull.
	var pc1 := PackedByteArray()
	pc1.resize(112)
	for i in range(6):
		pc1.encode_float(i * 16 + 0, planes[i].x)
		pc1.encode_float(i * 16 + 4, planes[i].y)
		pc1.encode_float(i * 16 + 8, planes[i].z)
		pc1.encode_float(i * 16 + 12, planes[i].w)
	pc1.encode_u32(96, _instance_count)

	_rd.capture_timestamp("DI_S1_B")
	var cl1 := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(cl1, _pipe_inst_cull)
	_rd.compute_list_bind_uniform_set(cl1, _us_inst_cull, 0)
	_rd.compute_list_set_push_constant(cl1, pc1, pc1.size())
	_rd.compute_list_dispatch(cl1, (_instance_count + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.capture_timestamp("DI_S1_E")

	# Stage 2: Cluster cull.
	var pc2 := PackedByteArray()
	pc2.resize(112)
	for i in range(6):
		pc2.encode_float(i * 16 + 0, planes[i].x)
		pc2.encode_float(i * 16 + 4, planes[i].y)
		pc2.encode_float(i * 16 + 8, planes[i].z)
		pc2.encode_float(i * 16 + 12, planes[i].w)
	pc2.encode_float(96, cam_tf.origin.x)
	pc2.encode_float(100, cam_tf.origin.y)
	pc2.encode_float(104, cam_tf.origin.z)
	pc2.encode_u32(108, _pair_count)

	_rd.capture_timestamp("DI_S2_B")
	var cl2 := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(cl2, _pipe_cluster_cull)
	_rd.compute_list_bind_uniform_set(cl2, _us_cluster_cull, 0)
	_rd.compute_list_set_push_constant(cl2, pc2, pc2.size())
	_rd.compute_list_dispatch(cl2, (_pair_count + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.capture_timestamp("DI_S2_E")

	# Stage 3: Compaction + indirect args write.
	var mode_u := 0 if path_mode == "c" else (1 if path_mode == "a" else 2)
	var pc3 := PackedByteArray()
	pc3.resize(16)
	pc3.encode_u32(0, _pair_count)
	pc3.encode_u32(4, mode_u)
	pc3.encode_u32(8, MAX_VIS_CLUSTERS)
	pc3.encode_u32(12, MAX_VIS_TRIS)

	_rd.capture_timestamp("DI_S3_B")
	var cl3 := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(cl3, _pipe_compact)
	_rd.compute_list_bind_uniform_set(cl3, _us_compact, 0)
	_rd.compute_list_set_push_constant(cl3, pc3, pc3.size())
	_rd.compute_list_dispatch(cl3, (_pair_count + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.capture_timestamp("DI_S3_E")

	# Stage 4: Visibility raster (timed on GPU and CPU submit).
	var pc4 := PackedByteArray()
	pc4.resize(64)
	var cols := [vp.x, vp.y, vp.z, vp.w]
	for ci in range(4):
		pc4.encode_float(ci * 16 + 0, cols[ci].x)
		pc4.encode_float(ci * 16 + 4, cols[ci].y)
		pc4.encode_float(ci * 16 + 8, cols[ci].z)
		pc4.encode_float(ci * 16 + 12, cols[ci].w)

	var t_cpu0 := Time.get_ticks_usec()
	_rd.capture_timestamp("DI_S4_B")
	var dl := _rd.draw_list_begin(
		_framebuffer,
		RenderingDevice.DRAW_CLEAR_ALL,
		PackedColorArray([Color(1.0, 1.0, 1.0, 1.0)]),
		0.0,
		0
	)
	_rd.draw_list_bind_render_pipeline(dl, _pipe_raster)
	_rd.draw_list_bind_uniform_set(dl, _us_raster, 0)
	_rd.draw_list_set_push_constant(dl, pc4, pc4.size())
	if path_mode == "a":
		_rd.draw_list_draw_indirect_count(dl, false, _buf_indirect, 0, _buf_count, 0, MAX_VIS_CLUSTERS, 16)
	else:
		_rd.draw_list_draw_indirect(dl, false, _buf_indirect, 0, 1, 16)
	_rd.draw_list_end()
	_rd.capture_timestamp("DI_S4_E")
	var t_cpu1 := Time.get_ticks_usec()

	_frame_counter += 1
	if _frame_counter > warmup_frames and _cpu_submit_samples.size() < measure_frames:
		_cpu_submit_samples.append(float(t_cpu1 - t_cpu0) / 1000.0)

func cleanup_gpu() -> void:
	if _rd == null or not _initialized_gpu:
		return
	for rid in [
		_us_inst_cull, _us_cluster_cull, _us_compact, _us_raster,
		_pipe_inst_cull, _pipe_cluster_cull, _pipe_compact, _pipe_raster,
		_sh_inst_cull, _sh_cluster_cull, _sh_compact, _sh_raster,
		_framebuffer, _tex_vis, _tex_depth,
		_buf_instances, _buf_inst_vis, _buf_pairs, _buf_clusters, _buf_cluster_vis,
		_buf_verts, _buf_tris, _buf_vis_clusters, _buf_vis_tris, _buf_indirect, _buf_count,
	]:
		if rid.is_valid():
			_rd.free_rid(rid)
	_initialized_gpu = false
