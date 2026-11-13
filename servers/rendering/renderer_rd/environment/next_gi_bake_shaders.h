/**************************************************************************/
/*  next_gi_bake_shaders.h                                                */
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

#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "core/templates/rid.h"

class ShaderRD;

namespace RendererRD {

// GodotNext: nextgi_vulkan (#50). NextGI creates its ray-query ShaderRDs on first use on the HWRT tier,
// which an export never reaches, so the shader baker saw none of them. This holds bake-only instances
// (gather, both SRC relight radiance formats, WRC update and the reference tracer), each initialized
// with its runtime twin's variant defines so the baked cache path is the one the runtime looks up.
// Creating them compiles nothing; the versions are freed with this object.
class NextGIBakeShaders {
	struct Instances;
	Instances *instances = nullptr;
	LocalVector<Pair<ShaderRD *, RID>> versions;

	void _add(ShaderRD &p_shader, const Vector<String> &p_variant_defines);

public:
	const LocalVector<Pair<ShaderRD *, RID>> &get_versions() const { return versions; }

	NextGIBakeShaders();
	~NextGIBakeShaders();
};

} // namespace RendererRD
