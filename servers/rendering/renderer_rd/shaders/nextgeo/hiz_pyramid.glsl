#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_gpu_cull (#76). Farthest-depth 2x2 reduction of the
// previous/current depth buffer (reverse-Z clears to 0.0, so the farthest
// depth is the minimum).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef CLEAR_VISIBILITY
layout(set = 0, binding = 0, r32ui) uniform restrict writeonly uimage2D target_visibility;
layout(set = 0, binding = 1, r32ui) uniform restrict writeonly uimage2D target_instance_id;
layout(push_constant, std430) uniform Params {
	uvec2 target_size;
	uvec2 pad;
}
params;

void main() {
	uvec2 coord = gl_GlobalInvocationID.xy;
	if (coord.x >= params.target_size.x || coord.y >= params.target_size.y) {
		return;
	}
	imageStore(target_visibility, ivec2(coord), uvec4(0xFFFFFFFFu));
	imageStore(target_instance_id, ivec2(coord), uvec4(0xFFFFFFFFu));
}
#else
layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1, r32f) uniform restrict writeonly image2D target_hiz;
layout(push_constant, std430) uniform Params {
	uvec2 source_size;
	uvec2 target_size;
}
params;

void main() {
	uvec2 coord = gl_GlobalInvocationID.xy;
	if (coord.x >= params.target_size.x || coord.y >= params.target_size.y) {
		return;
	}
	uvec2 source = coord * 2u;
	float depth = 1.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			uvec2 sample_coord = min(source + uvec2(x, y), params.source_size - uvec2(1u));
			depth = min(depth, texelFetch(source_depth, ivec2(sample_coord), 0).r);
		}
	}
	imageStore(target_hiz, ivec2(coord), vec4(depth, depth, depth, depth));
}
#endif
