/**************************************************************************/
/*  light_function_atlas.h                                                */
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

#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/light_function_atlas_packer.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/light_function.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/shader_compiler.h"

namespace RendererRD {

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8, GM-11, M-9).
// Compiles `shader_type light_function;` materials, shelf-packs per-light square tiles into an
// R8 (default) or RGBA8 GPU atlas, bakes static tiles once on creation/change and animated
// (`TIME`-using) tiles once per frame, and supplies the per-view LightFunctionEntry SSBO consumed
// by Forward+, Volumetric Fog, NextGI SRC relight, RT shadows (#141), and MegaLights (#142).

class LightFunctionAtlas {
public:
	enum VariantIndex {
		VARIANT_R8 = 0,
		VARIANT_RGBA8 = 1,
		VARIANT_MAX = 2,
	};

	enum UniformSetIndex {
		SET_BASE = 0,
		SET_ATLAS = 1,
		SET_MATERIAL = 2,
	};

	static constexpr int SAMPLERS_BINDING_FIRST_INDEX = 1;

	struct PushConstant {
		uint32_t tile_origin[2] = { 0, 0 };
		uint32_t tile_size = 0;
		float time = 0.0f;
	};
	static_assert(sizeof(PushConstant) == 16, "LightFunctionAtlas::PushConstant must match light_function.glsl.");

	struct LightFunctionShaderData : public MaterialStorage::ShaderData {
		bool valid = false;
		bool uses_time = false;
		RID version;
		PipelineDeferredRD pipelines[VARIANT_MAX];
		Vector<ShaderCompiler::GeneratedCode::Texture> texture_uniforms;
		Vector<uint32_t> ubo_offsets;
		uint32_t ubo_size = 0;
		String code;

		virtual void set_code(const String &p_code) override;
		virtual bool is_animated() const override;
		virtual bool casts_shadows() const override;
		virtual RenderingServerTypes::ShaderNativeSourceCode get_native_source_code() const override;
		virtual Pair<ShaderRD *, RID> get_native_shader_and_version() const override;

		LightFunctionShaderData() {}
		virtual ~LightFunctionShaderData();
	};

	struct LightFunctionMaterialData : public MaterialStorage::MaterialData {
		LightFunctionShaderData *shader_data = nullptr;
		RID uniform_set;
		bool uniform_set_updated = false;

		virtual void set_render_priority(int p_priority) override {}
		virtual void set_next_pass(RID p_pass) override {}
		virtual bool update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) override;
		virtual ~LightFunctionMaterialData();
	};

private:
	static LightFunctionAtlas *singleton;

	LightFunctionShaderRD shader;
	ShaderCompiler compiler;
	RID default_shader;
	RID default_material;
	RID default_shader_rd[VARIANT_MAX];
	RID base_uniform_set;

	// Lazily created only once a light with a valid light_function is registered.
	RID atlas_texture;
	RID atlas_uniform_set;
	uint32_t gpu_atlas_size = 0;
	LightFunctionAtlasPacker::Format gpu_atlas_format = LightFunctionAtlasPacker::FORMAT_R8;
	bool gpu_atlas_rgba = false;

	// Tiny zeroed fallback SSBO bound when no light function is active.
	RID dummy_entry_buffer;

	// Per-view active light function entries built by LightStorage::update_light_buffers().
	LocalVector<LightFunctionAtlasPacker::EntryGPU> view_entries;
	RID view_entry_buffer;
	uint32_t view_entry_capacity = 0;

	LightFunctionAtlasPacker packer;
	bool initialized = false;
	bool r8_storage_supported = true;

	// Maps Light RID -> assigned Material RID (only lights with a non-null material RID).
	HashMap<RID, RID> light_materials;

	static MaterialStorage::ShaderData *_create_shader_funcs();
	static MaterialStorage::MaterialData *_create_material_funcs(MaterialStorage::ShaderData *p_shader);
	MaterialStorage::ShaderData *_create_shader_func();
	MaterialStorage::MaterialData *_create_material_func(LightFunctionShaderData *p_shader);

	void _free_atlas_texture();
	bool _ensure_atlas_texture(uint32_t p_size, LightFunctionAtlasPacker::Format p_format);
	void _ensure_dummy_entry_buffer();
	void _ensure_view_entry_buffer(uint32_t p_count);

public:
	static LightFunctionAtlas *get_singleton() { return singleton; }

	// Project settings readers (read live so runtime setting changes take effect immediately).
	static uint32_t read_atlas_size();
	static LightFunctionAtlasPacker::Format read_atlas_format();
	static bool read_translucent_enabled();

	void init();
	void free_resources();

	// Light registration called by LightStorage::light_set_light_function / light_free.
	void set_light_material(RID p_light, RID p_material);
	void remove_light(RID p_light);

	// True when at least one light currently has a light_function assigned.
	bool has_active_lights() const { return !light_materials.is_empty(); }
	bool has_baked_atlas() const { return atlas_texture.is_valid() && packer.get_active_count() > 0; }
	bool is_atlas_rgba() const { return gpu_atlas_rgba; }

	// Bakes all dirty static tiles and (at most once per frame) animated tiles.
	// Complete no-op when no light has a light_function assigned.
	void bake(uint64_t p_frame_number, double p_time);

	// Per-view entry buffer construction driven by LightStorage::update_light_buffers().
	void begin_view_entries();
	uint32_t append_view_entry(RID p_light, LightFunctionAtlasPacker::Mode p_mode, const Transform3D &p_light_world_xform, const Vector3 &p_scale, float p_spot_angle_deg);
	void finish_view_entries();

	// Helper for NextGI SRC relight (builds a single light's EntryGPU against the current atlas).
	bool build_entry_for_light(RID p_light, LightFunctionAtlasPacker::Mode p_mode, const Transform3D &p_light_world_xform, const Vector3 &p_scale, float p_spot_angle_deg, LightFunctionAtlasPacker::EntryGPU &r_entry) const;

	// Consumer bindings (always valid RIDs; defaults when inactive).
	RID get_atlas_texture_or_default() const;
	RID get_view_entry_buffer_or_default();
	uint32_t get_view_entry_count() const { return view_entries.size(); }

	Dictionary get_stats() const;

	LightFunctionAtlas();
	~LightFunctionAtlas();
};

} // namespace RendererRD
