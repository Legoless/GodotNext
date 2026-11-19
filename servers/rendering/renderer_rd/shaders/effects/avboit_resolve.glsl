#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: avboit (#295). The AVBOIT resolve: the composite's accumulation target is a
// premultiplied layer (the weighted sum of the translucent surfaces in rgb, their coverage
// 1 - prod(1 - alpha) in a), and goes over the view's color as the sorted pass's mix blend
// would leave it. A pixel no translucent surface reached holds 0 and keeps its color exactly.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D accumulation;
layout(rgba16f, set = 0, binding = 1) uniform restrict image2D color;

layout(push_constant, std430) uniform Params {
	ivec2 offset;
	ivec2 size;
}
params;

void main() {
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(local, params.size))) {
		return;
	}
	ivec2 pos = params.offset + local;
	vec4 layer = texelFetch(accumulation, pos, 0);
	vec4 dest = imageLoad(color, pos);
	dest.rgb = layer.rgb + dest.rgb * (1.0 - layer.a);
	dest.a = layer.a + dest.a * (1.0 - layer.a);
	imageStore(color, pos, dest);
}
