#[compute]

// Reference path tracer v0 (#19, D16). Analytic only: one sample per pixel or patch,
// Russian roulette from bounce 4, hard stop at 64. D5 rules, because this file cannot
// include nextgi_rt_inc.glsl: opaque traversal, one rayQueryProceedEXT per trace, no
// shared memory, no triangle position fetch, no shader-binding-table offset.
#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) buffer Radiance {
	vec4 data[];
}
radiance;

layout(set = 0, binding = 2, std430) readonly buffer Triangles {
	vec4 data[];
}
triangles;

layout(set = 0, binding = 3, std430) readonly buffer Materials {
	vec4 data[];
}
materials;

layout(set = 0, binding = 4, std430) readonly buffer Lights {
	vec4 data[];
}
lights;

layout(set = 0, binding = 5, std430) readonly buffer Patches {
	vec4 data[];
}
patches;

layout(set = 0, binding = 6, std430) readonly buffer Instances {
	uint triangle_offset[];
}
instances;

// dispatch: width, height, sample_offset, sample_count
// control: seed, flags, light_count, triangle_count
layout(push_constant, std430) uniform Params {
	uvec4 dispatch;
	uvec4 control;
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
}
params;

const uint FLAG_PATCH = 1u;
const uint FLAG_DIRECT = 2u;
const uint BOUNCE_CAP = 64u;
const uint ROULETTE_FROM = 4u;
const float ORIGIN_OFFSET = 1e-4;
const float REF_PI = 3.14159265358979323846;
const uint DIFFUSE_BURLEY = 1u;

uint pcg_hash(uint value) {
	uint state = value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float rnd(uint index, uint sample_index, uint bounce, uint dimension) {
	uint hashed = pcg_hash(index);
	hashed = pcg_hash(hashed ^ sample_index);
	hashed = pcg_hash(hashed ^ bounce);
	hashed = pcg_hash(hashed ^ dimension);
	hashed = pcg_hash(hashed ^ params.control.x);
	return float(hashed & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

float schlick(float cos_theta) {
	float complement = 1.0 - cos_theta;
	float squared = complement * complement;
	return squared * squared * complement;
}

float d_ggx(float n_dot_h, float alpha) {
	float a = n_dot_h * alpha;
	float denom = 1.0 - n_dot_h * n_dot_h + a * a;
	float k = alpha / denom;
	return k * k * (1.0 / REF_PI);
}

float v_ggx(float n_dot_l, float n_dot_v, float alpha) {
	float denom = mix(2.0 * n_dot_l * n_dot_v, n_dot_l + n_dot_v, alpha);
	return denom > 0.0 ? 0.5 / denom : 0.0;
}

float lambda_ggx(float n_dot_x, float alpha) {
	float alpha2 = alpha * alpha;
	float cosine2 = max(n_dot_x * n_dot_x, 1e-8);
	return 0.5 * (-1.0 + sqrt(1.0 + alpha2 * (1.0 - cosine2) / cosine2));
}

void basis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
	vec3 helper = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(helper, normal));
	bitangent = cross(normal, tangent);
}

vec3 material_f0(uint material) {
	vec4 albedo_metal = materials.data[material * 3u];
	float specular = materials.data[material * 3u + 2u].x;
	float dielectric = 0.16 * specular * specular;
	return mix(vec3(dielectric), albedo_metal.rgb, albedo_metal.w);
}

vec3 diffuse_albedo(uint material) {
	vec4 albedo_metal = materials.data[material * 3u];
	return albedo_metal.rgb * (1.0 - albedo_metal.w);
}

float diffuse_brdf_nl(uint material, vec3 normal, vec3 view, vec3 light_dir, float n_dot_l) {
	if (materials.data[material * 3u + 2u].y != float(DIFFUSE_BURLEY)) {
		return n_dot_l * (1.0 / REF_PI);
	}
	float roughness = materials.data[material * 3u + 1u].w;
	float n_dot_v = max(dot(normal, view), 1e-4);
	vec3 half_vec = normalize(view + light_dir);
	float l_dot_h = clamp(dot(light_dir, half_vec), 0.0, 1.0);
	float fd90_minus_1 = 2.0 * l_dot_h * l_dot_h * roughness - 0.5;
	float fdv = 1.0 + fd90_minus_1 * schlick(n_dot_v);
	float fdl = 1.0 + fd90_minus_1 * schlick(n_dot_l);
	return (1.0 / REF_PI) * fdv * fdl * n_dot_l;
}

vec3 specular_brdf_nl(uint material, vec3 normal, vec3 view, vec3 light_dir, float n_dot_l) {
	float roughness = materials.data[material * 3u + 1u].w;
	if (roughness <= 0.0 || n_dot_l <= 0.0) {
		return vec3(0.0);
	}
	vec3 f0 = material_f0(material);
	float n_dot_v = max(dot(normal, view), 1e-4);
	vec3 half_vec = normalize(view + light_dir);
	float n_dot_h = clamp(dot(normal, half_vec), 0.0, 1.0);
	float l_dot_h = clamp(dot(light_dir, half_vec), 0.0, 1.0);
	float alpha = roughness * roughness;
	float distribution = d_ggx(n_dot_h, alpha);
	float visibility = v_ggx(n_dot_l, n_dot_v, alpha);
	float fresnel_pow = schlick(l_dot_h);
	float metallic = materials.data[material * 3u].w;
	float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), metallic, 1.0);
	vec3 fresnel = f0 + (f90 - f0) * fresnel_pow;
	return n_dot_l * distribution * fresnel * visibility;
}

float omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

void trace_opaque(vec3 origin, vec3 direction, float tmax, out uint hit, out vec3 position, out vec3 geometric_normal, out uint material) {
	hit = 0u;
	position = origin;
	geometric_normal = vec3(0.0, 1.0, 0.0);
	material = 0u;
	if (tmax <= 0.0) {
		return;
	}
	rayQueryEXT query;
	rayQueryInitializeEXT(query, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.0, direction, tmax);
	rayQueryProceedEXT(query);
	if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
		return;
	}
	uint instance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
	uint tri = instances.triangle_offset[instance] + primitive;
	if (tri >= params.control.w) {
		return;
	}
	vec4 a = triangles.data[tri * 3u];
	vec4 b = triangles.data[tri * 3u + 1u];
	vec4 c = triangles.data[tri * 3u + 2u];
	vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
	float weight0 = 1.0 - bary.x - bary.y;
	position = a.xyz * weight0 + b.xyz * bary.x + c.xyz * bary.y;
	// Face the side the ray arrived from. The acceleration structure's front face follows
	// Godot's counter-clockwise option, while these soups store an authored winding, so the
	// incident direction is the side the 1e-4 m offset has to stay on.
	vec3 geometric = cross(b.xyz - a.xyz, c.xyz - a.xyz);
	if (dot(geometric, direction) > 0.0) {
		geometric = -geometric;
	}
	geometric_normal = normalize(geometric);
	material = uint(a.w);
	hit = 1u;
}

vec3 next_event(vec3 position, vec3 normal, vec3 view, uint material) {
	vec3 lit = vec3(0.0);
	uint light_count = min(params.control.z, 8u);
	for (uint i = 0u; i < light_count; i++) {
		vec4 position_range = lights.data[i * 3u];
		vec4 color_energy = lights.data[i * 3u + 1u];
		vec4 extra = lights.data[i * 3u + 2u];
		vec3 shadow_origin = position + normal * ORIGIN_OFFSET;
		vec3 to_light = position_range.xyz - shadow_origin;
		float dist = length(to_light);
		if (dist <= 1e-6) {
			continue;
		}
		vec3 light_dir = to_light / dist;
		float n_dot_l = dot(normal, light_dir);
		if (n_dot_l <= 0.0) {
			continue;
		}
		uint blocked = 0u;
		vec3 unused_position;
		vec3 unused_normal;
		uint unused_material;
		trace_opaque(shadow_origin, light_dir, dist - ORIGIN_OFFSET, blocked, unused_position, unused_normal, unused_material);
		if (blocked != 0u) {
			continue;
		}
		float attenuation = omni_attenuation(dist, 1.0 / position_range.w, extra.x);
		// Non-physical Godot units: authored energy is multiplied by pi (light_storage).
		vec3 light_color = color_energy.rgb * color_energy.w * REF_PI;
		float specular_amount = extra.y * 2.0;
		vec3 diffuse = light_color * attenuation * diffuse_brdf_nl(material, normal, view, light_dir, n_dot_l) * diffuse_albedo(material);
		vec3 specular = light_color * attenuation * specular_brdf_nl(material, normal, view, light_dir, n_dot_l) * specular_amount;
		lit += diffuse + specular;
	}
	return lit;
}

vec3 sample_cosine(vec3 normal, vec2 random_uv) {
	float phi = 2.0 * REF_PI * random_uv.x;
	float sine = sqrt(random_uv.y);
	float cosine = sqrt(max(0.0, 1.0 - random_uv.y));
	vec3 tangent;
	vec3 bitangent;
	basis(normal, tangent, bitangent);
	return normalize(tangent * (sine * cos(phi)) + bitangent * (sine * sin(phi)) + normal * cosine);
}

vec3 sample_ggx(vec3 normal, vec3 view, float alpha, vec2 random_uv) {
	vec3 tangent;
	vec3 bitangent;
	basis(normal, tangent, bitangent);
	vec3 view_local = vec3(dot(view, tangent), dot(view, bitangent), dot(view, normal));
	if (view_local.z < 0.0) {
		view_local.z = 0.0;
	}
	vec3 stretched = normalize(vec3(alpha * view_local.x, alpha * view_local.y, view_local.z));
	float lensq = stretched.x * stretched.x + stretched.y * stretched.y;
	vec3 t1 = lensq > 0.0 ? vec3(-stretched.y, stretched.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(stretched, t1);
	float radius = sqrt(random_uv.x);
	float phi = 2.0 * REF_PI * random_uv.y;
	float t1s = radius * cos(phi);
	float t2s = radius * sin(phi);
	float cap = 0.5 * (1.0 + stretched.z);
	t2s = (1.0 - cap) * sqrt(max(0.0, 1.0 - t1s * t1s)) + cap * t2s;
	vec3 hemisphere = t1 * t1s + t2 * t2s + stretched * sqrt(max(0.0, 1.0 - t1s * t1s - t2s * t2s));
	vec3 half_local = normalize(vec3(alpha * hemisphere.x, alpha * hemisphere.y, max(0.0, hemisphere.z)));
	vec3 half_world = tangent * half_local.x + bitangent * half_local.y + normal * half_local.z;
	return reflect(-view, half_world);
}

float ggx_pdf(uint material, vec3 normal, vec3 view, vec3 light_dir) {
	float roughness = materials.data[material * 3u + 1u].w;
	float alpha = roughness * roughness;
	vec3 half_vec = normalize(view + light_dir);
	float n_dot_h = clamp(dot(normal, half_vec), 0.0, 1.0);
	float n_dot_v = max(dot(normal, view), 1e-4);
	float v_dot_h = max(dot(view, half_vec), 0.0);
	if (v_dot_h <= 0.0) {
		return 0.0;
	}
	float distribution = d_ggx(n_dot_h, alpha);
	float g1 = 1.0 / (1.0 + lambda_ggx(n_dot_v, alpha));
	return distribution * g1 * v_dot_h / (4.0 * n_dot_v * v_dot_h);
}

vec3 sample_bsdf(uint material, vec3 normal, vec3 view, uint index, uint sample_index, uint bounce, out vec3 direction, out float pdf) {
	direction = normal;
	pdf = 0.0;
	vec3 reflected = diffuse_albedo(material);
	vec3 f0 = material_f0(material);
	float roughness = materials.data[material * 3u + 1u].w;
	float spec_weight = max(f0.r, max(f0.g, f0.b));
	float diff_weight = max(reflected.r, max(reflected.g, reflected.b));
	float p_spec = 0.0;
	if (roughness > 0.0 && spec_weight + diff_weight > 0.0) {
		p_spec = spec_weight / (spec_weight + diff_weight);
	}
	vec2 random_uv = vec2(rnd(index, sample_index, bounce, 1u), rnd(index, sample_index, bounce, 2u));
	float lobe = rnd(index, sample_index, bounce, 3u);
	if (lobe < p_spec) {
		float alpha = roughness * roughness;
		direction = sample_ggx(normal, view, alpha, random_uv);
		float n_dot_l = dot(normal, direction);
		if (n_dot_l <= 0.0) {
			return vec3(0.0);
		}
		pdf = ggx_pdf(material, normal, view, direction) * p_spec;
		if (pdf <= 0.0) {
			return vec3(0.0);
		}
		return specular_brdf_nl(material, normal, view, direction, n_dot_l) / pdf;
	}
	direction = sample_cosine(normal, random_uv);
	float n_dot_l = max(dot(normal, direction), 0.0);
	float cosine_pdf = n_dot_l * (1.0 / REF_PI);
	pdf = cosine_pdf * (1.0 - p_spec);
	if (pdf <= 0.0) {
		return vec3(0.0);
	}
	return reflected * diffuse_brdf_nl(material, normal, view, direction, n_dot_l) / pdf;
}

vec3 camera_direction(uvec2 pixel) {
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.dispatch.xy);
	vec2 ndc = uv * 2.0 - 1.0;
	ndc.y = -ndc.y;
	float tan_half = tan(params.cam_origin_fov.w * 0.5);
	float aspect = params.cam_forward_aspect.w;
	return normalize(params.cam_right.xyz * (ndc.x * tan_half * aspect) + params.cam_up.xyz * (ndc.y * tan_half) + params.cam_forward_aspect.xyz);
}

vec3 integrate(uint index, uint sample_index, uvec2 pixel) {
	vec3 throughput = vec3(1.0);
	vec3 accumulated = vec3(0.0);
	vec3 origin;
	vec3 direction;
	uint bounce_start = 0u;
	bool patch_mode = (params.control.y & FLAG_PATCH) != 0u;
	bool direct_only = (params.control.y & FLAG_DIRECT) != 0u;
	if (patch_mode) {
		vec4 patch_position = patches.data[index * 2u];
		vec3 normal = normalize(patches.data[index * 2u + 1u].xyz);
		uint material = uint(patch_position.w);
		origin = patch_position.xyz;
		vec3 view = normal;
		accumulated += materials.data[material * 3u + 1u].rgb;
		accumulated += next_event(origin, normal, view, material);
		if (direct_only) {
			return accumulated;
		}
		float pdf = 0.0;
		vec3 weight = sample_bsdf(material, normal, view, index, sample_index, 0u, direction, pdf);
		if (pdf <= 0.0) {
			return accumulated;
		}
		throughput = weight;
		origin = patch_position.xyz + normal * ORIGIN_OFFSET;
		bounce_start = 1u;
	} else {
		origin = params.cam_origin_fov.xyz;
		direction = camera_direction(pixel);
	}

	for (uint bounce = bounce_start; bounce < BOUNCE_CAP; bounce++) {
		if (bounce >= ROULETTE_FROM) {
			float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
			if (survival <= 0.0 || rnd(index, sample_index, bounce, 0u) > survival) {
				break;
			}
			throughput /= survival;
		}
		uint hit = 0u;
		vec3 position;
		vec3 normal;
		uint material;
		trace_opaque(origin, direction, 1.0e6, hit, position, normal, material);
		if (hit == 0u) {
			break;
		}
		vec3 view = -direction;
		accumulated += throughput * materials.data[material * 3u + 1u].rgb;
		accumulated += throughput * next_event(position, normal, view, material);
		if (direct_only) {
			break;
		}
		float pdf = 0.0;
		vec3 next_dir;
		vec3 weight = sample_bsdf(material, normal, view, index, sample_index, bounce, next_dir, pdf);
		if (pdf <= 0.0 || weight.r != weight.r) {
			break;
		}
		throughput *= weight;
		if (max(throughput.r, max(throughput.g, throughput.b)) <= 0.0) {
			break;
		}
		origin = position + normal * ORIGIN_OFFSET;
		direction = next_dir;
	}
	return accumulated;
}

void main() {
	uvec2 pixel = gl_GlobalInvocationID.xy;
	if (pixel.x >= params.dispatch.x || pixel.y >= params.dispatch.y) {
		return;
	}
	uint index = pixel.y * params.dispatch.x + pixel.x;
	vec3 sum = vec3(0.0);
	uint sample_count = params.dispatch.w;
	uint sample_offset = params.dispatch.z;
	for (uint s = 0u; s < sample_count; s++) {
		sum += integrate(index, sample_offset + s, pixel);
	}
	vec4 acc = radiance.data[index];
	acc.rgb += sum;
	acc.a += float(sample_count);
	radiance.data[index] = acc;
}
