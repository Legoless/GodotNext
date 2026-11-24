#[compute]

// GodotNext: path_tracer_lights_camera (#169). Builds a 2D marginal/conditional CDF and UV-space PDF table
// over the sky radiance octmap (128x128) for unbiased sky importance sampling and MIS in the path tracer.
// Plain compute shader (no ray query). Two modes:
//   - default (variant 0): per-row weights (luminance * octahedral Jacobian) and row conditional CDFs
//   - MODE_MARGINAL (variant 1): marginal CDF over rows and normalized UV-space cell PDFs

#version 460

#VERSION_DEFINES

#include "../oct_inc.glsl"

#define SKY_CDF_DIM 128u
#define SKY_CDF_COND_OFFSET 128u
#define SKY_CDF_PDF_OFFSET (128u + 128u * 128u)
#define SKY_CDF_TOTAL_OFFSET (128u + 128u * 128u * 2u)

layout(set = 0, binding = 0) uniform texture2D sky_octmap;
layout(set = 0, binding = 1) uniform sampler linear_sampler;

layout(set = 0, binding = 2, std430) restrict buffer SkyCdfData {
	float data[];
}
sky_cdf;

layout(push_constant, std430) uniform SkyCdfPush {
	float border_pad;
	float border_scale;
	float radiance_scale;
	float pad;
}
push_params;

float sky_oct_jacobian(vec3 p_dir) {
	float l1 = abs(p_dir.x) + abs(p_dir.y) + abs(p_dir.z);
	return 4.0 * l1 * l1 * l1;
}

#ifdef MODE_MARGINAL

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

shared float s_row_sums[SKY_CDF_DIM];

void main() {
	uint y = gl_LocalInvocationID.x;
	float row_sum = max(sky_cdf.data[y], 0.0);
	s_row_sums[y] = row_sum;
	barrier();

	// Exclusive/inclusive prefix sum over the 128 row weights on thread 0 (128 iterations, negligible cost).
	if (y == 0u) {
		float total = 0.0;
		for (uint i = 0u; i < SKY_CDF_DIM; i++) {
			total += s_row_sums[i];
			s_row_sums[i] = total;
		}
		float safe_total = total > 1e-12 ? total : 1.0;
		sky_cdf.data[SKY_CDF_TOTAL_OFFSET] = total;
		for (uint i = 0u; i < SKY_CDF_DIM; i++) {
			sky_cdf.data[i] = (i + 1u == SKY_CDF_DIM) ? 1.0 : (s_row_sums[i] / safe_total);
		}
	}
	barrier();

	float total = sky_cdf.data[SKY_CDF_TOTAL_OFFSET];
	float inv_total_uv = total > 1e-12 ? (float(SKY_CDF_DIM * SKY_CDF_DIM) / total) : 1.0;
	uint row_base = SKY_CDF_PDF_OFFSET + y * SKY_CDF_DIM;
	for (uint x = 0u; x < SKY_CDF_DIM; x++) {
		float w = sky_cdf.data[row_base + x];
		sky_cdf.data[row_base + x] = total > 1e-12 ? (w * inv_total_uv) : 1.0;
	}
}

#else

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

shared float s_weights[SKY_CDF_DIM];

void main() {
	uint x = gl_LocalInvocationID.x;
	uint y = gl_WorkGroupID.x;
	if (y >= SKY_CDF_DIM) {
		return;
	}

	vec2 uv = (vec2(float(x), float(y)) + 0.5) / float(SKY_CDF_DIM);
	vec2 oct_uv = uv * push_params.border_scale + vec2(push_params.border_pad);
	vec3 dir = oct_to_vec3(uv * 2.0 - 1.0);
	vec3 rgb = max(textureLod(sampler2D(sky_octmap, linear_sampler), oct_uv, 0.0).rgb * push_params.radiance_scale, vec3(0.0));
	// Sanitize non-finite texels and keep a tiny positive floor so every cell has nonzero sampling probability.
	if (any(isnan(rgb)) || any(isinf(rgb))) {
		rgb = vec3(0.0);
	}
	float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	float weight = max(lum, 1e-4) * sky_oct_jacobian(dir);
	s_weights[x] = weight;
	sky_cdf.data[SKY_CDF_PDF_OFFSET + y * SKY_CDF_DIM + x] = weight;
	barrier();

	if (x == 0u) {
		float row_sum = 0.0;
		uint cond_base = SKY_CDF_COND_OFFSET + y * SKY_CDF_DIM;
		for (uint i = 0u; i < SKY_CDF_DIM; i++) {
			row_sum += s_weights[i];
			s_weights[i] = row_sum;
		}
		sky_cdf.data[y] = row_sum;
		float safe_sum = row_sum > 1e-12 ? row_sum : 1.0;
		for (uint i = 0u; i < SKY_CDF_DIM; i++) {
			sky_cdf.data[cond_base + i] = (i + 1u == SKY_CDF_DIM) ? 1.0 : (s_weights[i] / safe_sum);
		}
	}
}

#endif
