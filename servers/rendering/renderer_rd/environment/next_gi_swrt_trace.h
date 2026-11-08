/**************************************************************************/
/*  next_gi_swrt_trace.h                                                  */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"
#include "servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h"

class RenderDataRD;

// GodotNext: ps_swrt_trace (#186, plan §21.2 L5)
//
// Software trace for gather, reflections and SDF shadow rays. The order is
// screen, then detail mesh SDFs for the first 2 m when the mode is detail,
// then the global SDF, then the WRC at 1.5x probe spacing, then the sky.
// Global hits shade from voxel lighting. Detail hits use the D6 card lookup.
// The ray-query builder marks a whole ShaderRD, so these variants are sibling
// shaders. #187 owns the tier switch; this runs beside voxel lighting while
// the fallback tier is active.
class NextGISWRTTrace {
public:
	enum Mode {
		MODE_GLOBAL = 0,
		MODE_DETAIL = 1,
	};

	static constexpr uint32_t MAX_STEPS = 64;
	static constexpr float DETAIL_METERS = 2.0f;
	static constexpr float SELF_OFFSET_VOXELS = 1.5f;
	static constexpr float WRC_HANDOFF_SCALE = 1.5f;
	static constexpr uint32_t FROXEL_AXIS = 4;
	static constexpr uint32_t FROXEL_CELLS = FROXEL_AXIS * FROXEL_AXIS * FROXEL_AXIS;
	static constexpr uint32_t FROXEL_CAP = 8;
	static constexpr uint32_t INSTANCE_CAP = 64;
	static constexpr uint32_t BRICK_BUDGET = 64;
	static constexpr uint32_t NO_INSTANCE = 0xFFFFFFFFu;

	struct Hit {
		bool hit = false;
		float t = 0.0f;
		Vector3 position;
		Vector3 normal;
		uint32_t steps = 0;
		bool detail = false;
		uint32_t instance_id = NO_INSTANCE;
	};

	struct ShadeResult {
		Color color;
		bool used_voxel = false;
		bool used_wrc = false;
		bool used_card = false;
		uint32_t card = NextGIVoxelLighting::NO_CARD;
	};

	struct DetailInstance {
		NextGIGlobalSDF::Instance instance;
		uint32_t id = 0;
	};

	// 4^3 cells over a 2 m cube. Each cell holds up to 8 instance indices.
	struct FroxelGrid {
		uint32_t counts[FROXEL_CELLS] = {};
		uint32_t indices[FROXEL_CELLS * FROXEL_CAP] = {};
		Vector3 origin;
		float cell = DETAIL_METERS / float(FROXEL_AXIS);
	};

	static Vector3 offset_origin(const Vector3 &p_origin, const Vector3 &p_normal, float p_voxel_size);
	static float wrc_handoff(float p_probe_spacing);
	// Reflections keep the requested length. Gather stops at 1.5x the WRC spacing.
	static float trace_limit(float p_distance_from_camera, float p_requested, bool p_reflect);
	// The instance and brick caps fill from geometry in front of the camera.
	// A point counts as in front until it is more than 1 m behind the view plane,
	// so a rock straddling the camera is not dropped for a nearer rock behind it.
	static bool instance_in_front(const Vector3 &p_camera, const Vector3 &p_view_forward, const Vector3 &p_origin);
	static bool instance_before(bool p_a_in_front, float p_a_distance_squared, bool p_b_in_front, float p_b_distance_squared);

	static uint32_t froxel_index(const FroxelGrid &p_grid, const Vector3 &p_position);
	static void build_froxel(const DetailInstance *p_instances, uint32_t p_count, const Vector3 &p_volume_origin, FroxelGrid &r_grid);

	static Hit trace_global(const NextGIGlobalSDF &p_field, const Vector3i *p_origins, uint32_t p_level_count, const Vector3 &p_camera, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax, const NextGIGlobalSDF::Instance *p_instances, uint32_t p_instance_count);
	static Hit trace_detail(const NextGIGlobalSDF &p_field, const Vector3i *p_origins, uint32_t p_level_count, const Vector3 &p_camera, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax, const DetailInstance *p_instances, uint32_t p_instance_count, const FroxelGrid *p_froxel, const NextGIGlobalSDF::Instance *p_global_instances, uint32_t p_global_count);

	static ShadeResult shade_global(float p_weight, const Color &p_voxel, const Color &p_wrc, const Color &p_sky, bool p_allow_wrc);
	static ShadeResult shade_detail(const NextGIVoxelLighting::CardQuery *p_cards, const Color *p_card_radiance, uint32_t p_card_count, const Vector3 &p_position, const Vector3 &p_normal, float p_voxel_weight, const Color &p_voxel, const Color &p_wrc, const Color &p_sky, bool p_allow_wrc);

	NextGISWRTTrace();
	~NextGISWRTTrace();
	NextGISWRTTrace(const NextGISWRTTrace &) = delete;
	NextGISWRTTrace &operator=(const NextGISWRTTrace &) = delete;

	// Fallback tier only. The caller already rejected probes, unshaded views and a disabled Environment.
	void update_frame(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting);

	uint32_t get_trace_steps() const { return trace_steps; }
	uint32_t get_trace_hits() const { return trace_hits; }
	uint32_t get_trace_misses() const { return trace_misses; }
	uint32_t get_shadow_rays() const { return shadow_rays; }
	uint32_t get_mode() const { return mode; }

private:
	bool _ensure_gpu();

	struct GpuState;
	GpuState *gpu = nullptr;
	NextGIGlobalSDFGPU field;
	Vector3i level_origins[NextGIGlobalSDF::MAX_LEVELS];
	bool level_origin_valid[NextGIGlobalSDF::MAX_LEVELS] = {};
	uint32_t trace_steps = 0;
	uint32_t trace_hits = 0;
	uint32_t trace_misses = 0;
	uint32_t shadow_rays = 0;
	uint32_t mode = MODE_GLOBAL;
	bool pipelines_failed = false;
};
