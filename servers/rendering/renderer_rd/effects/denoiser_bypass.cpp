/**************************************************************************/
/*  denoiser_bypass.cpp                                                   */
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

#include "denoiser_bypass.h"

#include "core/os/mutex.h"
#include "core/templates/hash_set.h"

namespace {

struct DenoiserBypassState {
	Mutex mutex;
	Vector<StringName> registered;
	HashSet<const RenderSceneBuffersRD *> bypassed;
};

DenoiserBypassState &denoiser_bypass_state() {
	static DenoiserBypassState state;
	return state;
}

} // namespace

void DenoiserBypass::register_denoiser(const StringName &p_name) {
	if (p_name == StringName()) {
		return;
	}
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	if (!state.registered.has(p_name)) {
		state.registered.push_back(p_name);
	}
}

Vector<StringName> DenoiserBypass::get_registered_denoisers() {
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	return state.registered;
}

void DenoiserBypass::set_bypassed(const RenderSceneBuffersRD *p_rb, bool p_bypassed) {
	if (p_rb == nullptr) {
		return;
	}
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	if (p_bypassed) {
		state.bypassed.insert(p_rb);
	} else {
		state.bypassed.erase(p_rb);
	}
}

bool DenoiserBypass::is_bypassed(const RenderSceneBuffersRD *p_rb) {
	if (p_rb == nullptr) {
		return false;
	}
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	return state.bypassed.has(p_rb);
}

void DenoiserBypass::clear_rb(const RenderSceneBuffersRD *p_rb) {
	if (p_rb == nullptr) {
		return;
	}
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	state.bypassed.erase(p_rb);
}

void DenoiserBypass::clear_for_tests() {
	DenoiserBypassState &state = denoiser_bypass_state();
	MutexLock lock(state.mutex);
	state.registered.clear();
	state.bypassed.clear();
}
