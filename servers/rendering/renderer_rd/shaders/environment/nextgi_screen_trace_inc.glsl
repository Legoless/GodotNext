// GodotNext: nextgi_view_math. Not a standalone shader.
//
// The Hi-Z screen trace both tiers run before their world trace (#25, #186).
// The includer includes nextgi_view_inc.glsl first, declares the Hi-Z depth
// pyramid and the last frame's color, and defines NEXTGI_SCREEN_HIZ_TEX and
// NEXTGI_SCREEN_LAST_FRAME_TEX as their sampler2D names.

#ifndef NEXTGI_SCREEN_TRACE_INC_GLSL
#define NEXTGI_SCREEN_TRACE_INC_GLSL

// Hi-Z march, at most 24 steps. A hit samples the last frame. A miss leaves
// march_end where the ray left the screen, or on the origin.
bool nextgi_screen_trace_hiz(mat4 view_proj, mat4 inv_view_proj, vec3 camera, vec2 screen_size, float max_mip, vec3 origin, vec3 direction, float tmax, out vec3 color, out vec3 march_end) {
	color = vec3(0.0);
	march_end = origin;
	vec3 far_pos = origin + direction * tmax;
	// A segment that passes behind the camera is clipped where it crosses the
	// camera plane; marching it on the start pixel finds false hits.
	vec4 clip0 = view_proj * vec4(origin, 1.0);
	vec4 clip1 = view_proj * vec4(far_pos, 1.0);
	const float min_w = 1e-3;
	if (!(clip0.w > min_w)) {
		return false;
	}
	if (!(clip1.w > min_w)) {
		far_pos = mix(origin, far_pos, (clip0.w - min_w) / (clip0.w - clip1.w));
	}
	vec3 screen0 = nextgi_view_project(view_proj, origin);
	vec3 screen1 = nextgi_view_project(view_proj, far_pos);
	vec2 uv0 = screen0.xy;
	vec2 uv1 = screen1.xy;
	vec2 screen = max(screen_size, vec2(1.0));
	float mip = clamp(log2(max(length((uv1 - uv0) * screen) / 24.0, 1.0)), 0.0, max_mip);
	bool left_screen = false;
	for (int step = 0; step < 24; step++) {
		float t = (float(step) + 0.5) / 24.0;
		vec2 uv = mix(uv0, uv1, t);
		march_end = mix(origin, far_pos, t);
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
			left_screen = true;
			break;
		}
		float scene_z = textureLod(NEXTGI_SCREEN_HIZ_TEX, uv, mip).r;
		ivec2 pixel = ivec2(clamp(uv * screen, vec2(0.0), screen - 1.0));
		vec3 scene_pos = nextgi_view_reconstruct(inv_view_proj, (vec2(pixel) + 0.5) / screen, scene_z);
		float scene_dist = max(length(scene_pos - camera), 0.001);
		float ray_dist = length(march_end - camera);
		if (ray_dist > scene_dist && ray_dist < scene_dist + 0.1 * scene_dist) {
			color = textureLod(NEXTGI_SCREEN_LAST_FRAME_TEX, uv, 0.0).rgb;
			return true;
		}
	}
	if (!left_screen) {
		march_end = origin;
	}
	return false;
}

#endif // NEXTGI_SCREEN_TRACE_INC_GLSL
