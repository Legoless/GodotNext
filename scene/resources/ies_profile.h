/**************************************************************************/
/*  ies_profile.h                                                         */
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

// GodotNext: light_source_parity (#140, plan §21.3 contract M8, GM-11).
// Photometric IES profile resource storing a normalized 256 x 64 R16F candela table,
// peak candela intensity, LM-63 angle metadata, and a CPU sampling helper.

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/math/vector3.h"
#include "core/variant/variant.h"

class IESProfile : public Resource {
	GDCLASS(IESProfile, Resource);

public:
	static constexpr int TABLE_WIDTH = 256;
	static constexpr int TABLE_HEIGHT = 64;

	enum PhotometricType {
		PHOTOMETRIC_TYPE_C = 1,
		PHOTOMETRIC_TYPE_B = 2,
		PHOTOMETRIC_TYPE_A = 3,
	};

private:
	Ref<Image> profile_image;
	float peak_candela = 1.0f;
	float lumens = 0.0f;
	float watts = 0.0f;
	PhotometricType photometric_type = PHOTOMETRIC_TYPE_C;
	PackedFloat32Array vertical_angles;
	PackedFloat32Array horizontal_angles;

	mutable RID ies_rid;

	void _sync_to_server() const;
	static Ref<Image> _build_r16f_image(const PackedFloat32Array &p_table);

protected:
	static void _bind_methods();

public:
	void set_profile_image(const Ref<Image> &p_image);
	Ref<Image> get_profile_image() const;

	void set_candela_table(const PackedFloat32Array &p_table);
	PackedFloat32Array get_candela_table() const;

	void set_peak_candela(float p_peak);
	float get_peak_candela() const;

	void set_lumens(float p_lumens);
	float get_lumens() const;

	void set_watts(float p_watts);
	float get_watts() const;

	void set_photometric_type(PhotometricType p_type);
	PhotometricType get_photometric_type() const;

	void set_vertical_angles(const PackedFloat32Array &p_angles);
	PackedFloat32Array get_vertical_angles() const;

	void set_horizontal_angles(const PackedFloat32Array &p_angles);
	PackedFloat32Array get_horizontal_angles() const;

	Error load_from_ies_string(const String &p_text);
	Error load_from_ies_file(const String &p_path);

	// CPU bilinear sampling matching the GPU IES atlas lookup (forward emission along -Z,
	// right +X, up +Y in light local space). Returns normalized intensity in [0, 1].
	float sample(const Vector3 &p_direction_local) const;
	float sample_angles(float p_vertical_deg, float p_horizontal_deg) const;

	virtual RID get_rid() const override;

	IESProfile();
	~IESProfile();
};

VARIANT_ENUM_CAST(IESProfile::PhotometricType);
