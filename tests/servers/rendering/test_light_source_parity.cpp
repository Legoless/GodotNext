/**************************************************************************/
/*  test_light_source_parity.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_light_source_parity)

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "scene/3d/light_3d.h"
#include "scene/resources/ies_profile.h"
#include "servers/rendering/renderer_rd/light_grid_math.h"
#include "servers/rendering/renderer_rd/light_source_parity.h"
#ifdef TOOLS_ENABLED
#include "editor/import/resource_importer_ies.h"
#endif

#include <cmath>

namespace {

namespace LSP = RendererRD::LightSourceParity;
namespace LG = RendererRD::LightGrid;

String build_synthetic_ies(
		const String &p_header,
		const String &p_tilt,
		int p_photometric_type,
		float p_multiplier,
		float p_ballast,
		float p_field12,
		const Vector<float> &p_v_angles,
		const Vector<float> &p_h_angles,
		const Vector<float> &p_candela) {
	String s;
	s += p_header + "\n";
	s += "[TEST] Synthetic GodotNext clean-room profile\n";
	s += p_tilt + "\n";
	s += vformat("1 1000.0 %.6f %d %d %d 2 0.1 0.1 0.0\n", p_multiplier, p_v_angles.size(), p_h_angles.size(), p_photometric_type);
	s += vformat("%.6f %.6f 10.0\n", p_ballast, p_field12);
	for (int i = 0; i < p_v_angles.size(); i++) {
		if (i > 0) {
			s += " ";
		}
		s += vformat("%.4f", p_v_angles[i]);
	}
	s += "\n";
	for (int i = 0; i < p_h_angles.size(); i++) {
		if (i > 0) {
			s += " ";
		}
		s += vformat("%.4f", p_h_angles[i]);
	}
	s += "\n";
	for (int i = 0; i < p_candela.size(); i++) {
		if (i > 0) {
			s += (i % 10 == 0) ? "\n" : " ";
		}
		s += vformat("%.6f", p_candela[i]);
	}
	s += "\n";
	return s;
}

TEST_CASE("[LightSourceParity] LM-63-1995 and LM-63-2002 parsing and TILT=INCLUDE rejection") {
	Vector<float> v_angles;
	for (int i = 0; i <= 36; i++) {
		v_angles.push_back(float(i) * 5.0f);
	}
	Vector<float> h_angles;
	h_angles.push_back(0.0f);

	Vector<float> candela;
	for (int i = 0; i < v_angles.size(); i++) {
		const float rad = Math::deg_to_rad(v_angles[i]);
		const float c = Math::cos(rad);
		candela.push_back(c > 0.0f ? 250.0f * c * c : 0.0f);
	}

	// LM-63-1995 applies ballast_factor * ballast_lamp_photometric_factor * candela_multiplier.
	const String ies_1995 = build_synthetic_ies("IESNA:LM-63-1995", "TILT=NONE", 1, 2.0f, 0.9f, 0.8f, v_angles, h_angles, candela);
	Ref<IESProfile> p95;
	p95.instantiate();
	CHECK(p95->load_from_ies_string(ies_1995) == OK);
	CHECK(p95->get_profile_image().is_valid());
	CHECK(p95->get_photometric_type() == IESProfile::PHOTOMETRIC_TYPE_C);
	CHECK(p95->get_candela_table().size() == int(LSP::IES_TABLE_WIDTH * LSP::IES_TABLE_HEIGHT));
	CHECK(p95->get_peak_candela() == doctest::Approx(250.0f * 2.0f * 0.9f * 0.8f).epsilon(1e-3));

	Ref<Image> img = p95->get_profile_image();
	REQUIRE(img.is_valid());
	CHECK(img->get_width() == int(LSP::IES_TABLE_WIDTH));
	CHECK(img->get_height() == int(LSP::IES_TABLE_HEIGHT));
	CHECK(img->get_format() == Image::FORMAT_RH);

	// LM-63-2002 treats field 12 as reserved "future use" (may be 0.0 as emitted by ies_gen.py).
	const String ies_2002_zero_f12 = build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.5f, 1.0f, 0.0f, v_angles, h_angles, candela);
	Ref<IESProfile> p02;
	p02.instantiate();
	CHECK(p02->load_from_ies_string(ies_2002_zero_f12) == OK);
	CHECK(p02->get_profile_image().is_valid());
	CHECK(p02->get_peak_candela() == doctest::Approx(250.0f * 1.5f).epsilon(1e-3));

	// TILT=INCLUDE and external tilt files MUST be rejected cleanly.
	const String ies_tilt_include = build_synthetic_ies("IESNA:LM-63-2002", "TILT=INCLUDE", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, candela);
	const String ies_tilt_file = build_synthetic_ies("IESNA:LM-63-1995", "TILT=external.dat", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, candela);
	Ref<IESProfile> bad;
	bad.instantiate();
	ERR_PRINT_OFF;
	CHECK(bad->load_from_ies_string(ies_tilt_include) != OK);
	CHECK(bad->get_profile_image().is_null());
	CHECK(bad->load_from_ies_string(ies_tilt_file) != OK);
	CHECK(bad->load_from_ies_string("NOT_AN_IES_FILE\n") != OK);
	ERR_PRINT_ON;
}

TEST_CASE("[LightSourceParity] IES symmetry resampling accuracy against analytic candela functions") {
	Vector<float> v_angles;
	for (int i = 0; i <= 72; i++) {
		v_angles.push_back(float(i) * 2.5f);
	}

	// 1) Axially symmetric (0 deg) batwing profile: I(theta) = sin^2(theta) * cos^2(theta) for theta <= 90 deg.
	{
		Vector<float> h_angles;
		h_angles.push_back(0.0f);
		Vector<float> cd;
		auto analytic = [](float p_theta) -> float {
			if (p_theta >= float(Math::PI * 0.5)) {
				return 0.0f;
			}
			const float s = std::sin(p_theta);
			const float c = std::cos(p_theta);
			return 4.0f * s * s * c * c; // Peak = 1.0 at 45 deg.
		};
		for (int i = 0; i < v_angles.size(); i++) {
			cd.push_back(analytic(Math::deg_to_rad(v_angles[i])) * 400.0f);
		}
		Ref<IESProfile> prof;
		prof.instantiate();
		REQUIRE(prof->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, cd)) == OK);

		double err_sum = 0.0;
		double max_err = 0.0;
		int count = 0;
		for (int ti = 0; ti <= 18; ti++) {
			const float theta = Math::deg_to_rad(float(ti) * 5.0f);
			for (int pi = 0; pi < 8; pi++) {
				const float phi = Math::deg_to_rad(float(pi) * 45.0f);
				const Vector3 dir(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), -std::cos(theta));
				const float sampled = prof->sample(dir);
				const float expected = analytic(theta);
				const double err = std::fabs(double(sampled - expected));
				err_sum += err;
				max_err = MAX(max_err, err);
				count++;
			}
		}
		CHECK(err_sum / double(count) < 0.005);
		CHECK(max_err < 0.02);
	}

	// 2) Quadrant symmetric (0..90 deg) profile: I(theta, phi) = cos(theta) * (0.5 + 0.5 * cos(2*phi)).
	{
		Vector<float> h_angles;
		for (int i = 0; i <= 18; i++) {
			h_angles.push_back(float(i) * 5.0f);
		}
		auto analytic = [](float p_theta, float p_phi) -> float {
			const float c = std::cos(p_theta);
			if (c <= 0.0f) {
				return 0.0f;
			}
			return c * (0.5f + 0.5f * std::cos(2.0f * p_phi));
		};
		Vector<float> cd;
		for (int hi = 0; hi < h_angles.size(); hi++) {
			const float phi = Math::deg_to_rad(h_angles[hi]);
			for (int vi = 0; vi < v_angles.size(); vi++) {
				cd.push_back(analytic(Math::deg_to_rad(v_angles[vi]), phi) * 100.0f);
			}
		}
		Ref<IESProfile> prof;
		prof.instantiate();
		REQUIRE(prof->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, cd)) == OK);

		double max_err = 0.0;
		for (int pi = 0; pi < 16; pi++) {
			const float phi_deg = float(pi) * 22.5f;
			const float theta_deg = 30.0f;
			const float sampled = prof->sample_angles(theta_deg, phi_deg);
			const float expected = analytic(Math::deg_to_rad(theta_deg), Math::deg_to_rad(phi_deg));
			max_err = MAX(max_err, std::fabs(double(sampled - expected)));
		}
		CHECK(max_err < 0.02);
	}

	// 3) Bilateral symmetric (0..180 deg): I(theta, phi) = cos(theta) * (0.6 + 0.4 * cos(phi)).
	{
		Vector<float> h_angles;
		for (int i = 0; i <= 36; i++) {
			h_angles.push_back(float(i) * 5.0f);
		}
		auto analytic = [](float p_theta, float p_phi) -> float {
			const float c = std::cos(p_theta);
			return c > 0.0f ? c * (0.6f + 0.4f * std::cos(p_phi)) : 0.0f;
		};
		Vector<float> cd;
		for (int hi = 0; hi < h_angles.size(); hi++) {
			const float phi = Math::deg_to_rad(h_angles[hi]);
			for (int vi = 0; vi < v_angles.size(); vi++) {
				cd.push_back(analytic(Math::deg_to_rad(v_angles[vi]), phi) * 100.0f);
			}
		}
		Ref<IESProfile> prof;
		prof.instantiate();
		REQUIRE(prof->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, cd)) == OK);

		double max_err = 0.0;
		for (int pi = 0; pi < 16; pi++) {
			const float phi_deg = float(pi) * 22.5f;
			const float theta_deg = 35.0f;
			max_err = MAX(max_err, std::fabs(double(prof->sample_angles(theta_deg, phi_deg) - analytic(Math::deg_to_rad(theta_deg), Math::deg_to_rad(phi_deg)))));
		}
		CHECK(max_err < 0.02);
	}

	// 4) Full asymmetric (0..360 deg): I(theta, phi) = cos(theta) * (0.5 + 0.3 * cos(phi) + 0.2 * sin(phi)).
	{
		Vector<float> h_angles;
		for (int i = 0; i <= 36; i++) {
			h_angles.push_back(float(i) * 10.0f);
		}
		auto analytic = [](float p_theta, float p_phi) -> float {
			const float c = std::cos(p_theta);
			if (c <= 0.0f) {
				return 0.0f;
			}
			// Peak over phi is 0.5 + sqrt(0.3^2 + 0.2^2) = 0.5 + sqrt(0.13) at theta=0 -> normalize by max.
			return c * (0.5f + 0.3f * std::cos(p_phi) + 0.2f * std::sin(p_phi));
		};
		float peak = 0.0f;
		Vector<float> cd;
		for (int hi = 0; hi < h_angles.size(); hi++) {
			const float phi = Math::deg_to_rad(h_angles[hi]);
			for (int vi = 0; vi < v_angles.size(); vi++) {
				const float val = analytic(Math::deg_to_rad(v_angles[vi]), phi) * 100.0f;
				peak = MAX(peak, val);
				cd.push_back(val);
			}
		}
		Ref<IESProfile> prof;
		prof.instantiate();
		REQUIRE(prof->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.0f, 1.0f, 1.0f, v_angles, h_angles, cd)) == OK);

		// Asymmetric check: phi = +90 deg (+Y) must be strictly brighter than phi = 270 deg (-Y).
		const float pos_y = prof->sample(Vector3(0.0f, 0.5f, -std::sqrt(0.75f)));
		const float neg_y = prof->sample(Vector3(0.0f, -0.5f, -std::sqrt(0.75f)));
		CHECK(pos_y > neg_y + 0.25f);
		CHECK(pos_y == doctest::Approx(analytic(Math::deg_to_rad(30.0f), Math::deg_to_rad(90.0f)) * 100.0f / peak).epsilon(0.02));
		CHECK(neg_y == doctest::Approx(analytic(Math::deg_to_rad(30.0f), Math::deg_to_rad(270.0f)) * 100.0f / peak).epsilon(0.02));
	}

	// 5) Photometric Type B & Type A directional mapping.
	{
		Vector<float> vb;
		Vector<float> hb;
		for (int i = -9; i <= 9; i++) {
			vb.push_back(float(i) * 10.0f);
			hb.push_back(float(i) * 10.0f);
		}
		Vector<float> cd;
		for (int hi = 0; hi < hb.size(); hi++) {
			for (int vi = 0; vi < vb.size(); vi++) {
				// Peak at (h=0, v=0), drops with angle.
				const float r2 = (vb[vi] * vb[vi] + hb[hi] * hb[hi]) / (90.0f * 90.0f);
				cd.push_back(MAX(0.0f, 1.0f - r2) * 500.0f);
			}
		}
		Ref<IESProfile> prof_b;
		prof_b.instantiate();
		REQUIRE(prof_b->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 2, 1.0f, 1.0f, 1.0f, vb, hb, cd)) == OK);
		CHECK(prof_b->get_photometric_type() == IESProfile::PHOTOMETRIC_TYPE_B);
		CHECK(prof_b->sample(Vector3(0.0f, 0.0f, -1.0f)) > 0.95f);
		CHECK(prof_b->sample(Vector3(0.0f, 0.0f, 1.0f)) < 0.05f);

		Ref<IESProfile> prof_a;
		prof_a.instantiate();
		REQUIRE(prof_a->load_from_ies_string(build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 3, 1.0f, 1.0f, 1.0f, vb, hb, cd)) == OK);
		CHECK(prof_a->get_photometric_type() == IESProfile::PHOTOMETRIC_TYPE_A);
		CHECK(prof_a->sample(Vector3(0.0f, 0.0f, -1.0f)) > 0.95f);
	}
}

#ifdef TOOLS_ENABLED
TEST_CASE("[LightSourceParity] ResourceImporterIES round-trip save and load") {
	Vector<float> v_angles = { 0.0f, 30.0f, 60.0f, 90.0f, 180.0f };
	Vector<float> h_angles = { 0.0f };
	Vector<float> cd = { 1000.0f, 800.0f, 300.0f, 0.0f, 0.0f };
	const String ies_text = build_synthetic_ies("IESNA:LM-63-2002", "TILT=NONE", 1, 1.0f, 1.0f, 0.0f, v_angles, h_angles, cd);

	const String base_dir = OS::get_singleton()->get_cache_path().path_join("godotnext_ies_test");
	DirAccess::make_dir_recursive_absolute(base_dir);
	const String src_path = base_dir.path_join("test_profile.ies");
	const String save_base = base_dir.path_join("test_profile_out");
	{
		Ref<FileAccess> f = FileAccess::open(src_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string(ies_text);
	}

	Ref<ResourceImporterIES> importer;
	importer.instantiate();
	HashMap<StringName, Variant> options;
	List<String> platform_variants;
	List<String> gen_files;
	Variant metadata;
	CHECK(importer->import(ResourceUID::INVALID_ID, src_path, save_base, options, &platform_variants, &gen_files, &metadata) == OK);

	Ref<IESProfile> loaded = ResourceLoader::load(save_base + ".res");
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_profile_image().is_valid());
	CHECK(loaded->get_peak_candela() == doctest::Approx(1000.0f).epsilon(1e-3));
	CHECK(loaded->sample(Vector3(0.0f, 0.0f, -1.0f)) == doctest::Approx(1.0f).epsilon(0.02));
	CHECK(loaded->sample(Vector3(0.0f, 0.0f, 1.0f)) == doctest::Approx(0.0f).epsilon(0.02));

	DirAccess::remove_absolute(src_path);
	DirAccess::remove_absolute(save_base + ".res");
}
#endif

TEST_CASE("[LightSourceParity] IES atlas layer allocation, ref-counting, recycling, and capacity cap") {
	LSP::IESAtlasAllocator alloc(4);
	bool newly = false;

	const uint32_t l0 = alloc.acquire(101, &newly);
	CHECK(l0 == 0u);
	CHECK(newly);
	CHECK(alloc.get_active_layer_count() == 1u);

	// Sharing same key increments refcount without consuming a second layer.
	const uint32_t l0_again = alloc.acquire(101, &newly);
	CHECK(l0_again == 0u);
	CHECK(!newly);
	CHECK(alloc.get_ref_count(101) == 2u);
	CHECK(alloc.get_active_layer_count() == 1u);

	CHECK(alloc.acquire(102, &newly) == 1u);
	CHECK(newly);
	CHECK(alloc.acquire(103, &newly) == 2u);
	CHECK(alloc.acquire(104, &newly) == 3u);
	CHECK(alloc.get_active_layer_count() == 4u);

	// Atlas full -> returns IES_LAYER_NONE.
	CHECK(alloc.acquire(105, &newly) == LSP::IES_LAYER_NONE);
	CHECK(!newly);

	// Releasing 1 of 2 refs on key 101 keeps layer 0 occupied.
	CHECK(!alloc.release(101));
	CHECK(alloc.get_layer(101) == 0u);
	CHECK(alloc.acquire(105, &newly) == LSP::IES_LAYER_NONE);

	// Releasing the final ref on key 101 recycles layer 0 for key 105.
	CHECK(alloc.release(101));
	CHECK(alloc.get_layer(101) == LSP::IES_LAYER_NONE);
	CHECK(alloc.get_active_layer_count() == 3u);
	CHECK(alloc.acquire(105, &newly) == 0u);
	CHECK(newly);
	CHECK(alloc.get_active_layer_count() == 4u);

	// Evict removes regardless of refcount.
	alloc.acquire(102);
	CHECK(alloc.get_ref_count(102) == 2u);
	CHECK(alloc.evict(102));
	CHECK(alloc.get_layer(102) == LSP::IES_LAYER_NONE);
	CHECK(alloc.get_active_layer_count() == 3u);
	CHECK(alloc.acquire(106, &newly) == 1u);
	CHECK(newly);

	// Full 256-layer capacity check.
	LSP::IESAtlasAllocator full_alloc;
	for (uint32_t i = 0; i < LSP::IES_MAX_ATLAS_LAYERS; i++) {
		CHECK(full_alloc.acquire(uint64_t(i + 1)) == i);
	}
	CHECK(full_alloc.acquire(9999) == LSP::IES_LAYER_NONE);
	CHECK(full_alloc.release(42));
	CHECK(full_alloc.acquire(9999) == 41u);
}

TEST_CASE("[LightSourceParity] AreaLight3D barn-door rect clipping matches #139 culling and texture sub-rect") {
	const Vector2 half_size(1.0f, 0.5f);
	Vector2 cmin, cmax;

	// Open barn doors (90 deg) leave the full emitter unclipped.
	CHECK(LSP::barn_door_clip_rect(Vector3(3.0f, -2.0f, -1.5f), half_size, float(Math::deg_to_rad(90.0)), 0.5f, cmin, cmax));
	CHECK(cmin.is_equal_approx(-half_size));
	CHECK(cmax.is_equal_approx(half_size));

	// Behind the emitter is always occluded.
	CHECK(!LSP::barn_door_clip_rect(Vector3(0.0f, 0.0f, 0.1f), half_size, float(Math::deg_to_rad(30.0)), 0.5f, cmin, cmax));

	// Narrow barn doors (25 deg, length 0.5m): verify exact match with #139's barn_door_max_extent_at_depth().
	const float angle_deg = 25.0f;
	const float angle_rad = float(Math::deg_to_rad(double(angle_deg)));
	const float length = 0.5f;
	const float f_far = 3.0f;
	const float max_x = LG::barn_door_max_extent_at_depth(half_size.x, 100.0f, angle_rad, length, f_far);
	const float max_y = LG::barn_door_max_extent_at_depth(half_size.y, 100.0f, angle_rad, length, f_far);

	// Center point sees the full emitter.
	CHECK(LSP::barn_door_clip_rect(Vector3(0.0f, 0.0f, -f_far), half_size, angle_rad, length, cmin, cmax));
	CHECK(cmin.is_equal_approx(-half_size));
	CHECK(cmax.is_equal_approx(half_size));

	// Just inside the +X barn-door frustum boundary -> right door clips the +X edge (cmax.x < half_size.x).
	CHECK(LSP::barn_door_clip_rect(Vector3(max_x * 0.85f, 0.0f, -f_far), half_size, angle_rad, length, cmin, cmax));
	CHECK(cmin.x >= -half_size.x - 1e-5f);
	CHECK(cmax.x < half_size.x);
	CHECK(cmax.x > cmin.x);

	// Texture rect adjusts cleanly for the clipped sub-rect.
	const Rect2 full_tex(0.1f, 0.2f, 0.4f, 0.5f);
	const Rect2 clip_tex = LSP::barn_door_clip_texture_rect(full_tex, half_size, cmin, cmax);
	CHECK(clip_tex.size.x < full_tex.size.x);
	CHECK(clip_tex.size.x > 0.0f);
	CHECK(clip_tex.size.y == doctest::Approx(full_tex.size.y).epsilon(1e-5));
	CHECK(clip_tex.position.x >= full_tex.position.x - 1e-5f);
	CHECK(clip_tex.position.x + clip_tex.size.x <= full_tex.position.x + full_tex.size.x + 1e-5f);

	// Strictly outside the barn-door frustum boundary -> fully occluded.
	CHECK(!LSP::barn_door_clip_rect(Vector3(max_x + 0.05f, 0.0f, -f_far), half_size, angle_rad, length, cmin, cmax));
	CHECK(!LSP::barn_door_clip_rect(Vector3(0.0f, max_y + 0.05f, -f_far), half_size, angle_rad, length, cmin, cmax));

	// Continuity across the door-tip plane f = z_d = L * cos(angle).
	const float z_d = length * std::cos(angle_rad);
	const float tip_x = half_size.x + length * std::sin(angle_rad);
	CHECK(LSP::barn_door_clip_rect(Vector3(tip_x - 0.01f, 0.0f, -(z_d - 1e-3f)), half_size, angle_rad, length, cmin, cmax));
	CHECK(LSP::barn_door_clip_rect(Vector3(tip_x - 0.01f, 0.0f, -(z_d + 1e-3f)), half_size, angle_rad, length, cmin, cmax));
	CHECK(!LSP::barn_door_clip_rect(Vector3(tip_x + 0.05f, 0.0f, -(z_d - 1e-3f)), half_size, angle_rad, length, cmin, cmax));
	CHECK(!LSP::barn_door_clip_rect(Vector3(tip_x + 0.05f, 0.0f, -(z_d + 1e-3f)), half_size, angle_rad, length, cmin, cmax));
}

TEST_CASE("[LightSourceParity] Contact shadow resolution, depth scaling, and Light3D defaults") {
	// Light3D defaults (all subtypes).
	{
		DirectionalLight3D dir;
		OmniLight3D omni;
		SpotLight3D spot;
		AreaLight3D area;
		Light3D *lights[4] = { &dir, &omni, &spot, &area };
		for (Light3D *l : lights) {
			CHECK(l->get_param(Light3D::PARAM_CONTACT_SHADOW_LENGTH) == doctest::Approx(0.0));
			CHECK(!l->get_contact_shadow_length_world_space());
			CHECK(l->get_param(Light3D::PARAM_CONTACT_SHADOW_CASTING_INTENSITY) == doctest::Approx(1.0));
			CHECK(l->get_param(Light3D::PARAM_CONTACT_SHADOW_NON_CASTING_INTENSITY) == doctest::Approx(0.0));
			CHECK(l->get_ies_profile().is_null());
			CHECK(!l->get_ies_use_intensity());
			CHECK(l->get_ies_intensity_scale() == doctest::Approx(1.0f));
		}
		CHECK(area.get_param(Light3D::PARAM_BARN_DOOR_ANGLE) == doctest::Approx(90.0));
		CHECK(area.get_param(Light3D::PARAM_BARN_DOOR_LENGTH) == doctest::Approx(0.2));
	}

	// Length 0.0 is always off, even when project contact shadows are enabled.
	{
		const LSP::ResolvedContactShadow r = LSP::resolve_contact_shadow(0.0f, false, true, 1.0f, 0.0f, true, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM);
		CHECK(!r.active);
	}

	// Length -1.0 inherits project setting.
	{
		const LSP::ResolvedContactShadow r_off = LSP::resolve_contact_shadow(-1.0f, true, true, 1.0f, 0.0f, false, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG);
		CHECK(!r_off.active);

		const LSP::ResolvedContactShadow r_on = LSP::resolve_contact_shadow(-1.0f, true, true, 1.0f, 0.0f, true, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG);
		CHECK(r_on.active);
		CHECK(!r_on.world_space);
		CHECK(r_on.length == doctest::Approx(LSP::CONTACT_SHADOW_PROJECT_LENGTH_LONG));
	}

	// Positive per-light length overrides even when project setting is disabled.
	{
		const LSP::ResolvedContactShadow r = LSP::resolve_contact_shadow(0.25f, true, true, 0.0f, 0.75f, false, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_SHORT);
		CHECK(r.active);
		CHECK(r.world_space);
		CHECK(r.length == doctest::Approx(0.25f));
	}

	// Zero intensities or allow == false disable contact shadows.
	CHECK(!LSP::resolve_contact_shadow(0.1f, false, false, 1.0f, 1.0f, true, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM).active);
	CHECK(!LSP::resolve_contact_shadow(0.1f, false, true, 0.0f, 0.0f, true, RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM).active);

	// Screen-space vs world-space pixel length conversion.
	CHECK(LSP::contact_shadow_length_to_pixels(0.1f, false, 1000.0f, 1.0f, 2.0f, false) == doctest::Approx(100.0f));
	CHECK(LSP::contact_shadow_length_to_pixels(0.1f, false, 1000.0f, 1.0f, 20.0f, false) == doctest::Approx(100.0f));
	const float ws_near = LSP::contact_shadow_length_to_pixels(0.2f, true, 1000.0f, 1.0f, 2.0f, false);
	const float ws_far = LSP::contact_shadow_length_to_pixels(0.2f, true, 1000.0f, 1.0f, 10.0f, false);
	CHECK(ws_near == doctest::Approx(50.0f));
	CHECK(ws_far == doctest::Approx(10.0f));
	CHECK(LSP::contact_shadow_length_to_pixels(0.2f, true, 1000.0f, 1.0f, 10.0f, true) == doctest::Approx(100.0f));

	// Quality tier selection.
	CHECK(LSP::select_contact_shadow_quality(0.05f, false, 1000, 1) == RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_SHORT);
	CHECK(LSP::select_contact_shadow_quality(0.10f, false, 1000, 1) == RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM);
	CHECK(LSP::select_contact_shadow_quality(0.18f, false, 1000, 1) == RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG);
}

} // namespace
