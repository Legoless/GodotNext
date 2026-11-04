#[compute]

#version 450

#VERSION_DEFINES

// NextGI GPU-written TLAS instance records (#57, cut item 1, §9 RT API item 9).
//
// One thread per element encodes an instance into the API's record layout so a compute pass can feed
// tlas_build_from_buffer without a CPU readback. INSTANCE_64 is the Vulkan
// VkAccelerationStructureInstanceKHR layout, INSTANCE_72 the Metal
// IndirectAccelerationStructureInstanceDescriptor layout.
//
// The masks follow the plan: an element beyond 30 m whose angular radius is under 0.5 degrees drops
// to mask 0 (NOT_IN_RT_SCENE), and so does an inactive element (particles_copy writes a -INF origin).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// Row-major 3x4 transform, instance id, API flags and an active flag.
struct SourceInstance {
	vec4 row0;
	vec4 row1;
	vec4 row2;
	uint id;
	uint flags;
	uint is_active;
	uint pad;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer NextGIRtInstanceSource {
	SourceInstance data[];
}
sources;

layout(set = 0, binding = 1, std430) restrict writeonly buffer NextGIRtInstanceDestination {
	uint data[];
}
destination;

layout(push_constant, std430) uniform Params {
	uvec4 counts; // x = element count, y = source offset, z = destination instance offset, w = unused.
	vec4 camera; // xyz = camera position, w = maximum distance for the angular mask (30 m).
	uvec4 refs; // x = BLAS reference low, y = high, z = instance mask, w = unused.
	vec4 limits; // x = angular radius limit in radians (0.5 degrees).
}
params;

// The particle copy shader parks inactive elements at -INF, which this test catches along with NaN.
bool nextgi_instance_is_finite(vec3 p_position) {
	return all(lessThan(abs(p_position), vec3(3.0e38)));
}

uint nextgi_instance_mask(const SourceInstance p_instance, vec3 p_origin, float p_scale) {
	if (p_instance.is_active == 0u || !nextgi_instance_is_finite(p_origin)) {
		return 0u;
	}
	const float distance = length(p_origin - params.camera.xyz);
	if (distance <= params.camera.w) {
		return params.refs.z;
	}
	// Small on screen and far away: not worth a TLAS slot. The bound is the largest basis length.
	const float angular = atan(p_scale / max(distance, 1e-4));
	return angular < params.limits.x ? 0u : params.refs.z;
}

void main() {
	const uint index = gl_GlobalInvocationID.x;
	if (index >= params.counts.x) {
		return;
	}

	const SourceInstance instance = sources.data[params.counts.y + index];
	const vec3 origin = vec3(instance.row0.w, instance.row1.w, instance.row2.w);
	const float scale = max(max(length(instance.row0.xyz), length(instance.row1.xyz)), length(instance.row2.xyz));
	const uint mask = nextgi_instance_mask(instance, origin, scale);
	const uint flags = instance.flags & 0x0Fu;

#ifdef INSTANCE_64
	// VkAccelerationStructureInstanceKHR: 12 transform floats, packed id/mask, packed sbt/flags, u64 ref.
	const uint base = (params.counts.z + index) * 16u;
	destination.data[base + 0u] = floatBitsToUint(instance.row0.x);
	destination.data[base + 1u] = floatBitsToUint(instance.row0.y);
	destination.data[base + 2u] = floatBitsToUint(instance.row0.z);
	destination.data[base + 3u] = floatBitsToUint(instance.row0.w);
	destination.data[base + 4u] = floatBitsToUint(instance.row1.x);
	destination.data[base + 5u] = floatBitsToUint(instance.row1.y);
	destination.data[base + 6u] = floatBitsToUint(instance.row1.z);
	destination.data[base + 7u] = floatBitsToUint(instance.row1.w);
	destination.data[base + 8u] = floatBitsToUint(instance.row2.x);
	destination.data[base + 9u] = floatBitsToUint(instance.row2.y);
	destination.data[base + 10u] = floatBitsToUint(instance.row2.z);
	destination.data[base + 11u] = floatBitsToUint(instance.row2.w);
	destination.data[base + 12u] = (instance.id & 0x00FFFFFFu) | ((mask & 0xFFu) << 24u);
	// The SBT offset is 0 for ray-query-only instances (plan D5); the TLAS records no zero-SBT flag here.
	destination.data[base + 13u] = (flags & 0xFFu) << 24u;
	destination.data[base + 14u] = params.refs.x;
	destination.data[base + 15u] = params.refs.y;
#elif defined(INSTANCE_72)
	// IndirectAccelerationStructureInstanceDescriptor: four PackedFloat3 columns, options, mask, IFT, userID, ResourceID.
	const uint base = (params.counts.z + index) * 18u;
	destination.data[base + 0u] = floatBitsToUint(instance.row0.x);
	destination.data[base + 1u] = floatBitsToUint(instance.row1.x);
	destination.data[base + 2u] = floatBitsToUint(instance.row2.x);
	destination.data[base + 3u] = floatBitsToUint(instance.row0.y);
	destination.data[base + 4u] = floatBitsToUint(instance.row1.y);
	destination.data[base + 5u] = floatBitsToUint(instance.row2.y);
	destination.data[base + 6u] = floatBitsToUint(instance.row0.z);
	destination.data[base + 7u] = floatBitsToUint(instance.row1.z);
	destination.data[base + 8u] = floatBitsToUint(instance.row2.z);
	destination.data[base + 9u] = floatBitsToUint(instance.row0.w);
	destination.data[base + 10u] = floatBitsToUint(instance.row1.w);
	destination.data[base + 11u] = floatBitsToUint(instance.row2.w);
	destination.data[base + 12u] = flags;
	destination.data[base + 13u] = mask;
	destination.data[base + 14u] = 0u;
	destination.data[base + 15u] = instance.id;
	destination.data[base + 16u] = params.refs.x;
	destination.data[base + 17u] = params.refs.y;
#endif
}
