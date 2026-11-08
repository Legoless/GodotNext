#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_exclusive_dispatch (#79, Q14).
// Reduces the per-pixel fallback class buffer + NextGeo visibility buffer into:
//   counts[0] = n1 (legacy-drawn surface of a VGD mesh)
//   counts[1] = n2 (other legacy opaque surface)
//   counts[2] = n3 (virtual NextGeo opaque pixel)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform usampler2D fallback_class_tex;
layout(set = 0, binding = 1) uniform usampler2D visibility_tex;

layout(set = 0, binding = 2, std430) buffer restrict Counts {
	uint n1;
	uint n2;
	uint n3;
	uint pad;
}
counts;

layout(push_constant, std430) uniform PushParams {
	uint width;
	uint height;
	uint using_nextgeo;
	uint pad0;
}
push_params;

shared uint wg_n1;
shared uint wg_n2;
shared uint wg_n3;

void main() {
	if (gl_LocalInvocationIndex == 0u) {
		wg_n1 = 0u;
		wg_n2 = 0u;
		wg_n3 = 0u;
	}
	barrier();

	uvec2 px = gl_GlobalInvocationID.xy;
	if (px.x < push_params.width && px.y < push_params.height) {
		ivec2 ipx = ivec2(px);
		uint fb = texelFetch(fallback_class_tex, ipx, 0).r & 3u;
		if (fb == 1u) {
			atomicAdd(wg_n1, 1u);
		} else if (fb == 2u) {
			atomicAdd(wg_n2, 1u);
		} else if (push_params.using_nextgeo != 0u) {
			uint vis = texelFetch(visibility_tex, ipx, 0).r;
			if (vis != 0xFFFFFFFFu) {
				atomicAdd(wg_n3, 1u);
			}
		}
	}
	barrier();

	if (gl_LocalInvocationIndex == 0u) {
		if (wg_n1 > 0u) {
			atomicAdd(counts.n1, wg_n1);
		}
		if (wg_n2 > 0u) {
			atomicAdd(counts.n2, wg_n2);
		}
		if (wg_n3 > 0u) {
			atomicAdd(counts.n3, wg_n3);
		}
	}
}
