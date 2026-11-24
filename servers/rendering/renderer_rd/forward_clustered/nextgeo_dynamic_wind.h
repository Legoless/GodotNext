/**************************************************************************/
/*  nextgeo_dynamic_wind.h                                                */
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

#include "servers/rendering/storage/dynamic_wind_sim.h"

// GodotNext: nextgeo_dynamic_wind (#296, plan §21.4).
// RendererRD namespace aliases for the Dynamic Wind floor evaluator, one-time session logger,
// and clean-room spring+flutter CPU reference (defined in servers/rendering/storage/dynamic_wind_sim.h).
namespace RendererRD {

using NextGeoDynamicWindReason = NextGeoDynamicWindSim::Reason;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_NONE = NextGeoDynamicWindSim::REASON_NONE;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_DISABLED = NextGeoDynamicWindSim::REASON_DISABLED;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_NEXTGEO_UNAVAILABLE = NextGeoDynamicWindSim::REASON_NEXTGEO_UNAVAILABLE;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_BELOW_TIER_FLOOR = NextGeoDynamicWindSim::REASON_BELOW_TIER_FLOOR;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_SKINNED_UNAVAILABLE = NextGeoDynamicWindSim::REASON_SKINNED_UNAVAILABLE;
static constexpr NextGeoDynamicWindReason NEXTGEO_DYNAMIC_WIND_REASON_FOLIAGE_UNAVAILABLE = NextGeoDynamicWindSim::REASON_FOLIAGE_UNAVAILABLE;

using NextGeoDynamicWindFloor = NextGeoDynamicWindSim::FloorResult;
using NextGeoDynamicWindFloorLogger = NextGeoDynamicWindSim::FloorLogger;
using NextGeoDynamicWindSimParamsGPU = NextGeoDynamicWindSim::SimParamsGPU;

inline const char *nextgeo_dynamic_wind_reason_name(NextGeoDynamicWindReason p_reason) {
	return NextGeoDynamicWindSim::reason_name(p_reason);
}

inline NextGeoDynamicWindFloor nextgeo_dynamic_wind_evaluate_floor(
		bool p_setting_enabled,
		bool p_nextgeo_available,
		bool p_hybrid_tier_supported,
		bool p_skinned_available,
		bool p_foliage_available) {
	return NextGeoDynamicWindSim::evaluate_floor(p_setting_enabled, p_nextgeo_available, p_hybrid_tier_supported, p_skinned_available, p_foliage_available);
}

inline void nextgeo_dynamic_wind_step_bone(
		const RendererVirtualMeshStorage::WindBoneGPU &p_bone,
		const RendererVirtualMeshStorage::DynamicWindSlotDescGPU &p_slot,
		const Vector3 &p_global_wind_dir,
		float p_global_wind_strength,
		float p_gust_frequency,
		float p_time,
		float p_dt,
		const RendererVirtualMeshStorage::WindForceGPU *p_forces,
		uint32_t p_force_count,
		RendererVirtualMeshStorage::DynamicWindBoneStateGPU &r_state,
		float r_cur_rows[12],
		float r_prev_rows[12]) {
	NextGeoDynamicWindSim::step_bone(p_bone, p_slot, p_global_wind_dir, p_global_wind_strength, p_gust_frequency, p_time, p_dt, p_forces, p_force_count, r_state, r_cur_rows, r_prev_rows);
}

inline bool nextgeo_dynamic_wind_force_overlaps_aabb(
		const RendererVirtualMeshStorage::WindForceGPU &p_force,
		const Vector3 &p_aabb_min,
		const Vector3 &p_aabb_max,
		float p_margin = 0.0f) {
	return NextGeoDynamicWindSim::force_overlaps_aabb(p_force, p_aabb_min, p_aabb_max, p_margin);
}

} // namespace RendererRD
