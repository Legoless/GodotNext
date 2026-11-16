/**************************************************************************/
/*  rr_guides.h                                                           */
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

#include "core/math/basis.h"
#include "core/math/color.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/effects/rr_guide_pack.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// GodotNext: rr_guides (#205). The guides DLSS Ray Reconstruction reads (plan D21, §8.6):
// - diffuse albedo, albedo * (1 - metallic), linear RGBA8, 0.5 where no surface is;
// - specular albedo, the factor the forward shader scales the indirect specular light
//   by, linear RGBA8, 0 where no surface is;
// - the world-space unit normal with the roughness, RGBA16F, 0 where no surface is;
// - the specular hit distance the NextGI specular trace writes (R16F, #60), which stays
//   in the GI buffers (RB_SCOPE_GI, RB_TEX_SPECULAR_HIT).
// The opaque color passes write the two albedo guides as extra color attachments and
// pack() writes the third. Transparent surfaces write none: where one is in front (a
// depth-prepass-alpha surface writes the prepass depth and normal but is drawn in the
// transparent pass), all three guides say "no surface". Nothing here is compiled or
// allocated unless a view writes the guides.
class RRGuides {
	struct PushConstant {
		float view_to_world[12];
		int32_t size[2];
		uint32_t pad[2];
	};

	RrGuidePackShaderRD shader;
	RID shader_version;
	RID pipeline;

public:
	// Where the two albedo guides sit in the color pass, after color, separate specular
	// and velocity.
	static constexpr int ATTACHMENT_DIFFUSE_ALBEDO = 3;
	static constexpr int ATTACHMENT_SPECULAR_ALBEDO = 4;

	// True while the guides are wanted: while the DLSS-RR mode asks for them (set_requested(),
	// #206) or while the DEV switch is on.
	static bool is_requested();
	static void set_requested(bool p_requested);
	// GODOT_DEV_RR_GUIDES=1 in the environment, read once, in builds with DEBUG_ENABLED. It
	// forces the guides on without DLSS.
	static bool is_dev_switch_on();

	// The per-view gate. Reflection probes, multiview and MSAA views write no guides.
	static bool view_writes_guides(bool p_requested, bool p_reflection_probe, uint32_t p_view_count, bool p_msaa);

	// The only place the RR shader groups are enabled. The renderer passes
	// SceneShaderForwardClustered and is_requested(); the doctest passes a recorder.
	template <typename T>
	static bool enable_view_shader_groups(T &r_shader, bool p_requested, bool p_reflection_probe, uint32_t p_view_count, bool p_msaa, bool p_nextgeo) {
		if (!view_writes_guides(p_requested, p_reflection_probe, p_view_count, p_msaa)) {
			return false;
		}
		r_shader.enable_rr_guides_shader_group();
		if (p_nextgeo) {
			r_shader.enable_nextgeo_rr_guides_shader_group();
		}
		return true;
	}

	// What the two albedo guides hold where no opaque surface is. The opaque passes write
	// alpha 1, so alpha 0 marks a pixel no opaque surface covers.
	static Color diffuse_albedo_clear_color() { return Color(0.5, 0.5, 0.5, 0.0); }
	static Color specular_albedo_clear_color() { return Color(0.0, 0.0, 0.0, 0.0); }

	// The first opaque color pass of a frame to open the guides (NextGeo Stage B, else the
	// opaque pass) clears them with its render pass (#275); a later one loads them. The clear
	// covers the draw list's render region, as the color attachment's does: with an XR render
	// region the texels outside it keep older contents.
	// draw_list_begin() numbers its clear flags and clear colors by the color attachments that
	// exist, so the guides' index follows whichever of separate specular and velocity the
	// framebuffer has.
	static uint32_t first_guide_color_index(bool p_has_specular, bool p_has_velocity) {
		return 1 + (p_has_specular ? 1 : 0) + (p_has_velocity ? 1 : 0);
	}
	// Makes a color pass clear the guides: sets their clear flags and puts their clear colors at
	// their index in r_clear_colors, which grows to reach it. The colors before them stay; the
	// pass reads only those its flags clear. Fails with an error and changes nothing when a flag
	// clears a color attachment before the guides that has no clear color.
	static void clear_guides(BitField<RD::DrawFlags> &r_flags, Vector<Color> &r_clear_colors, bool p_has_specular, bool p_has_velocity);

	// The prepass stores roughness * ROUGHNESS_STORED_MAX / 255 in an RGBA8 channel, and one
	// minus that (above DYNAMIC_FLAG_THRESHOLD) for dynamic instances.
	static constexpr int ROUGHNESS_STORED_MAX = 127;
	static constexpr float DYNAMIC_FLAG_THRESHOLD = 0.5f;

	// The decode rr_guide_pack.glsl runs, given to it as defines: the shader calls
	// RR_DECODE_NORMAL() and RR_DECODE_ROUGHNESS() and holds none of the decode's numbers.
	// unpack_world_normal() and unpack_roughness() below are the same math on the CPU, and
	// the doctests check them and this text. The GPU result is checked by the testbed check
	// tests/rr_guides.
	static String glsl_pack_prelude();
	static Vector3 unpack_world_normal(const Basis &p_view_to_world, const Vector3 &p_encoded);
	static float unpack_roughness(float p_stored);

	// Writes the normal and roughness guide. Where the opaque passes drew no surface (the
	// diffuse albedo guide's alpha is 0), it writes 0, so the three guides agree.
	void pack(RID p_source_normal_roughness, RID p_diffuse_albedo, RID p_dest_normal_roughness, const Size2i &p_size, const Basis &p_view_to_world);

	RRGuides();
	~RRGuides();
};

} // namespace RendererRD
