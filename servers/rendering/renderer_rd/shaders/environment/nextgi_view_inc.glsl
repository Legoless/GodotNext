// GodotNext: nextgi_view_math. Not a standalone shader.
//
// The GLSL twin of NextGIViewMath (next_gi_view.h). The matrices come from
// NextGIView::view_projection() and friends: Godot's corrected projection
// (flip_y, reverse Z, [0, 1] depth), the one the depth prepass rasterizes
// with. Its clip space is the depth texture's sampling space, so a texel's
// uv * 2 - 1 and its stored depth are the clip coordinates as they are.

#ifndef NEXTGI_VIEW_INC_GLSL
#define NEXTGI_VIEW_INC_GLSL

// Depth-texture uv (texel centers at (pixel + 0.5) / size) and stored depth
// to a world position (or a view position with an inverse projection).
vec3 nextgi_view_reconstruct(mat4 inv_view_proj, vec2 uv, float depth) {
	vec4 world = inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return world.xyz / world.w;
}

// World position to (uv.x, uv.y, depth) in the same space. A point behind
// the camera returns a negative depth.
vec3 nextgi_view_project(mat4 view_proj, vec3 world) {
	vec4 clip = view_proj * vec4(world, 1.0);
	if (!(clip.w > 0.0)) {
		return vec3(0.0, 0.0, -1.0);
	}
	return vec3(clip.xy / clip.w * 0.5 + 0.5, clip.z / clip.w);
}

// Depth-texture value of a point p_linear meters in front of the camera.
// coefficients = NextGIViewMath::depth_coefficients() of the depth projection:
// the z and w rows' view-z and constant terms.
float nextgi_view_depth_from_linear(vec4 coefficients, float p_linear) {
	float z = -p_linear;
	return (coefficients.x * z + coefficients.y) / (coefficients.z * z + coefficients.w);
}

// The inverse: meters in front of the camera (view-space -z) of a stored depth.
float nextgi_view_linear_depth(vec4 coefficients, float depth) {
	return -(coefficients.y - depth * coefficients.w) / (depth * coefficients.z - coefficients.x);
}

// The prepass normal-roughness buffer stores view-space normals (encode24 * 0.5
// + 0.5). view_to_world is the camera basis (NextGIViewMath::store_basis()).
vec3 nextgi_view_world_normal(mat3 view_to_world, vec3 encoded) {
	vec3 normal = view_to_world * (encoded * 2.0 - 1.0);
	return dot(normal, normal) > 1e-12 ? normalize(normal) : vec3(0.0, 1.0, 0.0);
}

// Reverse Z: the cleared depth (sky, far plane) is 0.
bool nextgi_view_is_background(float depth) {
	return !(depth > 0.0);
}

#endif // NEXTGI_VIEW_INC_GLSL
