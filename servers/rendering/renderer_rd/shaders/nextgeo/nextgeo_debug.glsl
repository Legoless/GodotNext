#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0;
}

#[fragment]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_exclusive_dispatch (#79, plan §9, §12, §13.4 Q5 & Q14).
// Fullscreen producer for:
//   mode 0 = NEXTGEO_CRACKS
//   mode 1 = NEXTGEO_CLUSTER_ERROR
//   mode 2 = NEXTGEO_RESIDENCY
//   mode 3 = NEXTGEO_VISIBILITY_ID
//   mode 4 = NEXTGEO_FALLBACK
//   mode 5 = NEXTGEO_TRIANGLE_SIZE

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

layout(location = 0) in vec2 uv_interp;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform usampler2D visibility_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform usampler2D fallback_class_tex;

layout(set = 0, binding = 3, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 4, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;

layout(set = 0, binding = 5, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 6, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

layout(set = 0, binding = 7, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

layout(set = 0, binding = 8, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

layout(set = 0, binding = 9, std430) buffer restrict readonly Dag {
	uint data[];
}
dag;

layout(set = 0, binding = 10, std430) buffer restrict readonly Residency {
	uint data[];
}
residency;

layout(set = 0, binding = 11, std430) buffer restrict readonly DebugParams {
	mat4 proj;
	mat4 view;
	mat4 view_proj;
	vec4 camera; // xyz, w = max_pixel_error
	vec4 view_info; // x = width, y = height, z = z_near, w = lod_bias
}
dbg;

layout(set = 0, binding = 12, std430) buffer restrict Histogram {
	uint bins[16];
}
histogram;

layout(push_constant, std430) uniform PushParams {
	vec2 internal_size;
	vec2 output_size;
	uint mode;
	uint visible_capacity;
	uint bucket_table_words;
	float cull_sign;
	uint record_count;
	uint mesh_count;
	uint using_nextgeo;
	uint is_orthogonal;
}
push_params;

const uint MODE_CRACKS = 0u;
const uint MODE_CLUSTER_ERROR = 1u;
const uint MODE_RESIDENCY = 2u;
const uint MODE_VISIBILITY_ID = 3u;
const uint MODE_FALLBACK = 4u;
const uint MODE_TRIANGLE_SIZE = 5u;

uint mix32(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

vec3 hash_color(uint h) {
	uint x = mix32(h + 0x9e3779b9u);
	vec3 c = vec3(
			float(x & 0xFFu) / 255.0,
			float((x >> 8u) & 0xFFu) / 255.0,
			float((x >> 16u) & 0xFFu) / 255.0);
	return mix(vec3(0.20), vec3(0.95), c);
}

vec3 heat_ramp(float t) {
	t = clamp(t, 0.0, 1.0);
	if (t < 0.33) {
		return mix(vec3(0.08, 0.25, 0.85), vec3(0.10, 0.80, 0.45), t / 0.33);
	} else if (t < 0.66) {
		return mix(vec3(0.10, 0.80, 0.45), vec3(0.95, 0.85, 0.12), (t - 0.33) / 0.33);
	}
	return mix(vec3(0.95, 0.85, 0.12), vec3(0.95, 0.12, 0.10), (t - 0.66) / 0.34);
}

vec3 fallback_reason_color(uint reason, uint cls, uint rt_excl) {
	if (cls == 2u) {
		// Non-VGD legacy opaque surface.
		return rt_excl != 0u ? vec3(0.38, 0.32, 0.42) : vec3(0.30, 0.32, 0.36);
	}
	vec3 base_col;
	switch (reason) {
		case 1u: // SKINNING
			base_col = vec3(0.95, 0.45, 0.12);
			break;
		case 2u: // INSTANCE_UNIFORMS
			base_col = vec3(0.92, 0.78, 0.15);
			break;
		case 3u: // LIGHTMAPPED_INSTANCE
			base_col = vec3(0.65, 0.40, 0.92);
			break;
		case 4u: // NEXT_PASS_OR_OVERLAY
			base_col = vec3(0.95, 0.20, 0.55);
			break;
		case 5u: // MSAA
			base_col = vec3(0.20, 0.75, 0.95);
			break;
		case 6u: // MULTIVIEW
			base_col = vec3(0.15, 0.55, 0.95);
			break;
		case 7u: // DEBUG_DRAW
			base_col = vec3(0.55, 0.55, 0.90);
			break;
		case 8u: // MUTATED_MESH
			base_col = vec3(0.95, 0.12, 0.12);
			break;
		case 9u: // BAKED_GRIDMAP
			base_col = vec3(0.58, 0.42, 0.24);
			break;
		case 10u: // VERTEX_LIGHTING
			base_col = vec3(0.90, 0.60, 0.28);
			break;
		case 11u: // UNSUPPORTED_CUSTOM_SHADER
			base_col = vec3(0.88, 0.28, 0.85);
			break;
		case 12u: // UNSUPPORTED_DERIVATIVE
			base_col = vec3(0.95, 0.38, 0.38);
			break;
		case 13u: // ALPHA
			base_col = vec3(0.25, 0.88, 0.82);
			break;
		case 14u: // MULTIMESH_OR_PARTICLES / VGD_IGNORED
			base_col = vec3(0.45, 0.72, 0.25);
			break;
		default:
			base_col = vec3(0.82, 0.52, 0.22);
			break;
	}
	if (rt_excl != 0u) {
		base_col = mix(base_col, vec3(1.0, 0.95, 0.25), 0.25);
	}
	return base_col;
}

bool mesh_group_refine_ok(uint p_mesh_idx, uint p_group_idx) {
	if (p_mesh_idx >= residency.data[1]) {
		return true;
	}
	uint hdr = 2u + p_mesh_idx * 4u;
	uint res_groups = residency.data[hdr + 2u];
	if (p_group_idx >= res_groups) {
		return false;
	}
	uint refine_off = residency.data[hdr + 0u];
	uint word = residency.data[refine_off + (p_group_idx >> 5u)];
	return (word & (1u << (p_group_idx & 31u))) != 0u;
}

vec3 instance_point(NextGeoInstanceRecord p_record, vec3 p_local) {
	return vec3(
			dot(p_record.transform[0].xyz, p_local) + p_record.transform[0].w,
			dot(p_record.transform[1].xyz, p_local) + p_record.transform[1].w,
			dot(p_record.transform[2].xyz, p_local) + p_record.transform[2].w);
}

float projected_error(NextGeoInstanceRecord p_record, vec4 p_lod_sphere, float p_max_parent_error) {
	if (p_max_parent_error >= 3.4e38) {
		return dbg.camera.w + 1.0;
	}
	float scale = p_record.world_aabb_min.w;
	if (push_params.is_orthogonal != 0u) {
		return p_max_parent_error * scale * dbg.view_info.x * dbg.view_info.w;
	}
	vec3 center = instance_point(p_record, p_lod_sphere.xyz);
	float radius = p_lod_sphere.w * scale;
	float dist = max(length(center - dbg.camera.xyz) - radius, max(dbg.view_info.z, 1e-4));
	return p_max_parent_error * scale * 0.5 * dbg.view_info.y * abs(dbg.proj[1][1]) / dist * dbg.view_info.w;
}

void main() {
	vec2 uv = clamp(gl_FragCoord.xy / max(push_params.output_size, vec2(1.0)), vec2(0.0), vec2(1.0));
	ivec2 ipx = clamp(ivec2(uv * push_params.internal_size), ivec2(0), ivec2(push_params.internal_size) - ivec2(1));
	bool is_canonical_px = ivec2(floor((vec2(ipx) + vec2(0.5)) * push_params.output_size / max(push_params.internal_size, vec2(1.0)))) == ivec2(gl_FragCoord.xy);

	float raw_depth = texelFetch(depth_tex, ipx, 0).r;
	uint fb_raw = texelFetch(fallback_class_tex, ipx, 0).r;
	uint vis_id = (push_params.using_nextgeo != 0u) ? texelFetch(visibility_tex, ipx, 0).r : 0xFFFFFFFFu;

	if (push_params.mode == MODE_CRACKS) {
		// In MODE_CRACKS, fb_raw == 1u means an exposed legacy closed-surface backface passed depth test,
		// and fb_raw == 2u means a legacy opaque front face won at this pixel.
		if (fb_raw == 1u) {
			frag_color = vec4(1.0, 0.0, 1.0, 1.0);
			return;
		}
		if (fb_raw == 2u) {
			frag_color = vec4(0.46, 0.46, 0.46, 1.0);
			return;
		}
		if (vis_id == 0xFFFFFFFFu) {
			frag_color = (raw_depth > 0.0) ? vec4(0.46, 0.46, 0.46, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
			return;
		}
	} else {
		// In all other modes, fb_raw != 0u means a legacy opaque surface won the depth test at ipx.
		uint fb_cls = fb_raw & 3u;
		uint fb_reason = (fb_raw >> 2u) & 0xFu;
		uint fb_rt_excl = (fb_raw >> 6u) & 3u;

		if (push_params.mode == MODE_FALLBACK) {
			if (fb_cls != 0u) {
				frag_color = vec4(fallback_reason_color(fb_reason, fb_cls, fb_rt_excl), 1.0);
				return;
			}
			if (vis_id == 0xFFFFFFFFu) {
				frag_color = vec4(0.02, 0.02, 0.03, 1.0);
				return;
			}
		} else {
			if (fb_cls != 0u) {
				if (push_params.mode == MODE_TRIANGLE_SIZE && is_canonical_px) {
					atomicAdd(histogram.bins[8], 1u);
				}
				frag_color = vec4(0.24, 0.25, 0.28, 1.0);
				return;
			}
			if (vis_id == 0xFFFFFFFFu) {
				frag_color = vec4(0.02, 0.02, 0.03, 1.0);
				return;
			}
		}
	}

	// Decode visible virtual cluster & triangle.
	uint vis_cluster = vis_id >> 7u;
	uint tri_idx = vis_id & 0x7Fu;
	if (vis_cluster >= push_params.visible_capacity) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.05, 0.05, 0.05, 1.0);
		return;
	}

	uvec2 entry = visible_clusters.data[vis_cluster];
	if (entry.x >= push_params.record_count) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.05, 0.05, 0.05, 1.0);
		return;
	}
	NextGeoInstanceRecord record = instances.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || record.virtual_mesh >= push_params.mesh_count) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.05, 0.05, 0.05, 1.0);
		return;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.05, 0.05, 0.05, 1.0);
		return;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.05, 0.05, 0.05, 1.0);
		return;
	}

	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint stride = desc.z & 0xFFu;
	uint vert_count = (desc.z >> 8u) & 0xFFu;
	uint tri_count = (desc.z >> 16u) & 0xFFu;
	uint surface_id = desc.w & 0xFFFFu;
	uint page_id = desc.w >> 16u;

	if (push_params.mode == MODE_FALLBACK) {
		// Virtual surface pixel: emerald green, or teal when excluded from HWRT TLAS (e.g. RT_FAR or no BLAS).
		bool rt_excluded = (record.flags & NEXTGEO_RECORD_RT_FAR) != 0u || (record.tlas_instance_index == 0xFFFFFFFFu && record.blas_handle == 0xFFFFFFFFu);
		vec3 vcol = rt_excluded ? vec3(0.14, 0.72, 0.62) : vec3(0.14, 0.82, 0.34);
		frag_color = vec4(vcol, 1.0);
		return;
	}

	if (push_params.mode == MODE_VISIBILITY_ID) {
		vec3 cluster_col = hash_color(entry.x * 0x9e3779b9u ^ entry.y);
		vec3 tri_col = hash_color((entry.y << 8u) ^ tri_idx);
		frag_color = vec4(mix(cluster_col, tri_col, 0.25), 1.0);
		return;
	}

	if (push_params.mode == MODE_CLUSTER_ERROR || push_params.mode == MODE_RESIDENCY) {
		uint mesh_idx = record.virtual_mesh;
		uint slot_page_size = residency.data[0];
		uint page_slot = 0xFFFFFFFFu;
		uint page_first_cluster = 0u;
		if (mesh_idx < residency.data[1]) {
			uint hdr = 2u + mesh_idx * 4u;
			uint page_table_off = residency.data[hdr + 1u];
			uint res_pages = residency.data[hdr + 3u];
			if (page_id < res_pages) {
				page_slot = residency.data[page_table_off + page_id * 2u + 0u];
				page_first_cluster = residency.data[page_table_off + page_id * 2u + 1u];
			}
		}
		float px_err = 0.0;
		int parent_lod = -1;
		int refined_group = -1;
		bool wanted_finer = false;
		if (page_slot != 0xFFFFFFFFu && slot_page_size >= 96u && local_cluster >= page_first_cluster) {
			uint page_word_base = (page_slot * slot_page_size) >> 2u;
			uint page_clusters = pages.data[page_word_base];
			uint in_page = local_cluster - page_first_cluster;
			if (in_page < page_clusters) {
				uint rec_word = page_word_base + 4u + in_page * 20u;
				vec4 own_lod = vec4(
						uintBitsToFloat(pages.data[rec_word + 5u]),
						uintBitsToFloat(pages.data[rec_word + 6u]),
						uintBitsToFloat(pages.data[rec_word + 7u]),
						uintBitsToFloat(pages.data[rec_word + 8u]));
				float own_error = uintBitsToFloat(pages.data[rec_word + 9u]);
				parent_lod = int(pages.data[rec_word + 14u]);
				refined_group = int(pages.data[rec_word + 15u]);
				px_err = projected_error(record, own_lod, own_error);
				uint group_count = (mesh.bvh_node_offset > mesh.group_offset) ? ((mesh.bvh_node_offset - mesh.group_offset) / 60u) : 0u;
				if (refined_group >= 0 && uint(refined_group) < group_count && px_err > dbg.camera.w) {
					wanted_finer = !mesh_group_refine_ok(mesh_idx, uint(refined_group));
				}
			}
		}
		if (push_params.mode == MODE_CLUSTER_ERROR) {
			float norm_err = clamp(px_err / max(dbg.camera.w * 2.0, 0.25), 0.0, 1.0);
			frag_color = vec4(heat_ramp(norm_err), 1.0);
			return;
		}
		// MODE_RESIDENCY:
		//   wanted non-resident finer page (coarse fallback) -> warm amber/orange
		//   pinned root cluster -> cool cyan/blue
		//   resident streamed cut cluster -> emerald green
		vec3 res_col;
		if (wanted_finer) {
			res_col = vec3(0.96, 0.48, 0.12);
		} else if (parent_lod < 0) {
			res_col = vec3(0.18, 0.62, 0.95);
		} else {
			res_col = vec3(0.16, 0.85, 0.42);
		}
		frag_color = vec4(res_col, 1.0);
		return;
	}

	// MODE_CRACKS and MODE_TRIANGLE_SIZE need the 3 projected triangle corners.
	if (tri_idx >= tri_count || stride < 3u || vert_count == 0u) {
		frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.1, 0.1, 0.1, 1.0);
		return;
	}

	vec4 tri_clip[3];
	vec3 tri_view[3];
	for (uint c = 0u; c < 3u; c++) {
		uint ib = desc.y + tri_idx * 3u + c;
		uint vi = (pages.data[ib >> 2u] >> ((ib & 3u) * 8u)) & 0xFFu;
		if (vi >= vert_count) {
			frag_color = (push_params.mode == MODE_CRACKS) ? vec4(0.45, 0.45, 0.45, 1.0) : vec4(0.1, 0.1, 0.1, 1.0);
			return;
		}
		uint pw = desc.x + vi * stride;
		vec3 lp = vec3(
				uintBitsToFloat(pages.data[pw + 0u]),
				uintBitsToFloat(pages.data[pw + 1u]),
				uintBitsToFloat(pages.data[pw + 2u]));
		vec3 wp = instance_point(record, lp);
		vec3 vp = (dbg.view * vec4(wp, 1.0)).xyz;
		tri_view[c] = vp;
		tri_clip[c] = dbg.proj * vec4(vp, 1.0);
	}

	if (push_params.mode == MODE_TRIANGLE_SIZE) {
		vec2 sp[3];
		for (uint c = 0u; c < 3u; c++) {
			vec2 ndc = tri_clip[c].xy / max(abs(tri_clip[c].w), 1e-6);
			sp[c] = (ndc * 0.5 + 0.5) * push_params.internal_size;
		}
		float area_px = 0.5 * abs((sp[1].x - sp[0].x) * (sp[2].y - sp[0].y) - (sp[2].x - sp[0].x) * (sp[1].y - sp[0].y));
		int bin = clamp(int(floor(log2(max(area_px, 0.5)))) + 1, 0, 15);
		if (is_canonical_px) {
			atomicAdd(histogram.bins[bin], 1u);
		}
		frag_color = vec4(heat_ramp(1.0 - float(bin) / 15.0), 1.0);
		return;
	}

	// MODE_CRACKS: exact (255, 0, 255) on virtual closed-surface backfaces, neutral gray [0.35, 0.60] on front faces.
	uint bucket_flags = 0u;
	if (push_params.bucket_table_words > 0u && surface_id < 32u) {
		uint bucket_slot = record.bucket_base + surface_id;
		if (bucket_slot < push_params.bucket_table_words) {
			bucket_flags = bucket_table.data[bucket_slot];
		}
	}
	bool cull_disabled = (bucket_flags & (1u << 24u)) != 0u || (surface_id < 32u && (record.pad1[0] & (1u << surface_id)) != 0u);
	bool cull_front = (bucket_flags & (1u << 25u)) != 0u || (surface_id < 32u && (record.pad1[1] & (1u << surface_id)) != 0u);

	float det2d = determinant(mat3(tri_clip[0].xyw, tri_clip[1].xyw, tri_clip[2].xyw));
	float effective_cull_sign = cull_front ? -push_params.cull_sign : push_params.cull_sign;
	mat3 basis = mat3(record.transform[0].xyz, record.transform[1].xyz, record.transform[2].xyz);
	float orient_sign = (determinant(basis) * effective_cull_sign < 0.0) ? -1.0 : 1.0;

	if (!cull_disabled && (det2d * orient_sign < 0.0)) {
		frag_color = vec4(1.0, 0.0, 1.0, 1.0);
		return;
	}

	vec3 fn = cross(tri_view[2] - tri_view[0], tri_view[1] - tri_view[0]);
	float fn_len = length(fn);
	float nz = (fn_len > 1e-8) ? abs(fn.z / fn_len) : 0.5;
	float gray = clamp(0.38 + 0.20 * nz, 0.35, 0.60);
	frag_color = vec4(vec3(gray), 1.0);
}
