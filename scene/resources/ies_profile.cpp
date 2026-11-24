/**************************************************************************/
/*  ies_profile.cpp                                                       */
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

#include "ies_profile.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_server.h"

#include <cmath>
#include <cstdint>

namespace {

// Linearly interpolates a 1D monotonically non-decreasing table.
// Returns 0 outside [angles[0], angles[count - 1]] unless p_clamp_ends is true.
static float _sample_1d_sorted(const float *p_angles, const float *p_values, int p_count, float p_angle, bool p_clamp_ends = false) {
	if (p_count <= 0) {
		return 0.0f;
	}
	if (p_count == 1) {
		return p_clamp_ends ? p_values[0] : (Math::is_equal_approx(p_angle, p_angles[0]) ? p_values[0] : 0.0f);
	}
	const float eps = 1e-4f;
	if (p_angle < p_angles[0] - eps) {
		return p_clamp_ends ? p_values[0] : 0.0f;
	}
	if (p_angle > p_angles[p_count - 1] + eps) {
		return p_clamp_ends ? p_values[p_count - 1] : 0.0f;
	}
	if (p_angle <= p_angles[0]) {
		return p_values[0];
	}
	if (p_angle >= p_angles[p_count - 1]) {
		return p_values[p_count - 1];
	}

	int lo = 0;
	int hi = p_count - 1;
	while (hi - lo > 1) {
		const int mid = (lo + hi) >> 1;
		if (p_angles[mid] <= p_angle) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float span = p_angles[hi] - p_angles[lo];
	if (span <= 1e-6f) {
		return p_values[lo];
	}
	const float t = CLAMP((p_angle - p_angles[lo]) / span, 0.0f, 1.0f);
	return Math::lerp(p_values[lo], p_values[hi], t);
}

// Evaluates raw LM-63 Type C candela grid at (theta_deg in [0, 180], phi_deg in [0, 360]),
// applying standard ANSI/IES LM-63 horizontal symmetry folding.
static float _eval_type_c(const LocalVector<float> &p_v_angles, const LocalVector<float> &p_h_angles, const LocalVector<float> &p_candela, float p_theta_deg, float p_phi_deg) {
	const int nv = int(p_v_angles.size());
	const int nh = int(p_h_angles.size());
	if (nv <= 0 || nh <= 0) {
		return 0.0f;
	}

	// Single horizontal plane: axially symmetric (0 deg symmetry).
	if (nh == 1) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, p_theta_deg, false);
	}

	// Wrap phi into [0, 360).
	float phi = std::fmod(p_phi_deg, 360.0f);
	if (phi < 0.0f) {
		phi += 360.0f;
	}

	const float h_first = p_h_angles[0];
	const float h_last = p_h_angles[nh - 1];

	if (h_first <= 1e-3f && Math::abs(h_last - 90.0f) <= 1e-2f) {
		// Quadrant symmetry (0..90).
		if (phi > 180.0f) {
			phi = 360.0f - phi;
		}
		if (phi > 90.0f) {
			phi = 180.0f - phi;
		}
	} else if (h_first <= 1e-3f && Math::abs(h_last - 180.0f) <= 1e-2f) {
		// Bilateral symmetry across 0-180 plane (0..180).
		if (phi > 180.0f) {
			phi = 360.0f - phi;
		}
	} else if (Math::abs(h_first - 90.0f) <= 1e-2f && Math::abs(h_last - 270.0f) <= 1e-2f) {
		// Bilateral symmetry across 90-270 plane (90..270).
		if (phi < 90.0f) {
			phi = 180.0f - phi;
		} else if (phi > 270.0f) {
			phi = 540.0f - phi;
		}
	} else if (h_last > 180.0f + 1e-2f) {
		// Full 360 asymmetric distribution. Wrap-interpolate between h_last and 360 + h_first if needed.
		if (phi > h_last && h_last < 360.0f - 1e-3f) {
			const float span = (360.0f + h_first) - h_last;
			const float t = span > 1e-6f ? CLAMP((phi - h_last) / span, 0.0f, 1.0f) : 0.0f;
			const float c0 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + (nh - 1) * nv, nv, p_theta_deg, false);
			const float c1 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, p_theta_deg, false);
			return Math::lerp(c0, c1, t);
		}
		if (phi < h_first && h_first > 1e-3f) {
			const float span = (360.0f + h_first) - h_last;
			const float t = span > 1e-6f ? CLAMP((phi + 360.0f - h_last) / span, 0.0f, 1.0f) : 0.0f;
			const float c0 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + (nh - 1) * nv, nv, p_theta_deg, false);
			const float c1 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, p_theta_deg, false);
			return Math::lerp(c0, c1, t);
		}
	}

	// Find bracketing horizontal planes.
	if (phi <= h_first) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, p_theta_deg, false);
	}
	if (phi >= h_last) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + (nh - 1) * nv, nv, p_theta_deg, false);
	}
	int lo = 0;
	int hi = nh - 1;
	while (hi - lo > 1) {
		const int mid = (lo + hi) >> 1;
		if (p_h_angles[mid] <= phi) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float span = p_h_angles[hi] - p_h_angles[lo];
	const float t = span > 1e-6f ? CLAMP((phi - p_h_angles[lo]) / span, 0.0f, 1.0f) : 0.0f;
	const float c0 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + lo * nv, nv, p_theta_deg, false);
	const float c1 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + hi * nv, nv, p_theta_deg, false);
	return Math::lerp(c0, c1, t);
}

// Evaluates raw LM-63 Type B (horizontal polar axis +X) or Type A (vertical polar axis +Y)
// candela grid for a canonical direction vector d in light local space (forward = -Z, right = +X, up = +Y).
static float _eval_type_ab(int p_type, const LocalVector<float> &p_v_angles, const LocalVector<float> &p_h_angles, const LocalVector<float> &p_candela, const Vector3 &p_dir_ls) {
	const int nv = int(p_v_angles.size());
	const int nh = int(p_h_angles.size());
	if (nv <= 0 || nh <= 0) {
		return 0.0f;
	}

	// Aiming direction is -Z. Directions behind the luminaire emit zero in Type A/B.
	const float fwd = float(-p_dir_ls.z);
	if (fwd < -1e-6f) {
		return 0.0f;
	}

	float h_deg = 0.0f;
	float v_deg = 0.0f;
	if (p_type == IESProfile::PHOTOMETRIC_TYPE_B) {
		// Type B: d = (sin H, cos H * sin V, -cos H * cos V).
		h_deg = float(Math::rad_to_deg(double(std::asin(CLAMP(float(p_dir_ls.x), -1.0f, 1.0f)))));
		v_deg = float(Math::rad_to_deg(double(std::atan2(float(p_dir_ls.y), MAX(fwd, 0.0f)))));
	} else {
		// Type A: d = (cos V * sin H, sin V, -cos V * cos H).
		v_deg = float(Math::rad_to_deg(double(std::asin(CLAMP(float(p_dir_ls.y), -1.0f, 1.0f)))));
		h_deg = float(Math::rad_to_deg(double(std::atan2(float(p_dir_ls.x), MAX(fwd, 0.0f)))));
	}

	// Apply lateral/vertical symmetry if table starts at 0 rather than -90.
	if (p_h_angles[0] >= -1e-3f) {
		h_deg = std::fabs(h_deg);
	}
	if (p_v_angles[0] >= -1e-3f) {
		v_deg = std::fabs(v_deg);
	}

	if (nh == 1) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, v_deg, false);
	}
	if (h_deg < p_h_angles[0] - 1e-4f || h_deg > p_h_angles[nh - 1] + 1e-4f) {
		return 0.0f;
	}
	if (h_deg <= p_h_angles[0]) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr(), nv, v_deg, false);
	}
	if (h_deg >= p_h_angles[nh - 1]) {
		return _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + (nh - 1) * nv, nv, v_deg, false);
	}
	int lo = 0;
	int hi = nh - 1;
	while (hi - lo > 1) {
		const int mid = (lo + hi) >> 1;
		if (p_h_angles[mid] <= h_deg) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float span = p_h_angles[hi] - p_h_angles[lo];
	const float t = span > 1e-6f ? CLAMP((h_deg - p_h_angles[lo]) / span, 0.0f, 1.0f) : 0.0f;
	const float c0 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + lo * nv, nv, v_deg, false);
	const float c1 = _sample_1d_sorted(p_v_angles.ptr(), p_candela.ptr() + hi * nv, nv, v_deg, false);
	return Math::lerp(c0, c1, t);
}

static bool _tokenize_floats(const Vector<String> &p_lines, int p_start_line, LocalVector<float> &r_values) {
	r_values.clear();
	for (int i = p_start_line; i < p_lines.size(); i++) {
		const String &line = p_lines[i];
		const char32_t *ptr = line.ptr();
		const int len = line.length();
		int pos = 0;
		while (pos < len) {
			while (pos < len && (ptr[pos] == ' ' || ptr[pos] == '\t' || ptr[pos] == ',' || ptr[pos] == '\r' || ptr[pos] == '\n')) {
				pos++;
			}
			if (pos >= len) {
				break;
			}
			const int start = pos;
			while (pos < len && ptr[pos] != ' ' && ptr[pos] != '\t' && ptr[pos] != ',' && ptr[pos] != '\r' && ptr[pos] != '\n') {
				pos++;
			}
			const String token = line.substr(start, pos - start);
			if (!token.is_valid_float()) {
				return false;
			}
			const float val = float(token.to_float());
			if (!std::isfinite(val)) {
				return false;
			}
			r_values.push_back(val);
		}
	}
	return true;
}

} // namespace

Ref<Image> IESProfile::_build_r16f_image(const PackedFloat32Array &p_table) {
	const int expected = TABLE_WIDTH * TABLE_HEIGHT;
	ERR_FAIL_COND_V_MSG(p_table.size() != expected, Ref<Image>(), vformat("IESProfile: candela table must have %d entries (%dx%d), got %d.", expected, TABLE_WIDTH, TABLE_HEIGHT, p_table.size()));

	Vector<uint8_t> bytes;
	bytes.resize(expected * int(sizeof(uint16_t)));
	uint16_t *dst = reinterpret_cast<uint16_t *>(bytes.ptrw());
	const float *src = p_table.ptr();
	for (int i = 0; i < expected; i++) {
		const float clamped = std::isfinite(src[i]) ? CLAMP(src[i], 0.0f, 1.0f) : 0.0f;
		dst[i] = Math::make_half_float(clamped);
	}
	return Image::create_from_data(TABLE_WIDTH, TABLE_HEIGHT, false, Image::FORMAT_RH, bytes);
}

void IESProfile::_sync_to_server() const {
	if (ies_rid.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->ies_profile_set_data(ies_rid, profile_image, peak_candela);
	}
}

void IESProfile::set_profile_image(const Ref<Image> &p_image) {
	if (p_image.is_valid()) {
		ERR_FAIL_COND_MSG(p_image->get_width() != TABLE_WIDTH || p_image->get_height() != TABLE_HEIGHT,
				vformat("IESProfile: profile image must be %dx%d, got %dx%d.", TABLE_WIDTH, TABLE_HEIGHT, p_image->get_width(), p_image->get_height()));
		if (p_image->get_format() != Image::FORMAT_RH) {
			Ref<Image> converted = p_image->duplicate();
			converted->convert(Image::FORMAT_RH);
			profile_image = converted;
		} else {
			profile_image = p_image;
		}
	} else {
		profile_image = Ref<Image>();
	}
	_sync_to_server();
	emit_changed();
}

Ref<Image> IESProfile::get_profile_image() const {
	return profile_image;
}

void IESProfile::set_candela_table(const PackedFloat32Array &p_table) {
	Ref<Image> img = _build_r16f_image(p_table);
	if (img.is_valid()) {
		set_profile_image(img);
	}
}

PackedFloat32Array IESProfile::get_candela_table() const {
	PackedFloat32Array out;
	if (profile_image.is_null() || profile_image->get_width() != TABLE_WIDTH || profile_image->get_height() != TABLE_HEIGHT) {
		return out;
	}
	const int count = TABLE_WIDTH * TABLE_HEIGHT;
	out.resize(count);
	float *dst = out.ptrw();
	if (profile_image->get_format() == Image::FORMAT_RH) {
		const Vector<uint8_t> data = profile_image->get_data();
		if (data.size() >= count * int(sizeof(uint16_t))) {
			const uint16_t *src = reinterpret_cast<const uint16_t *>(data.ptr());
			for (int i = 0; i < count; i++) {
				dst[i] = Math::half_to_float(src[i]);
			}
			return out;
		}
	}
	for (int y = 0; y < TABLE_HEIGHT; y++) {
		for (int x = 0; x < TABLE_WIDTH; x++) {
			dst[y * TABLE_WIDTH + x] = profile_image->get_pixel(x, y).r;
		}
	}
	return out;
}

void IESProfile::set_peak_candela(float p_peak) {
	peak_candela = MAX(p_peak, 0.0f);
	_sync_to_server();
	emit_changed();
}

float IESProfile::get_peak_candela() const {
	return peak_candela;
}

void IESProfile::set_lumens(float p_lumens) {
	lumens = p_lumens;
}

float IESProfile::get_lumens() const {
	return lumens;
}

void IESProfile::set_watts(float p_watts) {
	watts = p_watts;
}

float IESProfile::get_watts() const {
	return watts;
}

void IESProfile::set_photometric_type(PhotometricType p_type) {
	ERR_FAIL_COND(p_type < PHOTOMETRIC_TYPE_C || p_type > PHOTOMETRIC_TYPE_A);
	photometric_type = p_type;
}

IESProfile::PhotometricType IESProfile::get_photometric_type() const {
	return photometric_type;
}

void IESProfile::set_vertical_angles(const PackedFloat32Array &p_angles) {
	vertical_angles = p_angles;
}

PackedFloat32Array IESProfile::get_vertical_angles() const {
	return vertical_angles;
}

void IESProfile::set_horizontal_angles(const PackedFloat32Array &p_angles) {
	horizontal_angles = p_angles;
}

PackedFloat32Array IESProfile::get_horizontal_angles() const {
	return horizontal_angles;
}

Error IESProfile::load_from_ies_string(const String &p_text) {
	ERR_FAIL_COND_V_MSG(p_text.is_empty(), ERR_PARSE_ERROR, "IESProfile: empty IES file content.");

	Vector<String> raw_lines = p_text.split("\n");
	Vector<String> lines;
	for (int i = 0; i < raw_lines.size(); i++) {
		String trimmed = raw_lines[i].strip_edges();
		if (!trimmed.is_empty()) {
			lines.push_back(trimmed);
		}
	}
	ERR_FAIL_COND_V_MSG(lines.size() < 4, ERR_PARSE_ERROR, "IESProfile: IES file has too few lines.");

	// Validate LM-63 header line (LM-63-1995 / LM-63-2002, also accepting IESNA91).
	const String &header_line = lines[0];
	bool is_lm63_1995 = false;
	if (header_line == "IESNA:LM-63-2002") {
		is_lm63_1995 = false;
	} else if (header_line == "IESNA:LM-63-1995" || header_line == "IESNA91") {
		is_lm63_1995 = true;
	} else {
		ERR_FAIL_V_MSG(ERR_PARSE_ERROR, vformat("IESProfile: unsupported IES header '%s' (expected IESNA:LM-63-1995 or IESNA:LM-63-2002).", header_line));
	}

	// Scan keyword metadata lines until TILT=.
	int tilt_line = -1;
	for (int i = 1; i < lines.size(); i++) {
		if (lines[i].begins_with("TILT=")) {
			tilt_line = i;
			break;
		}
	}
	ERR_FAIL_COND_V_MSG(tilt_line < 0, ERR_PARSE_ERROR, "IESProfile: missing required TILT= line.");

	const String tilt_value = lines[tilt_line].substr(5).strip_edges();
	if (tilt_value != "NONE") {
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("IESProfile: TILT=%s is not supported; only TILT=NONE IES files can be imported.", tilt_value));
	}

	LocalVector<float> nums;
	ERR_FAIL_COND_V_MSG(!_tokenize_floats(lines, tilt_line + 1, nums), ERR_PARSE_ERROR, "IESProfile: failed to parse numeric data after TILT=NONE.");
	// Minimum header: 10 luminaire fields + 3 ballast/watt fields = 13 numbers.
	ERR_FAIL_COND_V_MSG(nums.size() < 13u, ERR_PARSE_ERROR, "IESProfile: truncated photometric header after TILT=NONE.");

	const int num_lamps = int(Math::round(nums[0]));
	const float lumens_per_lamp = nums[1];
	const float candela_mult = nums[2];
	const int num_vertical = int(Math::round(nums[3]));
	const int num_horizontal = int(Math::round(nums[4]));
	const int photo_type = int(Math::round(nums[5]));
	// nums[6] = units type (1=feet, 2=meters), nums[7..9] = width, length, height.
	const float ballast_factor = nums[10];
	const float field_12 = nums[11]; // LM-63-1995: ballast-lamp photometric factor; LM-63-2002: future use (ignored).
	const float input_watts = nums[12];

	ERR_FAIL_COND_V_MSG(num_vertical <= 0 || num_horizontal <= 0, ERR_PARSE_ERROR, "IESProfile: vertical and horizontal angle counts must be positive.");
	ERR_FAIL_COND_V_MSG(num_vertical > 16384 || num_horizontal > 16384, ERR_PARSE_ERROR, "IESProfile: excessive angle count in header.");
	ERR_FAIL_COND_V_MSG(photo_type < PHOTOMETRIC_TYPE_C || photo_type > PHOTOMETRIC_TYPE_A, ERR_PARSE_ERROR, vformat("IESProfile: invalid photometric type %d (expected 1=C, 2=B, or 3=A).", photo_type));
	ERR_FAIL_COND_V_MSG(candela_mult <= 0.0f || ballast_factor <= 0.0f, ERR_PARSE_ERROR, "IESProfile: candela multiplier and ballast factor must be positive.");

	const uint64_t expected_nums = 13ull + uint64_t(num_vertical) + uint64_t(num_horizontal) + uint64_t(num_vertical) * uint64_t(num_horizontal);
	ERR_FAIL_COND_V_MSG(nums.size() < expected_nums, ERR_PARSE_ERROR, vformat("IESProfile: expected %d numeric values, found %d.", int64_t(expected_nums), int64_t(nums.size())));

	LocalVector<float> v_angles;
	v_angles.resize(num_vertical);
	for (int i = 0; i < num_vertical; i++) {
		v_angles[i] = nums[13 + i];
		if (i > 0 && v_angles[i] < v_angles[i - 1] - 1e-4f) {
			ERR_FAIL_V_MSG(ERR_PARSE_ERROR, "IESProfile: vertical angles must be monotonically non-decreasing.");
		}
	}

	LocalVector<float> h_angles;
	h_angles.resize(num_horizontal);
	const int h_offset = 13 + num_vertical;
	for (int i = 0; i < num_horizontal; i++) {
		h_angles[i] = nums[h_offset + i];
		if (i > 0 && h_angles[i] < h_angles[i - 1] - 1e-4f) {
			ERR_FAIL_V_MSG(ERR_PARSE_ERROR, "IESProfile: horizontal angles must be monotonically non-decreasing.");
		}
	}

	// Effective scale multiplier: per LM-63-2002 field 12 is reserved for future use and NOT multiplied
	// (e.g., ies_gen.py writes 0.0 there). Per LM-63-1995, field 12 is the ballast-lamp photometric factor.
	const float blpf = (is_lm63_1995 && field_12 > 0.0f) ? field_12 : 1.0f;
	const float total_scale = candela_mult * ballast_factor * blpf;

	const int candela_offset = h_offset + num_horizontal;
	const int candela_count = num_vertical * num_horizontal;
	LocalVector<float> raw_candela;
	raw_candela.resize(candela_count);
	float peak = 0.0f;
	for (int i = 0; i < candela_count; i++) {
		const float c = nums[candela_offset + i] * total_scale;
		ERR_FAIL_COND_V_MSG(c < -1e-4f, ERR_PARSE_ERROR, "IESProfile: candela table contains negative values.");
		const float clamped_c = MAX(c, 0.0f);
		raw_candela[i] = clamped_c;
		peak = MAX(peak, clamped_c);
	}
	ERR_FAIL_COND_V_MSG(peak <= 0.0f, ERR_PARSE_ERROR, "IESProfile: candela table has zero peak intensity.");

	// Normalize raw candela values to [0, 1].
	const float inv_peak = 1.0f / peak;
	for (int i = 0; i < candela_count; i++) {
		raw_candela[i] *= inv_peak;
	}

	// Resample symmetrically into the canonical 256 x 64 table:
	// x in [0, 255] -> theta = 180 * x / 255 (polar angle from forward -Z axis),
	// y in [0, 63]  -> phi   = 360 * y / 63  (azimuthal angle around -Z, closed [0, 360] so CLAMP_TO_EDGE wraps seamlessly).
	PackedFloat32Array resampled;
	resampled.resize(TABLE_WIDTH * TABLE_HEIGHT);
	float *dst = resampled.ptrw();

	for (int y = 0; y < TABLE_HEIGHT; y++) {
		const float phi_deg = 360.0f * (float(y) / float(TABLE_HEIGHT - 1));
		const float phi_rad = float(Math::deg_to_rad(double(phi_deg)));
		const float cos_phi = std::cos(phi_rad);
		const float sin_phi = std::sin(phi_rad);
		for (int x = 0; x < TABLE_WIDTH; x++) {
			const float theta_deg = 180.0f * (float(x) / float(TABLE_WIDTH - 1));
			float val = 0.0f;
			if (photo_type == PHOTOMETRIC_TYPE_C) {
				val = _eval_type_c(v_angles, h_angles, raw_candela, theta_deg, phi_deg);
			} else {
				const float theta_rad = float(Math::deg_to_rad(double(theta_deg)));
				const float sin_t = std::sin(theta_rad);
				const float cos_t = std::cos(theta_rad);
				const Vector3 dir_ls(sin_t * cos_phi, sin_t * sin_phi, -cos_t);
				val = _eval_type_ab(photo_type, v_angles, h_angles, raw_candela, dir_ls);
			}
			dst[y * TABLE_WIDTH + x] = CLAMP(val, 0.0f, 1.0f);
		}
	}

	Ref<Image> img = _build_r16f_image(resampled);
	ERR_FAIL_COND_V(img.is_null(), ERR_CANT_CREATE);

	profile_image = img;
	peak_candela = peak;
	lumens = (num_lamps > 0 && lumens_per_lamp > 0.0f) ? (float(num_lamps) * lumens_per_lamp) : 0.0f;
	watts = MAX(input_watts, 0.0f);
	photometric_type = PhotometricType(photo_type);

	vertical_angles.resize(num_vertical);
	for (int i = 0; i < num_vertical; i++) {
		vertical_angles.write[i] = v_angles[i];
	}
	horizontal_angles.resize(num_horizontal);
	for (int i = 0; i < num_horizontal; i++) {
		horizontal_angles.write[i] = h_angles[i];
	}

	_sync_to_server();
	emit_changed();
	return OK;
}

Error IESProfile::load_from_ies_file(const String &p_path) {
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(err != OK || f.is_null(), err != OK ? err : ERR_FILE_CANT_OPEN, vformat("IESProfile: cannot open '%s'.", p_path));
	const String text = f->get_as_text();
	return load_from_ies_string(text);
}

float IESProfile::sample_angles(float p_vertical_deg, float p_horizontal_deg) const {
	if (profile_image.is_null() || profile_image->get_width() != TABLE_WIDTH || profile_image->get_height() != TABLE_HEIGHT) {
		return 1.0f;
	}
	const float theta = CLAMP(p_vertical_deg, 0.0f, 180.0f);
	float phi = std::fmod(p_horizontal_deg, 360.0f);
	if (phi < 0.0f) {
		phi += 360.0f;
	}

	const float fx = (theta / 180.0f) * float(TABLE_WIDTH - 1);
	const float fy = (phi / 360.0f) * float(TABLE_HEIGHT - 1);

	const int x0 = CLAMP(int(std::floor(fx)), 0, TABLE_WIDTH - 1);
	const int x1 = MIN(x0 + 1, TABLE_WIDTH - 1);
	const int y0 = CLAMP(int(std::floor(fy)), 0, TABLE_HEIGHT - 1);
	const int y1 = MIN(y0 + 1, TABLE_HEIGHT - 1);
	const float tx = CLAMP(fx - float(x0), 0.0f, 1.0f);
	const float ty = CLAMP(fy - float(y0), 0.0f, 1.0f);

	auto read_texel = [&](int p_x, int p_y) -> float {
		if (profile_image->get_format() == Image::FORMAT_RH) {
			const Vector<uint8_t> data = profile_image->get_data();
			const int idx = p_y * TABLE_WIDTH + p_x;
			if (data.size() >= (idx + 1) * int(sizeof(uint16_t))) {
				const uint16_t *src = reinterpret_cast<const uint16_t *>(data.ptr());
				return Math::half_to_float(src[idx]);
			}
		}
		return profile_image->get_pixel(p_x, p_y).r;
	};

	const float c00 = read_texel(x0, y0);
	const float c10 = read_texel(x1, y0);
	const float c01 = read_texel(x0, y1);
	const float c11 = read_texel(x1, y1);
	const float c0 = Math::lerp(c00, c10, tx);
	const float c1 = Math::lerp(c01, c11, tx);
	return Math::lerp(c0, c1, ty);
}

float IESProfile::sample(const Vector3 &p_direction_local) const {
	const float len = float(p_direction_local.length());
	if (len <= 1e-8f) {
		return sample_angles(0.0f, 0.0f);
	}
	const Vector3 d = p_direction_local / len;
	const float cos_theta = CLAMP(float(-d.z), -1.0f, 1.0f);
	const float theta_deg = float(Math::rad_to_deg(double(std::acos(cos_theta))));
	float phi_deg = float(Math::rad_to_deg(double(std::atan2(float(d.y), float(d.x)))));
	if (phi_deg < 0.0f) {
		phi_deg += 360.0f;
	}
	return sample_angles(theta_deg, phi_deg);
}

RID IESProfile::get_rid() const {
	if (!ies_rid.is_valid() && RenderingServer::get_singleton() != nullptr) {
		ies_rid = RenderingServer::get_singleton()->ies_profile_create();
		if (ies_rid.is_valid()) {
			_sync_to_server();
		}
	}
	return ies_rid;
}

void IESProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_profile_image", "image"), &IESProfile::set_profile_image);
	ClassDB::bind_method(D_METHOD("get_profile_image"), &IESProfile::get_profile_image);

	ClassDB::bind_method(D_METHOD("set_candela_table", "table"), &IESProfile::set_candela_table);
	ClassDB::bind_method(D_METHOD("get_candela_table"), &IESProfile::get_candela_table);

	ClassDB::bind_method(D_METHOD("set_peak_candela", "peak_candela"), &IESProfile::set_peak_candela);
	ClassDB::bind_method(D_METHOD("get_peak_candela"), &IESProfile::get_peak_candela);

	ClassDB::bind_method(D_METHOD("set_lumens", "lumens"), &IESProfile::set_lumens);
	ClassDB::bind_method(D_METHOD("get_lumens"), &IESProfile::get_lumens);

	ClassDB::bind_method(D_METHOD("set_watts", "watts"), &IESProfile::set_watts);
	ClassDB::bind_method(D_METHOD("get_watts"), &IESProfile::get_watts);

	ClassDB::bind_method(D_METHOD("set_photometric_type", "type"), &IESProfile::set_photometric_type);
	ClassDB::bind_method(D_METHOD("get_photometric_type"), &IESProfile::get_photometric_type);

	ClassDB::bind_method(D_METHOD("set_vertical_angles", "angles"), &IESProfile::set_vertical_angles);
	ClassDB::bind_method(D_METHOD("get_vertical_angles"), &IESProfile::get_vertical_angles);

	ClassDB::bind_method(D_METHOD("set_horizontal_angles", "angles"), &IESProfile::set_horizontal_angles);
	ClassDB::bind_method(D_METHOD("get_horizontal_angles"), &IESProfile::get_horizontal_angles);

	ClassDB::bind_method(D_METHOD("load_from_ies_string", "text"), &IESProfile::load_from_ies_string);
	ClassDB::bind_method(D_METHOD("load_from_ies_file", "path"), &IESProfile::load_from_ies_file);
	ClassDB::bind_method(D_METHOD("sample", "direction_local"), &IESProfile::sample);
	ClassDB::bind_method(D_METHOD("sample_angles", "vertical_deg", "horizontal_deg"), &IESProfile::sample_angles);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "profile_image", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_profile_image", "get_profile_image");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "peak_candela", PROPERTY_HINT_RANGE, "0,1000000,0.01,or_greater,suffix:cd"), "set_peak_candela", "get_peak_candela");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lumens", PROPERTY_HINT_RANGE, "0,1000000,0.01,or_greater,suffix:lm"), "set_lumens", "get_lumens");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "watts", PROPERTY_HINT_RANGE, "0,100000,0.01,or_greater,suffix:W"), "set_watts", "get_watts");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "photometric_type", PROPERTY_HINT_ENUM, "Type C:1,Type B:2,Type A:3"), "set_photometric_type", "get_photometric_type");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "vertical_angles"), "set_vertical_angles", "get_vertical_angles");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "horizontal_angles"), "set_horizontal_angles", "get_horizontal_angles");

	BIND_ENUM_CONSTANT(PHOTOMETRIC_TYPE_C);
	BIND_ENUM_CONSTANT(PHOTOMETRIC_TYPE_B);
	BIND_ENUM_CONSTANT(PHOTOMETRIC_TYPE_A);
}

IESProfile::IESProfile() {
}

IESProfile::~IESProfile() {
	if (ies_rid.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free_rid(ies_rid);
	}
}
