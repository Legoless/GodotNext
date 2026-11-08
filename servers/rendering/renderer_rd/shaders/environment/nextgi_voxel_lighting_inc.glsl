// GodotNext: ps_voxel_lighting (#188). Included by the voxel-lighting compute
// shader and by the global-SDF trace (#186). Not a standalone shader.
//
// The includer declares, before this file:
//
// layout(set = S, binding = R) buffer NextGIVoxelRadiance { uint data[]; } voxel_radiance;
// layout(set = S, binding = W) buffer NextGIVoxelWeights { uint data[]; } voxel_weights;
//
// Clipmap origins are NextGIVoxelClipmap: each ivec4 is the global-SDF origin
// in xyz and the lighting voxel size as floatBits in w. Direction d stores the
// surface found by tracing along +d (that surface faces -d). The lookup reads
// the slot whose stored surface faces the gradient.

#ifndef NEXTGI_VOXEL_LIGHTING_INC_GLSL
#define NEXTGI_VOXEL_LIGHTING_INC_GLSL

const uint NEXTGI_VOXEL_LEVELS = 4u;
const uint NEXTGI_VOXEL_SIZE = 64u;
const uint NEXTGI_VOXEL_DIRECTIONS = 6u;
const float NEXTGI_VOXEL_WEIGHT_FALLBACK = 0.5;

struct NextGIVoxelClipmap {
	ivec4 origin_size[4];
};

uint nextgi_voxel_pack_positive(float value, uint mantissa_bits) {
	if (!(value > 0.0) || mantissa_bits == 0u || mantissa_bits > 6u) {
		return 0u;
	}
	uint bits = floatBitsToUint(value);
	int unbiased = int((bits >> 23u) & 255u) - 127;
	uint mantissa = bits & 0x7fffffu;
	int stored = unbiased + 15;
	uint max_mantissa = (1u << mantissa_bits) - 1u;
	if (stored <= 0) {
		float scale = exp2(float(1 - 15 - int(mantissa_bits)));
		float rounded = floor(value / scale + 0.5);
		if (!(rounded > 0.0)) {
			return 0u;
		}
		return min(uint(rounded), max_mantissa);
	}
	if (stored > 30) {
		return (30u << mantissa_bits) | max_mantissa;
	}
	uint shift = 23u - mantissa_bits;
	uint packed_mantissa = mantissa >> shift;
	if (shift > 0u && ((mantissa >> (shift - 1u)) & 1u) != 0u) {
		packed_mantissa += 1u;
		if (packed_mantissa > max_mantissa) {
			packed_mantissa = 0u;
			stored += 1;
			if (stored > 30) {
				return (30u << mantissa_bits) | max_mantissa;
			}
		}
	}
	return (uint(stored) << mantissa_bits) | packed_mantissa;
}

float nextgi_voxel_unpack_positive(uint packed, uint mantissa_bits) {
	uint mantissa_mask = (1u << mantissa_bits) - 1u;
	uint mantissa = packed & mantissa_mask;
	uint exponent = (packed >> mantissa_bits) & 31u;
	if (exponent == 0u) {
		if (mantissa == 0u) {
			return 0.0;
		}
		return exp2(float(1 - 15)) * (float(mantissa) / float(1u << mantissa_bits));
	}
	float significand = 1.0 + float(mantissa) / float(1u << mantissa_bits);
	return exp2(float(int(exponent) - 15)) * significand;
}

uint nextgi_voxel_pack_r11g11b10(vec3 color) {
	uint red = nextgi_voxel_pack_positive(color.r, 6u);
	uint green = nextgi_voxel_pack_positive(color.g, 6u);
	uint blue = nextgi_voxel_pack_positive(color.b, 5u);
	return red | (green << 11u) | (blue << 22u);
}

vec3 nextgi_voxel_unpack_r11g11b10(uint packed) {
	return vec3(
			nextgi_voxel_unpack_positive(packed & 0x7ffu, 6u),
			nextgi_voxel_unpack_positive((packed >> 11u) & 0x7ffu, 6u),
			nextgi_voxel_unpack_positive((packed >> 22u) & 0x3ffu, 5u));
}

int nextgi_voxel_wrap(int value) {
	int size = int(NEXTGI_VOXEL_SIZE);
	int wrapped = value % size;
	if (wrapped < 0) {
		wrapped += size;
	}
	return wrapped;
}

uint nextgi_voxel_weight_byte(uint voxel, uint direction) {
	uint byte_index = voxel * NEXTGI_VOXEL_DIRECTIONS + direction;
	uint word = voxel_weights.data[byte_index >> 2u];
	return (word >> ((byte_index & 3u) * 8u)) & 0xffu;
}

vec4 nextgi_voxel_sample(uint level, ivec3 physical, uint direction) {
	uint voxel = uint(physical.x) + uint(physical.y) * NEXTGI_VOXEL_SIZE + uint(physical.z) * NEXTGI_VOXEL_SIZE * NEXTGI_VOXEL_SIZE;
	uint voxel_key = level * NEXTGI_VOXEL_SIZE * NEXTGI_VOXEL_SIZE * NEXTGI_VOXEL_SIZE + voxel;
	uint sample_index = voxel_key * NEXTGI_VOXEL_DIRECTIONS + direction;
	vec3 radiance = nextgi_voxel_unpack_r11g11b10(voxel_radiance.data[sample_index]);
	float weight = float(nextgi_voxel_weight_byte(voxel_key, direction)) / 255.0;
	return vec4(radiance, weight);
}

// p_positive: the stored surface that faces the +axis (traced along -axis).
uint nextgi_voxel_facing_direction(uint axis, bool positive) {
	return positive ? axis + 3u : axis;
}

vec4 nextgi_voxel_resolve(vec3 gradient, vec4 samples_px, vec4 samples_nx, vec4 samples_py, vec4 samples_ny, vec4 samples_pz, vec4 samples_nz) {
	vec4 resolved = vec4(0.0);
	if (dot(gradient, gradient) <= 1e-12) {
		return resolved;
	}
	vec3 normal = normalize(gradient);
	float axis_weights[3] = float[3](abs(normal.x), abs(normal.y), abs(normal.z));
	float axis_sum = axis_weights[0] + axis_weights[1] + axis_weights[2];
	if (!(axis_sum > 1e-6)) {
		return resolved;
	}
	vec4 chosen[3] = vec4[3](
			normal.x >= 0.0 ? samples_nx : samples_px,
			normal.y >= 0.0 ? samples_ny : samples_py,
			normal.z >= 0.0 ? samples_nz : samples_pz);
	float blend = 0.0;
	vec3 accum = vec3(0.0);
	for (uint axis = 0u; axis < 3u; axis++) {
		float axis_weight = axis_weights[axis] / axis_sum;
		blend += axis_weight * chosen[axis].w;
		accum += axis_weight * chosen[axis].w * chosen[axis].xyz;
	}
	resolved = vec4(accum, blend);
	return resolved;
}

bool nextgi_voxel_local(vec3 world, NextGIVoxelClipmap clipmap, uint level, out vec3 coord) {
	float lighting = uintBitsToFloat(uint(clipmap.origin_size[level].w));
	if (!(lighting > 0.0)) {
		coord = vec3(0.0);
		return false;
	}
	vec3 origin_world = vec3(clipmap.origin_size[level].xyz) * (lighting * 0.5);
	coord = (world - origin_world) / lighting - 0.5;
	return coord.x >= 0.0 && coord.y >= 0.0 && coord.z >= 0.0 && coord.x < float(NEXTGI_VOXEL_SIZE - 1u) && coord.y < float(NEXTGI_VOXEL_SIZE - 1u) && coord.z < float(NEXTGI_VOXEL_SIZE - 1u);
}

ivec3 nextgi_voxel_physical(ivec3 origin, ivec3 local) {
	ivec3 world = origin / 2 + local;
	return ivec3(nextgi_voxel_wrap(world.x), nextgi_voxel_wrap(world.y), nextgi_voxel_wrap(world.z));
}

vec4 nextgi_voxel_resolve_at(uint level, ivec3 physical, vec3 gradient) {
	vec4 samples_px = nextgi_voxel_sample(level, physical, 0u);
	vec4 samples_nx = nextgi_voxel_sample(level, physical, 3u);
	vec4 samples_py = nextgi_voxel_sample(level, physical, 1u);
	vec4 samples_ny = nextgi_voxel_sample(level, physical, 4u);
	vec4 samples_pz = nextgi_voxel_sample(level, physical, 2u);
	vec4 samples_nz = nextgi_voxel_sample(level, physical, 5u);
	return nextgi_voxel_resolve(gradient, samples_px, samples_nx, samples_py, samples_ny, samples_pz, samples_nz);
}

// World-space lookup. Selects the finest clipmap that contains the position,
// trilinearly interpolates, and normalizes by weight. Weight below 0.5 returns
// wrc. used_wrc is written through weight: the caller compares it.
vec3 nextgi_voxel_lighting_lookup(vec3 world, vec3 gradient, vec3 wrc, NextGIVoxelClipmap clipmap, out float weight) {
	weight = 0.0;
	int selected = -1;
	vec3 coord = vec3(0.0);
	for (uint level = 0u; level < NEXTGI_VOXEL_LEVELS; level++) {
		vec3 level_coord;
		if (nextgi_voxel_local(world, clipmap, level, level_coord)) {
			selected = int(level);
			coord = level_coord;
			break;
		}
	}
	if (selected < 0) {
		return wrc;
	}
	ivec3 base = ivec3(floor(coord));
	vec3 fract_coord = coord - vec3(base);
	vec3 accum = vec3(0.0);
	float blend = 0.0;
	for (uint corner = 0u; corner < 8u; corner++) {
		ivec3 delta = ivec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
		ivec3 local = base + delta;
		ivec3 physical = nextgi_voxel_physical(clipmap.origin_size[selected].xyz, local);
		vec4 resolved = nextgi_voxel_resolve_at(uint(selected), physical, gradient);
		float corner_weight = (delta.x == 1 ? fract_coord.x : 1.0 - fract_coord.x);
		corner_weight *= (delta.y == 1 ? fract_coord.y : 1.0 - fract_coord.y);
		corner_weight *= (delta.z == 1 ? fract_coord.z : 1.0 - fract_coord.z);
		blend += corner_weight * resolved.w;
		accum += corner_weight * resolved.xyz;
	}
	weight = blend;
	if (!(blend >= NEXTGI_VOXEL_WEIGHT_FALLBACK)) {
		return wrc;
	}
	return accum / blend;
}

#endif
