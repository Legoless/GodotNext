/**************************************************************************/
/*  megalights_inputs.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// GodotNext: megalights_inputs (#136). Header-only so #143 can take over
// MegaLightsInputs::is_requested() without a new translation unit.

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

namespace RendererRD {

class MegaLightsInputs {
	struct State {
		bool override_set = false;
		bool env_latched = false;
		bool requested = false;
		bool overflow_logged = false;
		LocalVector<uint32_t> masks;
		HashMap<uint32_t, uint8_t> lookup;
	};

	static State &_state() {
		static State state;
		return state;
	}

public:
	static constexpr uint8_t MODEL_STANDARD = 0;
	static constexpr uint8_t MODEL_SSS = 1;
	static constexpr uint8_t MODEL_CLEARCOAT = 2;
	static constexpr uint8_t MODEL_FALLBACK = 3;
	static constexpr uint8_t FLAG_CLEARCOAT = 1u << 2;
	static constexpr uint8_t FLAG_TRANSMISSION = 1u << 3;
	static constexpr uint8_t FLAG_SSS = 1u << 4;
	static constexpr uint8_t LAYER_GROUP_OVERFLOW = 255;
	static constexpr uint32_t LAYER_GROUP_LIMIT = 255;

	static constexpr uint32_t FALLBACK_CUSTOM_LIGHT = 1u << 0;
	static constexpr uint32_t FALLBACK_UNSHADED = 1u << 1;
	static constexpr uint32_t FALLBACK_DIFFUSE_TOON = 1u << 2;
	static constexpr uint32_t FALLBACK_DIFFUSE_LAMBERT_WRAP = 1u << 3;
	static constexpr uint32_t FALLBACK_SPECULAR_TOON = 1u << 4;
	static constexpr uint32_t FALLBACK_SPECULAR_DISABLED = 1u << 5;
	static constexpr uint32_t FALLBACK_RIM = 1u << 6;
	static constexpr uint32_t FALLBACK_BACKLIGHT = 1u << 7;
	static constexpr uint32_t FALLBACK_ANISOTROPY = 1u << 8;
	static constexpr uint32_t FALLBACK_DIFFUSE_LAMBERT = 1u << 9;
	static constexpr uint32_t FALLBACK_ALPHA_PASS = 1u << 10;

	// Hundredths are the source for both the C++ pack and the GLSL defines.
	static constexpr int LUMA_R_CENTI = 30;
	static constexpr int LUMA_G_CENTI = 59;
	static constexpr int LUMA_B_CENTI = 11;
	static constexpr int CB_SCALE_CENTI = 178;
	static constexpr int CR_SCALE_CENTI = 140;
	static constexpr int DIELECTRIC_CENTI = 16;
	// 15 codes, 0..14. Bias 7 is exact zero chroma, so gray F0 does not sit on a rounding tie.
	static constexpr int CHROMA_LEVELS = 14;
	static constexpr int CHROMA_BIAS = 7;

	static constexpr float LUMA_R = 0.30f;
	static constexpr float LUMA_G = 0.59f;
	static constexpr float LUMA_B = 0.11f;
	static constexpr float CB_SCALE = 1.78f;
	static constexpr float CR_SCALE = 1.40f;
	static constexpr float DIELECTRIC_SCALE = 0.16f;

	// ml_albedo.rgb is the diffuse albedo, albedo * (1 - metallic).
	// ml_albedo.a is the direct-light occlusion the clustered loop applies,
	// mix(1.0, ao, ao_light_affect). The bent-normal cone is indirect specular
	// only, so it is not stored. F0 is packed from the base albedo, not the
	// diffuse product, so metals keep their tint.
	//
	// #143 replaces is_requested() with the project setting. Until then the
	// switch is GODOTNEXT_MEGALIGHTS_INPUTS=1, read once, or set_requested().
	static bool is_requested() {
		State &state = _state();
		if (state.override_set) {
			return state.requested;
		}
		if (!state.env_latched) {
			state.env_latched = true;
			OS *os = OS::get_singleton();
			if (os != nullptr) {
				state.requested = os->get_environment("GODOTNEXT_MEGALIGHTS_INPUTS") == "1";
			}
		}
		return state.requested;
	}

	static void set_requested(bool p_requested) {
		State &state = _state();
		state.override_set = true;
		state.requested = p_requested;
	}

	static bool view_writes_inputs(bool p_msaa) {
		if (!is_requested()) {
			return false;
		}
		if (p_msaa) {
			WARN_PRINT_ONCE("MegaLights is disabled for this view because MSAA is enabled.");
			return false;
		}
		return true;
	}

	// The per-view gate for the MegaLights shader groups. They are enabled only for a
	// view that writes the inputs; with MegaLights off or MSAA on nothing is enabled.
	// The renderer passes SceneShaderForwardClustered; the doctest passes a recorder.
	template <typename T>
	static bool enable_view_shader_groups(T &r_shader, bool p_msaa, bool p_multiview, bool p_nextgeo) {
		if (!view_writes_inputs(p_msaa)) {
			return false;
		}
		r_shader.enable_megalights_shader_group(p_multiview);
		if (p_nextgeo) {
			r_shader.enable_nextgeo_megalights_shader_group();
		}
		return true;
	}

	static void begin_layer_groups() {
		State &state = _state();
		state.masks.clear();
		state.lookup.clear();
		state.overflow_logged = false;
	}

	static uint8_t assign_layer_group(uint32_t p_mask) {
		State &state = _state();
		if (const uint8_t *found = state.lookup.getptr(p_mask)) {
			return *found;
		}
		if (state.masks.size() >= LAYER_GROUP_LIMIT) {
			if (!state.overflow_logged) {
				state.overflow_logged = true;
				WARN_PRINT_ONCE("MegaLights layer-group table exceeded 255 distinct instance layer masks. Further masks use the match-all index 255.");
			}
			return LAYER_GROUP_OVERFLOW;
		}
		const uint8_t index = uint8_t(state.masks.size());
		state.masks.push_back(p_mask);
		state.lookup.insert(p_mask, index);
		return index;
	}

	static const LocalVector<uint32_t> &get_layer_group_masks() {
		return _state().masks;
	}

	static uint32_t get_layer_group_count() {
		return _state().masks.size();
	}

	struct FallbackQuery {
		bool unshaded = false;
		bool custom_light = false;
		bool diffuse_toon = false;
		bool diffuse_lambert_wrap = false;
		bool specular_toon = false;
		bool specular_disabled = false;
		bool rim = false;
		bool backlight = false;
		bool anisotropy = false;
		bool diffuse_lambert = false;
		// Drawn by the depth prepass (depth_prepass_alpha or alpha antialiasing) but shaded
		// by the alpha pass, so the opaque color pass never applies its inputs.
		bool alpha_pass = false;
	};

	// Lambert is the lights-inc #else: explicit DIFFUSE_LAMBERT, or a shaded
	// material with neither Burley, toon, nor wrap. Unshaded is its own flag.
	static bool is_diffuse_lambert(bool p_unshaded, bool p_has_lambert, bool p_has_burley, bool p_has_toon, bool p_has_wrap) {
		if (p_unshaded) {
			return false;
		}
		return p_has_lambert || (!p_has_burley && !p_has_toon && !p_has_wrap);
	}

	static uint32_t fallback_bits(const FallbackQuery &p_query) {
		uint32_t bits = 0;
		if (p_query.custom_light) {
			bits |= FALLBACK_CUSTOM_LIGHT;
		}
		if (p_query.unshaded) {
			bits |= FALLBACK_UNSHADED;
		}
		if (p_query.diffuse_toon) {
			bits |= FALLBACK_DIFFUSE_TOON;
		}
		if (p_query.diffuse_lambert_wrap) {
			bits |= FALLBACK_DIFFUSE_LAMBERT_WRAP;
		}
		if (p_query.specular_toon) {
			bits |= FALLBACK_SPECULAR_TOON;
		}
		if (p_query.specular_disabled) {
			bits |= FALLBACK_SPECULAR_DISABLED;
		}
		if (p_query.rim) {
			bits |= FALLBACK_RIM;
		}
		if (p_query.backlight) {
			bits |= FALLBACK_BACKLIGHT;
		}
		if (p_query.anisotropy) {
			bits |= FALLBACK_ANISOTROPY;
		}
		if (p_query.diffuse_lambert) {
			bits |= FALLBACK_DIFFUSE_LAMBERT;
		}
		if (p_query.alpha_pass) {
			bits |= FALLBACK_ALPHA_PASS;
		}
		return bits;
	}

	static bool is_fallback(const FallbackQuery &p_query) {
		return fallback_bits(p_query) != 0;
	}

	// B: bits 0-1 shading model, bits 2-4 flags. Fallback wins and clears flags.
	// Model 1 is SSS. Transmission is the color pass's LIGHT_TRANSMITTANCE_USED,
	// which requires SSS as well. Transmission alone stays model 0.
	static uint8_t pack_shading_model(bool p_fallback, bool p_sss, bool p_transmission, bool p_clearcoat) {
		if (p_fallback) {
			return MODEL_FALLBACK;
		}
		const bool transmittance = p_sss && p_transmission;
		uint8_t flags = 0;
		if (p_clearcoat) {
			flags |= FLAG_CLEARCOAT;
		}
		if (transmittance) {
			flags |= FLAG_TRANSMISSION;
		}
		if (p_sss) {
			flags |= FLAG_SSS;
		}
		uint8_t model = MODEL_STANDARD;
		if (p_sss) {
			model = MODEL_SSS;
		} else if (p_clearcoat) {
			model = MODEL_CLEARCOAT;
		}
		return uint8_t(model | flags);
	}

	static void f0_from_material(float p_metallic, float p_specular, float p_albedo_r, float p_albedo_g, float p_albedo_b, float &r_f0_r, float &r_f0_g, float &r_f0_b) {
		const float dielectric = DIELECTRIC_SCALE * p_specular * p_specular;
		r_f0_r = Math::lerp(dielectric, p_albedo_r, p_metallic);
		r_f0_g = Math::lerp(dielectric, p_albedo_g, p_metallic);
		r_f0_b = Math::lerp(dielectric, p_albedo_b, p_metallic);
	}

	static int quantize_unorm8(float p_value) {
		return int(Math::round(CLAMP(p_value, 0.0f, 1.0f) * 255.0f));
	}

	// n = clamp(round(c * 14) + 7, 0, 14). Zero chroma is index 7, not a .5 tie.
	static int quantize_nibble(float p_chroma) {
		return int(CLAMP(Math::round(p_chroma * float(CHROMA_LEVELS)) + float(CHROMA_BIAS), 0.0f, float(CHROMA_LEVELS)));
	}

	// Injected into every forward-clustered shader. The numbers and the chroma
	// rule exist only here; GLSL calls ML_QUANTIZE_CHROMA and the ML_* defines.
	static String glsl_pack_prelude() {
		String prelude;
		prelude += "#define ML_LUMA_R (" + String::num_int64(LUMA_R_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_LUMA_G (" + String::num_int64(LUMA_G_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_LUMA_B (" + String::num_int64(LUMA_B_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_CB_SCALE (" + String::num_int64(CB_SCALE_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_CR_SCALE (" + String::num_int64(CR_SCALE_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_DIELECTRIC_SCALE (" + String::num_int64(DIELECTRIC_CENTI) + ".0 / 100.0)\n";
		prelude += "#define ML_CHROMA_LEVELS " + String::num_int64(CHROMA_LEVELS) + "\n";
		prelude += "#define ML_CHROMA_BIAS " + String::num_int64(CHROMA_BIAS) + "\n";
		prelude += "#define ML_MODEL_SSS " + String::num_int64(MODEL_SSS) + "\n";
		prelude += "#define ML_MODEL_CLEARCOAT " + String::num_int64(MODEL_CLEARCOAT) + "\n";
		prelude += "#define ML_MODEL_FALLBACK " + String::num_int64(MODEL_FALLBACK) + "\n";
		prelude += "#define ML_FLAG_CLEARCOAT " + String::num_int64(FLAG_CLEARCOAT) + "\n";
		prelude += "#define ML_FLAG_TRANSMISSION " + String::num_int64(FLAG_TRANSMISSION) + "\n";
		prelude += "#define ML_FLAG_SSS " + String::num_int64(FLAG_SSS) + "\n";
		prelude += "#define ML_QUANTIZE_CHROMA(c) clamp(round((c) * float(ML_CHROMA_LEVELS)) + float(ML_CHROMA_BIAS), 0.0, float(ML_CHROMA_LEVELS))\n";
		return prelude;
	}

	// R is F0 luma. G is 4-bit Cb in the high nibble and 4-bit Cr in the low nibble.
	static void pack_f0(float p_f0_r, float p_f0_g, float p_f0_b, uint8_t &r_luma, uint8_t &r_tint) {
		const float luma = p_f0_r * LUMA_R + p_f0_g * LUMA_G + p_f0_b * LUMA_B;
		const float cb = (p_f0_b - luma) / CB_SCALE;
		const float cr = (p_f0_r - luma) / CR_SCALE;
		r_luma = uint8_t(quantize_unorm8(luma));
		const int cb4 = quantize_nibble(cb);
		const int cr4 = quantize_nibble(cr);
		r_tint = uint8_t((cb4 << 4) | cr4);
	}

	static void unpack_f0(uint8_t p_luma, uint8_t p_tint, float &r_f0_r, float &r_f0_g, float &r_f0_b) {
		const float luma = float(p_luma) / 255.0f;
		const float cb = (float((p_tint >> 4) & 0x0f) - float(CHROMA_BIAS)) / float(CHROMA_LEVELS);
		const float cr = (float(p_tint & 0x0f) - float(CHROMA_BIAS)) / float(CHROMA_LEVELS);
		r_f0_b = luma + cb * CB_SCALE;
		r_f0_r = luma + cr * CR_SCALE;
		r_f0_g = (luma - LUMA_R * r_f0_r - LUMA_B * r_f0_b) / LUMA_G;
	}
};

} // namespace RendererRD
