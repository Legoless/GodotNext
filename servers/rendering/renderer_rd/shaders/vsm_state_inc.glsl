// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). The per-view parameters of the page
// marking pass, shared by vsm_mark_pages.glsl and the C++ copy
// RendererRD::VirtualShadowMapStorage::ViewParams; the VSM_STATE_* and VSM_MAP_STATE_* layouts come
// from RendererVirtualShadowMapStorage::get_glsl_defines(). Not a standalone shader.

struct VSMViewParams {
	vec4 inv_projection[4]; // Rows: depth-texture clip space (uv * 2 - 1, stored depth) to view space.
	vec4 view_to_world[3]; // Rows of the camera transform.
	uvec4 screen; // x, y: the depth buffer's size; z: 1 to capture the depth read (GPU check); w: request list capacity.
	vec4 footprint; // x, y: a pixel's footprint in meters is view depth * x + y; z: the camera's far plane (cluster slices).
	uvec4 cluster; // x: shift, y: width in cells, z: one light type's size in uints, w: max elements / 32.
	uvec4 counts; // x: directional lights, y: omni, z: spot, w: area.
	uvec4 misc; // x: the page table's map count, y: transparent boxes, z: frame, w: deferred static invalidation budget (#212).
	ivec4 bias; // x: directional LOD bias, y: local-light LOD bias (mips), z: pool grid width, w: pool page count (#212).
	uvec4 fog_size; // xyz: the froxel grid's cells, w: unused.
	vec4 fog_a; // x: fog length, y: detail spread, zw: the grid's near half size.
	vec4 fog_b; // xy: the grid's far half size.
	uvec4 directional[VSM_MAX_DIRECTIONAL_LIGHTS]; // Map base, light flags, max distance and fog energy (float bits).
};
