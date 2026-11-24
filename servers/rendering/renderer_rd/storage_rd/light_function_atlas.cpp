/**************************************************************************/
/*  light_function_atlas.cpp                                              */
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

#include "light_function_atlas.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_default.h"

using namespace RendererRD;

LightFunctionAtlas *LightFunctionAtlas::singleton = nullptr;

LightFunctionAtlas::LightFunctionAtlas() {
	singleton = this;
}

LightFunctionAtlas::~LightFunctionAtlas() {
	free_resources();
	if (singleton == this) {
		singleton = nullptr;
	}
}

uint32_t LightFunctionAtlas::read_atlas_size() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return LightFunctionAtlasPacker::DEFAULT_ATLAS_SIZE;
	}
	const StringName key = SNAME("rendering/lights_and_shadows/light_function_atlas/size");
	if (!ps->has_setting(key)) {
		return LightFunctionAtlasPacker::DEFAULT_ATLAS_SIZE;
	}
	const int64_t val = int64_t(ps->get_setting_with_override(key));
	return LightFunctionAtlasPacker::sanitize_atlas_size(uint32_t(MAX(val, int64_t(LightFunctionAtlasPacker::MIN_ATLAS_SIZE))));
}

LightFunctionAtlasPacker::Format LightFunctionAtlas::read_atlas_format() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return LightFunctionAtlasPacker::FORMAT_R8;
	}
	const StringName key = SNAME("rendering/lights_and_shadows/light_function_atlas/format");
	if (!ps->has_setting(key)) {
		return LightFunctionAtlasPacker::FORMAT_R8;
	}
	const int val = int(ps->get_setting_with_override(key));
	return val == 1 ? LightFunctionAtlasPacker::FORMAT_RGBA8 : LightFunctionAtlasPacker::FORMAT_R8;
}

bool LightFunctionAtlas::read_translucent_enabled() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return false;
	}
	const StringName key = SNAME("rendering/lights_and_shadows/light_function_atlas/translucent");
	if (!ps->has_setting(key)) {
		return false;
	}
	return bool(ps->get_setting_with_override(key));
}

////////////////////////////////////////////////////////////////////////////////
// LightFunctionShaderData / LightFunctionMaterialData

void LightFunctionAtlas::LightFunctionShaderData::set_code(const String &p_code) {
	code = p_code;
	valid = false;
	uses_time = false;
	ubo_size = 0;
	uniforms.clear();

	if (code.is_empty()) {
		return;
	}

	LightFunctionAtlas *atlas = LightFunctionAtlas::get_singleton();
	ERR_FAIL_NULL(atlas);

	ShaderCompiler::GeneratedCode gen_code;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["light_function"] = ShaderCompiler::STAGE_COMPUTE;
	actions.usage_flag_pointers["TIME"] = &uses_time;
	actions.uniforms = &uniforms;

	Error err = atlas->compiler.compile(RSE::SHADER_LIGHT_FUNCTION, code, &actions, path, gen_code);
	ERR_FAIL_COND_MSG(err != OK, "Light function shader compilation failed.");

	if (version.is_null()) {
		version = atlas->shader.version_create();
	} else {
		for (int i = 0; i < VARIANT_MAX; i++) {
			pipelines[i].free();
		}
	}

	atlas->shader.version_set_compute_code(version, gen_code.code, gen_code.uniforms, gen_code.stage_globals[ShaderCompiler::STAGE_COMPUTE], gen_code.defines);
	ERR_FAIL_COND(!atlas->shader.version_is_valid(version));

	ubo_size = gen_code.uniform_total_size;
	ubo_offsets = gen_code.uniform_offsets;
	texture_uniforms = gen_code.texture_uniforms;

	for (int i = 0; i < VARIANT_MAX; i++) {
		pipelines[i].create_compute_pipeline(atlas->shader.version_get_shader(version, i));
	}

	valid = true;
}

bool LightFunctionAtlas::LightFunctionShaderData::is_animated() const {
	return uses_time;
}

bool LightFunctionAtlas::LightFunctionShaderData::casts_shadows() const {
	return false;
}

RenderingServerTypes::ShaderNativeSourceCode LightFunctionAtlas::LightFunctionShaderData::get_native_source_code() const {
	LightFunctionAtlas *atlas = LightFunctionAtlas::get_singleton();
	ERR_FAIL_NULL_V(atlas, RenderingServerTypes::ShaderNativeSourceCode());
	return atlas->shader.version_get_native_source_code(version);
}

Pair<ShaderRD *, RID> LightFunctionAtlas::LightFunctionShaderData::get_native_shader_and_version() const {
	LightFunctionAtlas *atlas = LightFunctionAtlas::get_singleton();
	ERR_FAIL_NULL_V(atlas, (Pair<ShaderRD *, RID>()));
	return { &atlas->shader, version };
}

LightFunctionAtlas::LightFunctionShaderData::~LightFunctionShaderData() {
	for (int i = 0; i < VARIANT_MAX; i++) {
		pipelines[i].free();
	}

	LightFunctionAtlas *atlas = LightFunctionAtlas::get_singleton();
	if (atlas != nullptr && version.is_valid()) {
		atlas->shader.version_free(version);
	}
}

bool LightFunctionAtlas::LightFunctionMaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	uniform_set_updated = true;

	LightFunctionAtlas *atlas = LightFunctionAtlas::get_singleton();
	if (atlas == nullptr || shader_data == nullptr || !shader_data->valid) {
		return false;
	}

	return update_parameters_uniform_set(
			p_parameters,
			p_uniform_dirty,
			p_textures_dirty,
			shader_data->uniforms,
			shader_data->ubo_offsets.ptr(),
			shader_data->texture_uniforms,
			shader_data->default_texture_params,
			shader_data->ubo_size,
			uniform_set,
			atlas->shader.version_get_shader(shader_data->version, VARIANT_R8),
			SET_MATERIAL,
			true,
			true);
}

LightFunctionAtlas::LightFunctionMaterialData::~LightFunctionMaterialData() {
	free_parameters_uniform_set(uniform_set);
}

MaterialStorage::ShaderData *LightFunctionAtlas::_create_shader_func() {
	return memnew(LightFunctionShaderData);
}

MaterialStorage::ShaderData *LightFunctionAtlas::_create_shader_funcs() {
	return LightFunctionAtlas::get_singleton()->_create_shader_func();
}

MaterialStorage::MaterialData *LightFunctionAtlas::_create_material_func(LightFunctionShaderData *p_shader) {
	LightFunctionMaterialData *md = memnew(LightFunctionMaterialData);
	md->shader_data = p_shader;
	return md;
}

MaterialStorage::MaterialData *LightFunctionAtlas::_create_material_funcs(MaterialStorage::ShaderData *p_shader) {
	return LightFunctionAtlas::get_singleton()->_create_material_func(static_cast<LightFunctionShaderData *>(p_shader));
}

////////////////////////////////////////////////////////////////////////////////
// Lifecycle

void LightFunctionAtlas::init() {
	if (initialized) {
		return;
	}

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL(material_storage);
	ERR_FAIL_NULL(rd);

	r8_storage_supported = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT);

	{
		String defines = "#define SAMPLERS_BINDING_FIRST_INDEX " + itos(SAMPLERS_BINDING_FIRST_INDEX) + "\n";
		Vector<ShaderRD::VariantDefine> variants;
		variants.push_back(ShaderRD::VariantDefine(0, "", false));
		variants.push_back(ShaderRD::VariantDefine(0, "\n#define LIGHT_FUNCTION_RGBA8\n", false));
		shader.initialize(variants, defines);
		shader.enable_group(0);

		material_storage->shader_set_data_request_function(MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION, _create_shader_funcs);
		material_storage->material_set_data_request_function(MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION, _create_material_funcs);
	}

	{
		ShaderCompiler::DefaultIdentifierActions actions;
		actions.renames["TIME"] = "params.time";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["LIGHT_UV"] = "light_uv";
		actions.renames["LIGHT_MASK"] = "light_mask";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = SET_MATERIAL;
		actions.base_uniform_string = "material.";
		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms.data";

		compiler.initialize(actions);
	}

	{
		default_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(default_shader);
		material_storage->shader_set_code(default_shader, R"(
// Default light function shader.
shader_type light_function;

void light_function() {
	LIGHT_MASK = vec3(1.0);
}
)");
		default_material = material_storage->material_allocate();
		material_storage->material_initialize(default_material);
		material_storage->material_set_shader(default_material, default_shader);

		LightFunctionMaterialData *md = static_cast<LightFunctionMaterialData *>(material_storage->material_get_data(default_material, MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION));
		ERR_FAIL_NULL(md);
		ERR_FAIL_NULL(md->shader_data);
		for (int i = 0; i < VARIANT_MAX; i++) {
			default_shader_rd[i] = shader.version_get_shader(md->shader_data->version, i);
		}

		Vector<RD::Uniform> uniforms;
		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 0;
			u.append_id(material_storage->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}
		material_storage->samplers_rd_get_default().append_uniforms(uniforms, SAMPLERS_BINDING_FIRST_INDEX);
		base_uniform_set = rd->uniform_set_create(uniforms, default_shader_rd[VARIANT_R8], SET_BASE);
	}

	packer.set_atlas_size(read_atlas_size());
	initialized = true;
}

void LightFunctionAtlas::_free_atlas_texture() {
	RD *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (atlas_uniform_set.is_valid() && rd->uniform_set_is_valid(atlas_uniform_set)) {
			rd->free_rid(atlas_uniform_set);
		}
		if (atlas_texture.is_valid()) {
			rd->free_rid(atlas_texture);
		}
	}
	atlas_uniform_set = RID();
	atlas_texture = RID();
	gpu_atlas_size = 0;
	gpu_atlas_rgba = false;
}

void LightFunctionAtlas::free_resources() {
	if (!initialized) {
		return;
	}

	_free_atlas_texture();

	RD *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (base_uniform_set.is_valid() && rd->uniform_set_is_valid(base_uniform_set)) {
			rd->free_rid(base_uniform_set);
		}
		if (dummy_entry_buffer.is_valid()) {
			rd->free_rid(dummy_entry_buffer);
		}
		if (view_entry_buffer.is_valid()) {
			rd->free_rid(view_entry_buffer);
		}
	}
	base_uniform_set = RID();
	dummy_entry_buffer = RID();
	view_entry_buffer = RID();
	view_entry_capacity = 0;
	view_entries.clear();

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	if (material_storage != nullptr) {
		if (default_material.is_valid()) {
			material_storage->material_free(default_material);
		}
		if (default_shader.is_valid()) {
			material_storage->shader_free(default_shader);
		}
		material_storage->shader_set_data_request_function(MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION, nullptr);
		material_storage->material_set_data_request_function(MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION, nullptr);
	}
	default_material = RID();
	default_shader = RID();
	for (int i = 0; i < VARIANT_MAX; i++) {
		default_shader_rd[i] = RID();
	}

	light_materials.clear();
	packer.clear();
	initialized = false;
}

bool LightFunctionAtlas::_ensure_atlas_texture(uint32_t p_size, LightFunctionAtlasPacker::Format p_format) {
	const bool want_rgba = (p_format == LightFunctionAtlasPacker::FORMAT_RGBA8) || !r8_storage_supported;
	const LightFunctionAtlasPacker::Format resolved_format = want_rgba ? LightFunctionAtlasPacker::FORMAT_RGBA8 : LightFunctionAtlasPacker::FORMAT_R8;

	if (atlas_texture.is_valid() && gpu_atlas_size == p_size && gpu_atlas_format == resolved_format && atlas_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(atlas_uniform_set)) {
		return true;
	}

	_free_atlas_texture();

	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	RD::TextureFormat tf;
	tf.format = want_rgba ? RD::DATA_FORMAT_R8G8B8A8_UNORM : RD::DATA_FORMAT_R8_UNORM;
	tf.width = p_size;
	tf.height = p_size;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	atlas_texture = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(!atlas_texture.is_valid(), false);
	rd->set_resource_name(atlas_texture, "LightFunctionAtlas");
	rd->texture_clear(atlas_texture, Color(1, 1, 1, 1), 0, 1, 0, 1);

	Vector<RD::Uniform> uniforms;
	RD::Uniform u;
	u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
	u.binding = 0;
	u.append_id(atlas_texture);
	uniforms.push_back(u);

	const int variant = want_rgba ? VARIANT_RGBA8 : VARIANT_R8;
	atlas_uniform_set = rd->uniform_set_create(uniforms, default_shader_rd[variant], SET_ATLAS);
	ERR_FAIL_COND_V(!atlas_uniform_set.is_valid(), false);

	gpu_atlas_size = p_size;
	gpu_atlas_format = resolved_format;
	gpu_atlas_rgba = want_rgba;

	packer.set_atlas_size(p_size);
	packer.mark_all_dirty();
	return true;
}

void LightFunctionAtlas::_ensure_dummy_entry_buffer() {
	if (dummy_entry_buffer.is_valid()) {
		return;
	}
	LightFunctionAtlasPacker::EntryGPU zero_entry;
	dummy_entry_buffer = RD::get_singleton()->storage_buffer_create(sizeof(zero_entry), Span<uint8_t>((const uint8_t *)&zero_entry, sizeof(zero_entry)));
	RD::get_singleton()->set_resource_name(dummy_entry_buffer, "LightFunctionAtlasDummyBuffer");
}

void LightFunctionAtlas::_ensure_view_entry_buffer(uint32_t p_count) {
	const uint32_t needed = MAX(p_count, 1u);
	if (view_entry_buffer.is_valid() && view_entry_capacity >= needed) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (view_entry_buffer.is_valid()) {
		rd->free_rid(view_entry_buffer);
	}
	view_entry_capacity = Math::next_power_of_2(MAX(needed, 16u));
	view_entry_buffer = rd->storage_buffer_create(view_entry_capacity * sizeof(LightFunctionAtlasPacker::EntryGPU));
	rd->set_resource_name(view_entry_buffer, "LightFunctionAtlasViewEntries");
}

////////////////////////////////////////////////////////////////////////////////
// Light registration & baking

void LightFunctionAtlas::set_light_material(RID p_light, RID p_material) {
	if (!p_light.is_valid()) {
		return;
	}
	if (!p_material.is_valid()) {
		remove_light(p_light);
		return;
	}

	light_materials[p_light] = p_material;

	if (!initialized) {
		return;
	}

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	bool is_animated = false;
	uint64_t serial = 0;
	if (material_storage != nullptr && material_storage->owns_material(p_material)) {
		serial = material_storage->material_get_change_serial(p_material);
		LightFunctionMaterialData *md = static_cast<LightFunctionMaterialData *>(material_storage->material_get_data(p_material, MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION));
		if (md != nullptr && md->shader_data != nullptr && md->shader_data->valid) {
			is_animated = md->shader_data->uses_time;
		}
	}

	packer.register_or_update_light(p_light.get_id(), p_material.get_id(), is_animated, serial);
}

void LightFunctionAtlas::remove_light(RID p_light) {
	if (!p_light.is_valid()) {
		return;
	}
	light_materials.erase(p_light);
	packer.unregister_light(p_light.get_id());
}

void LightFunctionAtlas::bake(uint64_t p_frame_number, double p_time) {
	if (!initialized || light_materials.is_empty()) {
		return;
	}

	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RD *rd = RD::get_singleton();
	if (material_storage == nullptr || rd == nullptr) {
		return;
	}

	// Ensure queued material parameter/shader updates have flushed before checking validity and serials.
	material_storage->_update_queued_materials();

	// Sync each registered light against its current MaterialStorage state.
	bool any_valid = false;
	bool any_animated = false;
	LocalVector<RID> stale_lights;
	for (const KeyValue<RID, RID> &kv : light_materials) {
		const RID light_rid = kv.key;
		const RID mat_rid = kv.value;
		if (!material_storage->owns_material(mat_rid)) {
			stale_lights.push_back(light_rid);
			continue;
		}
		LightFunctionMaterialData *md = static_cast<LightFunctionMaterialData *>(material_storage->material_get_data(mat_rid, MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION));
		if (md == nullptr || md->shader_data == nullptr || !md->shader_data->valid) {
			packer.unregister_light(light_rid.get_id());
			continue;
		}
		any_valid = true;
		any_animated = any_animated || md->shader_data->uses_time;
		const uint64_t serial = material_storage->material_get_change_serial(mat_rid);
		packer.register_or_update_light(light_rid.get_id(), mat_rid.get_id(), md->shader_data->uses_time, serial);
	}
	for (const RID &stale : stale_lights) {
		remove_light(stale);
	}

	if (!any_valid) {
		return;
	}

	const uint32_t target_size = read_atlas_size();
	const LightFunctionAtlasPacker::Format target_format = read_atlas_format();
	if (!_ensure_atlas_texture(target_size, target_format)) {
		return;
	}

	LocalVector<LightFunctionAtlasPacker::BakeJob> jobs;
	packer.collect_bake_jobs(p_frame_number, jobs);

	if (any_animated) {
		RenderingServerDefault::redraw_request();
	}

	if (jobs.is_empty()) {
		return;
	}

	const int variant = gpu_atlas_rgba ? VARIANT_RGBA8 : VARIANT_R8;

	rd->draw_command_begin_label("Bake Light Function Atlas");
	RD::ComputeListID compute_list = rd->compute_list_begin();

	for (uint32_t i = 0; i < jobs.size(); i++) {
		const LightFunctionAtlasPacker::BakeJob &job = jobs[i];
		const RID mat_rid = RID::from_uint64(job.material_id);
		if (!material_storage->owns_material(mat_rid)) {
			continue;
		}
		LightFunctionMaterialData *md = static_cast<LightFunctionMaterialData *>(material_storage->material_get_data(mat_rid, MaterialStorage::SHADER_TYPE_LIGHT_FUNCTION));
		if (md == nullptr || md->shader_data == nullptr || !md->shader_data->valid) {
			continue;
		}
		const RID pipeline = md->shader_data->pipelines[variant].get_rid();
		if (!pipeline.is_valid()) {
			continue;
		}

		PushConstant pc;
		pc.tile_origin[0] = job.rect.x;
		pc.tile_origin[1] = job.rect.y;
		pc.tile_size = job.rect.size;
		pc.time = float(p_time);

		rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
		rd->compute_list_bind_uniform_set(compute_list, base_uniform_set, SET_BASE);
		rd->compute_list_bind_uniform_set(compute_list, atlas_uniform_set, SET_ATLAS);
		if (md->uniform_set.is_valid() && rd->uniform_set_is_valid(md->uniform_set)) {
			rd->compute_list_bind_uniform_set(compute_list, md->uniform_set, SET_MATERIAL);
			md->set_as_used();
		}
		rd->compute_list_set_push_constant(compute_list, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(compute_list, job.rect.size, job.rect.size, 1);
	}

	rd->compute_list_end();
	rd->draw_command_end_label();
}

////////////////////////////////////////////////////////////////////////////////
// Per-view entry table & consumer accessors

void LightFunctionAtlas::begin_view_entries() {
	view_entries.clear();
}

bool LightFunctionAtlas::build_entry_for_light(RID p_light, LightFunctionAtlasPacker::Mode p_mode, const Transform3D &p_light_world_xform, const Vector3 &p_scale, float p_spot_angle_deg, LightFunctionAtlasPacker::EntryGPU &r_entry) const {
	if (!has_baked_atlas() || !p_light.is_valid()) {
		return false;
	}
	const LightFunctionAtlasPacker::TileRect rect = packer.get_tile_rect(p_light.get_id());
	if (!rect.valid) {
		return false;
	}
	r_entry = LightFunctionAtlasPacker::build_entry_gpu(rect, gpu_atlas_size, p_mode, p_light_world_xform, p_scale, p_spot_angle_deg, gpu_atlas_rgba);
	return true;
}

uint32_t LightFunctionAtlas::append_view_entry(RID p_light, LightFunctionAtlasPacker::Mode p_mode, const Transform3D &p_light_world_xform, const Vector3 &p_scale, float p_spot_angle_deg) {
	LightFunctionAtlasPacker::EntryGPU entry;
	if (!build_entry_for_light(p_light, p_mode, p_light_world_xform, p_scale, p_spot_angle_deg, entry)) {
		return LightFunctionAtlasPacker::ENTRY_NONE;
	}
	const uint32_t idx = view_entries.size();
	view_entries.push_back(entry);
	return idx;
}

void LightFunctionAtlas::finish_view_entries() {
	if (view_entries.is_empty()) {
		return;
	}
	_ensure_view_entry_buffer(view_entries.size());
	RD::get_singleton()->buffer_update(view_entry_buffer, 0, view_entries.size() * sizeof(LightFunctionAtlasPacker::EntryGPU), view_entries.ptr());
}

RID LightFunctionAtlas::get_atlas_texture_or_default() const {
	if (has_baked_atlas()) {
		return atlas_texture;
	}
	return TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
}

RID LightFunctionAtlas::get_view_entry_buffer_or_default() {
	if (!view_entries.is_empty() && view_entry_buffer.is_valid()) {
		return view_entry_buffer;
	}
	_ensure_dummy_entry_buffer();
	return dummy_entry_buffer;
}

Dictionary LightFunctionAtlas::get_stats() const {
	const LightFunctionAtlasPacker::Stats &s = packer.get_stats();
	Dictionary d;
	d["atlas_size"] = int64_t(gpu_atlas_size > 0 ? gpu_atlas_size : read_atlas_size());
	d["format"] = String(gpu_atlas_size > 0 ? (gpu_atlas_rgba ? "RGBA8" : "R8") : (read_atlas_format() == LightFunctionAtlasPacker::FORMAT_RGBA8 ? "RGBA8" : "R8"));
	d["translucent"] = read_translucent_enabled();
	d["tile_size"] = int64_t(s.current_tile_size);
	d["active_tiles"] = int64_t(s.active_tiles);
	d["static_tiles"] = int64_t(s.static_tiles);
	d["animated_tiles"] = int64_t(s.animated_tiles);
	d["last_frame_bakes"] = int64_t(s.last_frame_bakes);
	d["total_bakes"] = int64_t(s.total_bakes);
	d["static_bakes"] = int64_t(s.static_bakes);
	d["animated_bakes"] = int64_t(s.animated_bakes);
	d["repacks"] = int64_t(s.repacks);
	d["view_entries"] = int64_t(view_entries.size());
	return d;
}
