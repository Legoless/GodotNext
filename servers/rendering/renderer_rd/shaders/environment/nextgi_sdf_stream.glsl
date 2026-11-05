#[compute]

#version 460

#VERSION_DEFINES

// Mesh SDF mip requests (#185, plan §21.2 L3). One invocation per instance
// record; writes the mip the camera distance asks for. The CPU reads the
// buffer back one frame later and streams the mips in.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct SDFInstance {
	vec4 world_to_local[3];
	vec4 world_position; // w = scale bound
	uint sdf_id;
	uint reason;
	uint flags;
	uint required_mip;
	uint mip_count;
	uint pad[3];
};

layout(set = 0, binding = 0, std430) restrict buffer SDFInstances {
	SDFInstance data[];
} sdf_instances;

layout(push_constant, std430) uniform Params {
	vec4 camera;
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
} params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	uint mip_count = max(sdf_instances.data[index].mip_count, 1u);
	if (sdf_instances.data[index].sdf_id == 0xFFFFFFFFu || mip_count <= 1u) {
		sdf_instances.data[index].required_mip = 0u;
		return;
	}
	vec3 delta = sdf_instances.data[index].world_position.xyz - params.camera.xyz;
	float distance = length(delta);
	// One mip per doubling beyond eight scale bounds; the instance's own
	// extent keeps small and large objects on the same relative curve.
	float band = max(sdf_instances.data[index].world_position.w * 8.0, 0.001);
	float mip = clamp(floor(log2(max(distance / band, 1.0))), 0.0, float(mip_count - 1u));
	sdf_instances.data[index].required_mip = uint(mip);
}
