// GodotNext: nextgi_view_math. Not a standalone shader.
//
// The Hi-Z screen trace both tiers run before their world trace (#25, #186).
// The includer includes nextgi_view_inc.glsl first, declares the Hi-Z depth
// pyramid and the last frame's color, and defines NEXTGI_SCREEN_HIZ_TEX and
// NEXTGI_SCREEN_LAST_FRAME_TEX as their sampler2D names.

#ifndef NEXTGI_SCREEN_TRACE_INC_GLSL
#define NEXTGI_SCREEN_TRACE_INC_GLSL

// Hi-Z march, at most 24 steps. A hit samples the last frame and leaves
// march_end on the hit sample. A miss that left the screen or passed behind a
// surface (beyond the hit thickness) stops there, as UE's screen traces stop
// and hand over (#186 step 4), and leaves march_end on the last sample before
// it, which the ray reached in front of the depth buffer (the origin when the
// first sample stopped). A march that ran all its samples in front of the
// depth buffer leaves it on the origin.
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
	float far_w = clip1.w;
	if (!(clip1.w > min_w)) {
		far_pos = mix(origin, far_pos, (clip0.w - min_w) / (clip0.w - clip1.w));
		far_w = min_w;
	}
	vec3 screen0 = nextgi_view_project(view_proj, origin);
	vec3 screen1 = nextgi_view_project(view_proj, far_pos);
	vec2 uv0 = screen0.xy;
	vec2 uv1 = screen1.xy;
	vec2 screen = max(screen_size, vec2(1.0));
	float mip = clamp(log2(max(length((uv1 - uv0) * screen) / 24.0, 1.0)), 0.0, max_mip);
	bool stopped = false;
	vec3 last_clear = origin;
	for (int step = 0; step < 24; step++) {
		float t = (float(step) + 0.5) / 24.0;
		vec2 uv = mix(uv0, uv1, t);
		// GodotNext: ps_swrt_trace (#186 step 4). Even steps on screen are uneven along the ray
		// under perspective: this is the point of the segment that projects to uv.
		vec3 sample_pos = mix(origin, far_pos, t * clip0.w / mix(far_w, clip0.w, t));
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
			stopped = true;
			break;
		}
		float scene_z = textureLod(NEXTGI_SCREEN_HIZ_TEX, uv, mip).r;
		ivec2 pixel = ivec2(clamp(uv * screen, vec2(0.0), screen - 1.0));
		vec3 scene_pos = nextgi_view_reconstruct(inv_view_proj, (vec2(pixel) + 0.5) / screen, scene_z);
		float scene_dist = max(length(scene_pos - camera), 0.001);
		float ray_dist = length(sample_pos - camera);
		if (ray_dist > scene_dist && ray_dist < scene_dist + 0.1 * scene_dist) {
			color = textureLod(NEXTGI_SCREEN_LAST_FRAME_TEX, uv, 0.0).rgb;
			march_end = sample_pos;
			return true;
		}
		// GodotNext: ps_swrt_trace (#186 step 4). Behind a surface the screen cannot tell what the
		// ray meets: stop, and the world trace resumes before this sample, pulled back.
		if (ray_dist >= scene_dist + 0.1 * scene_dist) {
			stopped = true;
			break;
		}
		last_clear = sample_pos;
	}
	if (stopped) {
		march_end = last_clear;
	}
	return false;
}

// GodotNext: ps_swrt_trace (#186 step 4).
// r.Lumen.HardwareRayTracing.PullbackBias resumes a screen-traced ray pulled back
// from where the march stopped. Its published default is 8, and UE world units are
// centimeters, so the bias here is 8 cm, taken back from march_end: the last sample
// the march saw in front of the depth buffer, so the surface the ray passed behind
// lies after it. On the software tier the caller passes one voxel of the SDF level
// at march_end as the clearance, and the bias grows to it.
const float NEXTGI_SCREEN_PULLBACK_MIN_M = 0.08;

float nextgi_screen_pullback(float clearance) {
	return max(NEXTGI_SCREEN_PULLBACK_MIN_M, clearance);
}

// A screen miss resumes the world trace at marched (the distance from the origin
// to march_end), pulled back toward the origin. The distance from the origin to
// the resumed origin is returned. It is 0, and the trace starts at the origin,
// when the march did not stop (marched is 0) and when the pulled-back point lies
// within the clearance of the origin: the software tier cannot tell the two apart
// there, and a trace from the origin keeps its escape rule.
float nextgi_screen_resume(vec3 origin, vec3 direction, float marched, float world_tmax, float clearance, out vec3 trace_origin, out float trace_tmax) {
	trace_origin = origin;
	trace_tmax = world_tmax;
	float resume = marched - nextgi_screen_pullback(clearance);
	if (!(resume > max(clearance, 0.001))) {
		return 0.0;
	}
	trace_origin = origin + direction * resume;
	trace_tmax = max(world_tmax - resume, 0.0);
	return resume;
}

#endif // NEXTGI_SCREEN_TRACE_INC_GLSL
