#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0;
}

#[fragment]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_exclusive_dispatch (#79).
// NEXTGEO_RT_PROXY: linear depth difference and silhouette mismatch overlay
// between the rasterized scene depth and the NextGeo ray-tracing proxy depth.

layout(location = 0) in vec2 uv_interp;

layout(set = 0, binding = 0) uniform sampler2D raster_depth_tex;
layout(set = 0, binding = 1) uniform sampler2D proxy_depth_tex;

layout(location = 0) out vec4 frag_color;

layout(push_constant, std430) uniform Params {
	vec2 internal_size;
	vec2 output_size;
	float z_near;
	float z_far;
	uint is_orthogonal;
	uint pad0;
}
params;

float linearize_depth(float raw_d) {
	if (params.is_orthogonal != 0u) {
		return params.z_near + (1.0 - raw_d) * (params.z_far - params.z_near);
	}
	float denom = max(params.z_near + raw_d * (params.z_far - params.z_near), 1e-6);
	return (params.z_near * params.z_far) / denom;
}

vec3 heat_ramp(float t) {
	t = clamp(t, 0.0, 1.0);
	if (t < 0.5) {
		return mix(vec3(0.08, 0.55, 0.22), vec3(0.95, 0.85, 0.12), t * 2.0);
	}
	return mix(vec3(0.95, 0.85, 0.12), vec3(0.95, 0.12, 0.10), (t - 0.5) * 2.0);
}

void main() {
	vec2 uv = clamp(gl_FragCoord.xy / max(params.output_size, vec2(1.0)), vec2(0.0), vec2(1.0));
	ivec2 ipx = clamp(ivec2(uv * params.internal_size), ivec2(0), ivec2(params.internal_size) - ivec2(1));

	float d_raster = texelFetch(raster_depth_tex, ipx, 0).r;
	float d_proxy = texelFetch(proxy_depth_tex, ipx, 0).r;

	bool hit_raster = d_raster > 0.0;
	bool hit_proxy = d_proxy > 0.0;

	if (!hit_raster && !hit_proxy) {
		frag_color = vec4(0.02, 0.02, 0.03, 0.92);
		return;
	}
	if (hit_raster && !hit_proxy) {
		// Raster hit with no RT proxy hit (excluded or silhouette inward step).
		frag_color = vec4(0.10, 0.55, 0.95, 0.88);
		return;
	}
	if (!hit_raster && hit_proxy) {
		// RT proxy silhouette overhang beyond raster silhouette.
		frag_color = vec4(0.95, 0.22, 0.80, 0.88);
		return;
	}

	float z_r = linearize_depth(d_raster);
	float z_p = linearize_depth(d_proxy);
	float diff_m = abs(z_r - z_p);
	float ref_scale = max(0.02, 0.01 * z_r);
	vec3 col = heat_ramp(diff_m / ref_scale);
	frag_color = vec4(col, 0.88);
}
