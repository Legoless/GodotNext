// Reference path tracer estimator (#27, D16). The includer declares the TLAS, includes
// nextgi_rt_inc.glsl, then the material, light, record, surface and chunk buffers.
// Bounce rules match the v0 testbed tracer: Russian roulette after 4, hard stop at 64,
// no clamp. Each trace is one nextgi_trace() call.

const uint NEXTGI_PT_BOUNCE_CAP = 64u;
const uint NEXTGI_PT_ROULETTE_FROM = 4u;
const float NEXTGI_PT_PI = 3.14159265358979323846;
const float NEXTGI_PT_ORIGIN_OFFSET = 1e-4;

uint nextgi_pt_hash(uint value) {
	uint state = value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float nextgi_pt_rnd(inout uint rng) {
	rng = nextgi_pt_hash(rng);
	return float(rng & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

float nextgi_pt_schlick(float cos_theta) {
	float complement = 1.0 - cos_theta;
	float squared = complement * complement;
	return squared * squared * complement;
}

float nextgi_pt_d_ggx(float n_dot_h, float alpha) {
	float a = n_dot_h * alpha;
	float denom = 1.0 - n_dot_h * n_dot_h + a * a;
	float k = alpha / denom;
	return k * k * (1.0 / NEXTGI_PT_PI);
}

float nextgi_pt_v_ggx(float n_dot_l, float n_dot_v, float alpha) {
	float denom = mix(2.0 * n_dot_l * n_dot_v, n_dot_l + n_dot_v, alpha);
	return denom > 0.0 ? 0.5 / denom : 0.0;
}

void nextgi_pt_basis(vec3 normal, out vec3 tangent, out vec3 bitangent) {
	vec3 helper = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(helper, normal));
	bitangent = cross(normal, tangent);
}

vec3 nextgi_pt_f0(uint material) {
	vec4 albedo_metal = materials.data[material].albedo_metal;
	float specular = materials.data[material].specular_mode.x;
	float dielectric = 0.16 * specular * specular;
	return mix(vec3(dielectric), albedo_metal.rgb, albedo_metal.w);
}

vec3 nextgi_pt_diffuse_albedo(uint material) {
	vec4 albedo_metal = materials.data[material].albedo_metal;
	return albedo_metal.rgb * (1.0 - albedo_metal.w);
}

float nextgi_pt_diffuse_nl(uint material, vec3 normal, vec3 view, vec3 light_dir, float n_dot_l) {
	if (materials.data[material].specular_mode.y < 0.5) {
		return n_dot_l * (1.0 / NEXTGI_PT_PI);
	}
	float roughness = materials.data[material].emission_roughness.w;
	float n_dot_v = max(dot(normal, view), 1e-4);
	vec3 half_vec = normalize(view + light_dir);
	float l_dot_h = clamp(dot(light_dir, half_vec), 0.0, 1.0);
	float fd90_minus_1 = 2.0 * l_dot_h * l_dot_h * roughness - 0.5;
	float fdv = 1.0 + fd90_minus_1 * nextgi_pt_schlick(n_dot_v);
	float fdl = 1.0 + fd90_minus_1 * nextgi_pt_schlick(n_dot_l);
	return (1.0 / NEXTGI_PT_PI) * fdv * fdl * n_dot_l;
}

vec3 nextgi_pt_specular_nl(uint material, vec3 normal, vec3 view, vec3 light_dir, float n_dot_l) {
	float roughness = materials.data[material].emission_roughness.w;
	if (roughness <= 0.0 || n_dot_l <= 0.0) {
		return vec3(0.0);
	}
	vec3 f0 = nextgi_pt_f0(material);
	float n_dot_v = max(dot(normal, view), 1e-4);
	vec3 half_vec = normalize(view + light_dir);
	float n_dot_h = clamp(dot(normal, half_vec), 0.0, 1.0);
	float l_dot_h = clamp(dot(light_dir, half_vec), 0.0, 1.0);
	float alpha = roughness * roughness;
	float distribution = nextgi_pt_d_ggx(n_dot_h, alpha);
	float visibility = nextgi_pt_v_ggx(n_dot_l, n_dot_v, alpha);
	float metallic = materials.data[material].albedo_metal.w;
	float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), metallic, 1.0);
	vec3 fresnel = f0 + (vec3(f90) - f0) * nextgi_pt_schlick(l_dot_h);
	return n_dot_l * distribution * fresnel * visibility;
}

float nextgi_pt_omni(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

vec3 nextgi_pt_mul_point(NextGIInstanceRecord rec, vec3 p) {
	vec3 r0 = vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]);
	vec3 r1 = vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]);
	vec3 r2 = vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]);
	vec3 origin = vec3(rec.object_to_world[3], rec.object_to_world[7], rec.object_to_world[11]);
	return vec3(dot(r0, p), dot(r1, p), dot(r2, p)) + origin;
}

vec3 nextgi_pt_mul_normal(NextGIInstanceRecord rec, vec3 n) {
	vec3 r0 = vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]);
	vec3 r1 = vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]);
	vec3 r2 = vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]);
	return normalize(vec3(dot(r0, n), dot(r1, n), dot(r2, n)));
}

uint nextgi_pt_chunk_uint(uint chunk, uint byte_offset) {
	uint word = byte_offset >> 2u;
	if (chunk == 0u) {
		return chunk0.data[word];
	}
	if (chunk == 1u) {
		return chunk1.data[word];
	}
	if (chunk == 2u) {
		return chunk2.data[word];
	}
	return chunk3.data[word];
}

vec3 nextgi_pt_chunk_vec3(uint chunk, uint byte_offset) {
	return vec3(uintBitsToFloat(nextgi_pt_chunk_uint(chunk, byte_offset)), uintBitsToFloat(nextgi_pt_chunk_uint(chunk, byte_offset + 4u)), uintBitsToFloat(nextgi_pt_chunk_uint(chunk, byte_offset + 8u)));
}

bool nextgi_pt_hit_surface(NextGIHit hit, out vec3 world_pos, out vec3 world_normal, out uint material) {
	world_pos = vec3(0.0);
	world_normal = vec3(0.0, 1.0, 0.0);
	material = 0u;
	if (!hit.hit || hit.custom_index >= ref_params.counts.w) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	uint surface_index = rec.surface_base + hit.geometry_index;
	if (surface_index >= ref_params.counts.z) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[surface_index];
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_pt_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_pt_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i2 * 12u);
	vec3 object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	vec3 face = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face = -face;
	}
	world_pos = nextgi_pt_mul_point(rec, object_pos);
	world_normal = nextgi_pt_mul_normal(rec, face);
	material = surface_index;
	return true;
}

vec3 nextgi_pt_cosine(vec3 normal, vec2 random_uv) {
	float phi = 2.0 * NEXTGI_PT_PI * random_uv.x;
	float sine = sqrt(random_uv.y);
	float cosine = sqrt(max(0.0, 1.0 - random_uv.y));
	vec3 tangent;
	vec3 bitangent;
	nextgi_pt_basis(normal, tangent, bitangent);
	return normalize(tangent * (sine * cos(phi)) + bitangent * (sine * sin(phi)) + normal * cosine);
}

vec3 nextgi_pt_next_event(vec3 position, vec3 normal, vec3 view, uint material) {
	vec3 lit = vec3(0.0);
	uint light_count = min(ref_params.counts.y, 8u);
	for (uint i = 0u; i < light_count; i++) {
		vec4 position_range = lights.data[i * 3u];
		vec4 color_energy = lights.data[i * 3u + 1u];
		vec4 extra = lights.data[i * 3u + 2u];
		vec3 shadow_origin = position + normal * NEXTGI_PT_ORIGIN_OFFSET;
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
		NextGIHit shadow_hit;
		if (nextgi_trace(shadow_origin, light_dir, dist - NEXTGI_PT_ORIGIN_OFFSET, shadow_hit)) {
			continue;
		}
		float attenuation = nextgi_pt_omni(dist, 1.0 / max(position_range.w, 1e-4), extra.x);
		vec3 light_color = color_energy.rgb * color_energy.w * NEXTGI_PT_PI;
		float specular_amount = extra.y * 2.0;
		vec3 diffuse = light_color * attenuation * nextgi_pt_diffuse_nl(material, normal, view, light_dir, n_dot_l) * nextgi_pt_diffuse_albedo(material);
		vec3 specular = light_color * attenuation * nextgi_pt_specular_nl(material, normal, view, light_dir, n_dot_l) * specular_amount;
		lit += diffuse + specular;
	}
	return lit;
}

float nextgi_pt_lambda(float n_dot_x, float alpha) {
	float alpha2 = alpha * alpha;
	float cosine2 = max(n_dot_x * n_dot_x, 1e-8);
	return 0.5 * (-1.0 + sqrt(1.0 + alpha2 * (1.0 - cosine2) / cosine2));
}

vec3 nextgi_pt_ggx(vec3 normal, vec3 view, float alpha, vec2 random_uv) {
	vec3 tangent;
	vec3 bitangent;
	nextgi_pt_basis(normal, tangent, bitangent);
	vec3 view_local = vec3(dot(view, tangent), dot(view, bitangent), dot(view, normal));
	if (view_local.z < 0.0) {
		view_local.z = 0.0;
	}
	vec3 stretched = normalize(vec3(alpha * view_local.x, alpha * view_local.y, view_local.z));
	float lensq = stretched.x * stretched.x + stretched.y * stretched.y;
	vec3 t1 = lensq > 0.0 ? vec3(-stretched.y, stretched.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(stretched, t1);
	float radius = sqrt(random_uv.x);
	float phi = 2.0 * NEXTGI_PT_PI * random_uv.y;
	float t1s = radius * cos(phi);
	float t2s = radius * sin(phi);
	float cap = 0.5 * (1.0 + stretched.z);
	t2s = (1.0 - cap) * sqrt(max(0.0, 1.0 - t1s * t1s)) + cap * t2s;
	vec3 hemisphere = t1 * t1s + t2 * t2s + stretched * sqrt(max(0.0, 1.0 - t1s * t1s - t2s * t2s));
	vec3 half_local = normalize(vec3(alpha * hemisphere.x, alpha * hemisphere.y, max(0.0, hemisphere.z)));
	vec3 half_world = tangent * half_local.x + bitangent * half_local.y + normal * half_local.z;
	return reflect(-view, half_world);
}

float nextgi_pt_ggx_pdf(uint material, vec3 normal, vec3 view, vec3 light_dir) {
	float roughness = materials.data[material].emission_roughness.w;
	float alpha = roughness * roughness;
	vec3 half_vec = normalize(view + light_dir);
	float n_dot_h = clamp(dot(normal, half_vec), 0.0, 1.0);
	float n_dot_v = max(dot(normal, view), 1e-4);
	float v_dot_h = max(dot(view, half_vec), 0.0);
	if (v_dot_h <= 0.0) {
		return 0.0;
	}
	float distribution = nextgi_pt_d_ggx(n_dot_h, alpha);
	float g1 = 1.0 / (1.0 + nextgi_pt_lambda(n_dot_v, alpha));
	return distribution * g1 / (4.0 * n_dot_v);
}

vec3 nextgi_pt_sample(uint material, vec3 normal, vec3 view, inout uint rng, out vec3 direction, out float pdf) {
	direction = normal;
	pdf = 0.0;
	vec3 reflected = nextgi_pt_diffuse_albedo(material);
	vec3 f0 = nextgi_pt_f0(material);
	float roughness = materials.data[material].emission_roughness.w;
	float spec_weight = max(f0.r, max(f0.g, f0.b));
	float diff_weight = max(reflected.r, max(reflected.g, reflected.b));
	float p_spec = 0.0;
	if (roughness > 0.0 && spec_weight + diff_weight > 0.0) {
		p_spec = spec_weight / (spec_weight + diff_weight);
	}
	vec2 random_uv = vec2(nextgi_pt_rnd(rng), nextgi_pt_rnd(rng));
	float lobe = nextgi_pt_rnd(rng);
	if (lobe < p_spec) {
		float alpha = roughness * roughness;
		direction = nextgi_pt_ggx(normal, view, alpha, random_uv);
		float n_dot_l = dot(normal, direction);
		if (n_dot_l <= 0.0) {
			return vec3(0.0);
		}
		pdf = nextgi_pt_ggx_pdf(material, normal, view, direction) * p_spec;
		if (pdf <= 0.0) {
			return vec3(0.0);
		}
		return nextgi_pt_specular_nl(material, normal, view, direction, n_dot_l) / pdf;
	}
	direction = nextgi_pt_cosine(normal, random_uv);
	float n_dot_l = max(dot(normal, direction), 0.0);
	float cosine_pdf = n_dot_l * (1.0 / NEXTGI_PT_PI);
	pdf = cosine_pdf * (1.0 - p_spec);
	if (pdf <= 0.0) {
		return vec3(0.0);
	}
	return reflected * nextgi_pt_diffuse_nl(material, normal, view, direction, n_dot_l) / pdf;
}

vec3 nextgi_pt_radiance_at(vec3 position, vec3 normal, vec3 wo, uint material, inout uint rng) {
	vec3 throughput = vec3(1.0);
	vec3 accumulated = materials.data[material].emission_roughness.rgb;
	vec3 view = wo;
	accumulated += nextgi_pt_next_event(position, normal, view, material);
	if (ref_params.dispatch.w != 0u) {
		return accumulated;
	}
	for (uint bounce = 1u; bounce < NEXTGI_PT_BOUNCE_CAP; bounce++) {
		if (bounce >= NEXTGI_PT_ROULETTE_FROM) {
			float survival = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
			if (survival <= 0.0 || nextgi_pt_rnd(rng) > survival) {
				break;
			}
			throughput /= survival;
		}
		vec3 direction;
		float pdf = 0.0;
		vec3 weight = nextgi_pt_sample(material, normal, view, rng, direction, pdf);
		if (pdf <= 0.0 || weight.r != weight.r) {
			break;
		}
		throughput *= weight;
		vec3 origin = position + normal * NEXTGI_PT_ORIGIN_OFFSET;
		NextGIHit hit;
		if (!nextgi_trace(origin, direction, 1.0e6, hit)) {
			accumulated += throughput * ref_params.sky_color.rgb;
			break;
		}
		if (!nextgi_pt_hit_surface(hit, position, normal, material)) {
			break;
		}
		view = -direction;
		accumulated += throughput * materials.data[material].emission_roughness.rgb;
		accumulated += throughput * nextgi_pt_next_event(position, normal, view, material);
		if (ref_params.dispatch.w != 0u) {
			break;
		}
	}
	return accumulated;
}

// GodotNext: nextgi_reference_reflections (#60 step 6). One GGX lobe sample for the
// reflection-only reference; the out parameters feed the per-pixel hit distance and hit fraction.
vec3 nextgi_pt_sample_ggx(vec3 p_normal, vec3 p_view, float p_roughness, inout uint p_rng) {
	float alpha = max(p_roughness * p_roughness, 1e-3);
	vec3 tangent;
	vec3 bitangent;
	nextgi_pt_basis(p_normal, tangent, bitangent);
	float u1 = nextgi_pt_rnd(p_rng);
	float u2 = nextgi_pt_rnd(p_rng);
	float phi = 6.2831853 * u1;
	float cos_theta = sqrt((1.0 - u2) / (1.0 + (alpha * alpha - 1.0) * u2));
	float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
	vec3 h = tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + p_normal * cos_theta;
	return reflect(-p_view, normalize(h));
}

vec3 nextgi_pt_reflection(vec3 p_position, vec3 p_normal, vec3 p_view, uint p_material, inout uint p_rng, out float r_distance, out float r_hit) {
	r_distance = 0.0;
	r_hit = 0.0;
	const float roughness = materials.data[p_material].emission_roughness.w;
	vec3 direction = nextgi_pt_sample_ggx(p_normal, p_view, roughness, p_rng);
	vec3 origin = p_position + p_normal * NEXTGI_PT_ORIGIN_OFFSET;
	NextGIHit hit;
	if (!nextgi_trace(origin, direction, 1.0e6, hit)) {
		return ref_params.sky_color.rgb;
	}
	r_distance = hit.t;
	r_hit = 1.0;
	vec3 position;
	vec3 normal;
	uint material;
	if (!nextgi_pt_hit_surface(hit, position, normal, material)) {
		return ref_params.sky_color.rgb;
	}
	return nextgi_pt_radiance_at(position, normal, -direction, material, p_rng);
}

vec3 nextgi_pt_radiance(vec3 origin, vec3 wo, inout uint rng) {
	NextGIHit hit;
	if (!nextgi_trace(origin, -wo, 0.2, hit)) {
		return ref_params.sky_color.rgb;
	}
	vec3 position;
	vec3 normal;
	uint material;
	if (!nextgi_pt_hit_surface(hit, position, normal, material)) {
		return ref_params.sky_color.rgb;
	}
	return nextgi_pt_radiance_at(position, normal, wo, material, rng);
}
