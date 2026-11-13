/**************************************************************************/
/*  next_gi_bake_shaders.cpp                                              */
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

#include "next_gi_bake_shaders.h"

#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update.glsl.gen.h"

using namespace RendererRD;

struct NextGIBakeShaders::Instances {
	NextgiGatherShaderRD gather;
	NextgiSrcRelightShaderRD relight_packed;
	NextgiSrcRelightShaderRD relight_rgba16f;
	NextgiWrcUpdateShaderRD wrc_update;
	NextgiReferenceShaderRD reference;
};

void NextGIBakeShaders::_add(ShaderRD &p_shader, const Vector<String> &p_variant_defines) {
	p_shader.initialize(p_variant_defines);
	const RID version = p_shader.version_create(false);
	ERR_FAIL_COND(!version.is_valid());
	versions.push_back({ &p_shader, version });
}

NextGIBakeShaders::NextGIBakeShaders() {
	instances = memnew(Instances);
	_add(instances->gather, NextGIGather::get_shader_variant_defines());
	_add(instances->relight_packed, NextGIRelight::get_shader_variant_defines(true));
	_add(instances->relight_rgba16f, NextGIRelight::get_shader_variant_defines(false));
	_add(instances->wrc_update, NextGIWRC::get_shader_variant_defines());
	_add(instances->reference, NextGIReference::get_shader_variant_defines());
}

NextGIBakeShaders::~NextGIBakeShaders() {
	for (const Pair<ShaderRD *, RID> &version : versions) {
		version.first->version_free(version.second);
	}
	versions.clear();
	memdelete(instances);
}
