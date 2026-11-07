/**************************************************************************/
/*  nextgi_velocity.h                                                     */
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

// §6.1 velocity table while NextGI is active (#37). NextGI off keeps today's path.

enum NextGIVelocityScale {
	NEXTGI_VELOCITY_SCALE_NONE,
	NEXTGI_VELOCITY_SCALE_FSR2,
	NEXTGI_VELOCITY_SCALE_MFX,
	NEXTGI_VELOCITY_SCALE_EXTERNAL,
};

enum NextGIVelocityMode {
	NEXTGI_VELOCITY_NONE,
	NEXTGI_VELOCITY_TAA,
	NEXTGI_VELOCITY_SENTINEL, // FSR2.
	NEXTGI_VELOCITY_DENSE, // MetalFX. #104 maps DLSS onto this row.
};

struct NextGIVelocityPlan {
	bool active = false;
	float pre_clear_r = 0.0f;
	float pre_clear_g = 0.0f;
	bool prepass_writes = false;
	bool color_pass_loads = false;
	bool skip_clear_pass = false;
	bool mfx_preserve = false;
};

inline NextGIVelocityPlan nextgi_velocity_plan(NextGIVelocityScale p_scale, NextGIVelocityMode p_mode, bool p_using_taa, bool p_using_nextgi) {
	NextGIVelocityPlan plan;
	if (!p_using_nextgi) {
		return plan;
	}
	// The upscaler row wins over TAA. p_scale is the caller's scale; p_mode is the row.
	(void)p_scale;
	(void)p_using_taa;
	plan.active = true;
	plan.prepass_writes = true;
	switch (p_mode) {
		case NEXTGI_VELOCITY_TAA:
			plan.color_pass_loads = true;
			break;
		case NEXTGI_VELOCITY_SENTINEL:
			plan.pre_clear_r = -1.0f;
			plan.pre_clear_g = -1.0f;
			plan.skip_clear_pass = true;
			break;
		case NEXTGI_VELOCITY_DENSE:
			plan.pre_clear_r = -1.0f;
			plan.pre_clear_g = -1.0f;
			plan.mfx_preserve = true;
			break;
		case NEXTGI_VELOCITY_NONE:
		default:
			break;
	}
	return plan;
}

// PRESERVE_WRITTEN overwrites a texel only when it is exactly the (-1, -1) pre-clear.
inline bool nextgi_velocity_texel_is_preclear(float p_x, float p_y) {
	return p_x == -1.0f && p_y == -1.0f;
}
