#[compute]

#version 450

#VERSION_DEFINES

// NextGI GPU-written TLAS instance records (#57, cut item 1, §9 RT API item 9).
//
// One thread per element encodes an instance into the API's record layout so a compute pass can feed
// tlas_build_from_buffer without a CPU readback. The source is the element's own GPU buffer: the
// MultiMesh transform buffer (3x4 floats, stride from the multimesh) or the particle instance buffer
// (five vec4 per element, stride 20 floats). Only the first three rows are read.
//
// INSTANCE_64 is the Vulkan VkAccelerationStructureInstanceKHR layout, INSTANCE_72 the Metal
// IndirectAccelerationStructureInstanceDescriptor layout.
//
// The masks follow the plan: an element beyond 30 m whose angular radius is under 0.5 degrees drops
// to mask 0 (NOT_IN_RT_SCENE), and so does an inactive element (particles_copy writes a -INF origin).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer NextGIRtInstanceSource {
	float data[];
}
source;

layout(set = 0, binding = 1, std430) restrict writeonly buffer NextGIRtInstanceDestination {
	uint data[];
}
destination;

// Indirect sources (a MultiMesh command buffer) publish their element count on the GPU. The dummy
// buffer holds 0 and the indirect flag is off for every other source.
layout(set = 0, binding = 2, std430) restrict readonly buffer NextGIRtInstanceCount {
	uint count;
}
count_buffer;

layout(push_constant, std430) uniform Params {
	uvec4 counts; // x = element count, y = source element offset, z = destination instance offset, w = id base.
	uvec4 info; // x = source stride in floats, y = visible count, z = API flags, w = instance mask.
	vec4 camera; // xyz = camera position, w = maximum distance for the angular mask (30 m).
	uvec4 refs; // x = BLAS reference low, y = high, z and w unused.
	vec4 limits; // x = angular radius limit in radians (0.5 degrees).
	uvec4 extra; // x = 1 when the element count comes from count_buffer.
}
params;

// The particle copy shader parks inactive elements at -INF, which this test catches along with NaN.
bool nextgi_instance_is_finite(vec3 p_position) {
	return all(lessThan(abs(p_position), vec3(3.0e38)));
}

vec4 nextgi_instance_row(uint p_element, uint p_row) {
	const uint base = (p_element - params.counts.y) * params.info.x + p_row * 4u;
	return vec4(source.data[base + 0u], source.data[base + 1u], source.data[base + 2u], source.data[base + 3u]);
}

uint nextgi_instance_mask(bool p_active, vec3 p_origin, float p_scale) {
	if (!p_active || !nextgi_instance_is_finite(p_origin)) {
		return 0u;
	}
	const float distance = length(p_origin - params.camera.xyz);
	if (distance <= params.camera.w) {
		return params.info.w;
	}
	// Small on screen and far away: not worth a TLAS slot. The bound is the largest basis length.
	const float angular = atan(p_scale / max(distance, 1e-4));
	return angular < params.limits.x ? 0u : params.info.w;
}

void main() {
	uint count = params.counts.x;
	if (params.extra.x != 0u) {
		count = min(count, count_buffer.count);
	}
	const uint index = gl_GlobalInvocationID.x;
	if (index >= count) {
		return;
	}

	const uint element = params.counts.y + index;
	const vec4 row0 = nextgi_instance_row(element, 0u);
	const vec4 row1 = nextgi_instance_row(element, 1u);
	const vec4 row2 = nextgi_instance_row(element, 2u);
	const vec3 origin = vec3(row0.w, row1.w, row2.w);
	const float scale = max(max(length(row0.xyz), length(row1.xyz)), length(row2.xyz));
	const bool is_active = index < params.info.y;
	const uint mask = nextgi_instance_mask(is_active, origin, scale);
	const uint instance_id = params.counts.w + index;

#ifdef INSTANCE_64
	// VkAccelerationStructureInstanceKHR: 12 transform floats, packed id/mask, packed sbt/flags, u64 ref.
	const uint base = (params.counts.z + index) * 16u;
	destination.data[base + 0u] = floatBitsToUint(row0.x);
	destination.data[base + 1u] = floatBitsToUint(row0.y);
	destination.data[base + 2u] = floatBitsToUint(row0.z);
	destination.data[base + 3u] = floatBitsToUint(row0.w);
	destination.data[base + 4u] = floatBitsToUint(row1.x);
	destination.data[base + 5u] = floatBitsToUint(row1.y);
	destination.data[base + 6u] = floatBitsToUint(row1.z);
	destination.data[base + 7u] = floatBitsToUint(row1.w);
	destination.data[base + 8u] = floatBitsToUint(row2.x);
	destination.data[base + 9u] = floatBitsToUint(row2.y);
	destination.data[base + 10u] = floatBitsToUint(row2.z);
	destination.data[base + 11u] = floatBitsToUint(row2.w);
	destination.data[base + 12u] = (instance_id & 0x00FFFFFFu) | ((mask & 0xFFu) << 24u);
	// The SBT offset is 0 for ray-query-only instances (plan D5); the TLAS records no zero-SBT flag here.
	destination.data[base + 13u] = (params.info.z & 0xFFu) << 24u;
	destination.data[base + 14u] = params.refs.x;
	destination.data[base + 15u] = params.refs.y;
#elif defined(INSTANCE_72)
	// IndirectAccelerationStructureInstanceDescriptor: four PackedFloat3 columns, options, mask, IFT, userID, ResourceID.
	const uint base = (params.counts.z + index) * 18u;
	destination.data[base + 0u] = floatBitsToUint(row0.x);
	destination.data[base + 1u] = floatBitsToUint(row1.x);
	destination.data[base + 2u] = floatBitsToUint(row2.x);
	destination.data[base + 3u] = floatBitsToUint(row0.y);
	destination.data[base + 4u] = floatBitsToUint(row1.y);
	destination.data[base + 5u] = floatBitsToUint(row2.y);
	destination.data[base + 6u] = floatBitsToUint(row0.z);
	destination.data[base + 7u] = floatBitsToUint(row1.z);
	destination.data[base + 8u] = floatBitsToUint(row2.z);
	destination.data[base + 9u] = floatBitsToUint(row0.w);
	destination.data[base + 10u] = floatBitsToUint(row1.w);
	destination.data[base + 11u] = floatBitsToUint(row2.w);
	destination.data[base + 12u] = params.info.z;
	destination.data[base + 13u] = mask;
	destination.data[base + 14u] = 0u;
	destination.data[base + 15u] = instance_id;
	destination.data[base + 16u] = params.refs.x;
	destination.data[base + 17u] = params.refs.y;
#endif
}
