/**************************************************************************/
/*  vendor_seam.c                                                         */
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

// GodotNext: pu_vendor_seam_parity (#210, plan §21.5 U2, GU+-1). A test-only GDExtension in plain C
// (no godot-cpp) that implements both vendor seams the way a vendor plugin would. It is not a vendor
// upscaler or frame generator and uses no AMD, Intel or NVIDIA file.
//
// - SeamCopy extends TemporalUpscalerExtension: it copies the colour input to the output with a
//   compute shader recorded through the RenderingDevice of RenderingServer.get_rendering_device(), as
//   a vendor upscaler records its passes. A copy cannot resolve jitter, so it asks for a scale of
//   exactly 1.0, no jitter and no mip bias offset: its frame equals the frame without an upscaler.
// - SeamPassthrough extends FrameGenerationExtension: it generates no frames. It counts the calls,
//   checks that every input texture is valid and counts the D3D12 present pairs.
//
// The library registers one instance of each with the RenderingServer once the engine has started
// (the main loop startup callback; the RenderingServer singleton does not exist yet at the SCENE
// initialization level) and unregisters both before the engine shuts down or the library unloads.
// The static get_counters() of each class returns that instance's counters. SeamCopy's shader is
// created on the rendering thread at the first _process() and freed in the shutdown callback.
//
// Virtual methods go through get_virtual_call_data_func and call_virtual_with_data_func. Their
// arguments and return values use the pointer encoding of core/variant/method_ptrcall.h: bool is one
// byte, int is int64_t, float is double, and Ref<T> and Object * arrive as a pointer to the object
// pointer. A Ref<T> return is written into a Ref<T> slot, so the slot starts as NULL and the caller
// owns the reference it receives. Vector2 and Vector3 assume a single-precision build (the default).

#include "deps/gdextension_interface.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define SEAM_EXPORT __declspec(dllexport)
#else
#define SEAM_EXPORT __attribute__((visibility("default")))
#endif

// Opaque engine types. StringName, String and Dictionary are one pointer. A Variant is 24 bytes
// (40 in a double-precision build); this code only passes Variants by pointer.
typedef struct {
	void *data;
} SeamStringName;

typedef struct {
	void *data;
} SeamString;

typedef struct {
	void *data;
} SeamDictionary;

typedef struct {
	void *data;
} SeamArray;

typedef struct {
	uint64_t data[5];
} SeamVariant;

typedef struct {
	float x;
	float y;
} SeamVector2;

typedef struct {
	int32_t x;
	int32_t y;
} SeamVector2i;

// Method bind hashes, from `godot --dump-extension-api` (extension_api.json) of this engine.
#define HASH_RS_REGISTER_TEMPORAL_UPSCALER_EXTENSION 2580093078
#define HASH_RS_UNREGISTER_TEMPORAL_UPSCALER_EXTENSION 2580093078
#define HASH_RS_REGISTER_FRAME_GENERATION_EXTENSION 1388700651
#define HASH_RS_UNREGISTER_FRAME_GENERATION_EXTENSION 1388700651
#define HASH_RS_GET_RENDERING_DEVICE 1405107940
#define HASH_RS_FORCE_SYNC 3218959716
#define HASH_RD_TEXTURE_IS_VALID 3521089500
#define HASH_RD_SHADER_COMPILE_SPIRV_FROM_SOURCE 1178973306
#define HASH_RD_SHADER_CREATE_FROM_SPIRV 342949005
#define HASH_RD_COMPUTE_PIPELINE_CREATE 1448838280
#define HASH_RD_COMPUTE_LIST_BEGIN 994427514
#define HASH_RD_COMPUTE_LIST_BIND_COMPUTE_PIPELINE 4040184819
#define HASH_RD_COMPUTE_LIST_BIND_UNIFORM_SET 749655778
#define HASH_RD_COMPUTE_LIST_DISPATCH 4275841770
#define HASH_RD_COMPUTE_LIST_END 3218959716
#define HASH_RD_FREE_RID 2722037293
#define HASH_RD_SHADER_SOURCE_SET_STAGE_SOURCE 620821314
#define HASH_RD_SHADER_SPIRV_GET_STAGE_COMPILE_ERROR 3354920045
#define HASH_RD_UNIFORM_SET_UNIFORM_TYPE 1664894931
#define HASH_RD_UNIFORM_SET_BINDING 1286410249
#define HASH_RD_UNIFORM_ADD_ID 2722037293
#define HASH_UNIFORM_SET_CACHE_RD_GET_CACHE 658571723
#define HASH_ARRAY_PUSH_BACK 3316032543
#define HASH_RID_GETTER 2944877500
#define HASH_VECTOR2I_GETTER 3690982128
#define HASH_REFCOUNTED_UNREFERENCE 2240911060
#define HASH_OBJECT_NOTIFICATION 4023243586
#define HASH_UTILITY_PRINT 2648703342

// Interface functions.
static struct {
	GDExtensionInterfaceClassdbRegisterExtensionClass6 classdb_register_extension_class6;
	GDExtensionInterfaceClassdbRegisterExtensionClassMethod classdb_register_extension_class_method;
	GDExtensionInterfaceClassdbUnregisterExtensionClass classdb_unregister_extension_class;
	GDExtensionInterfaceClassdbConstructObject3 classdb_construct_object3;
	GDExtensionInterfaceClassdbGetMethodBind classdb_get_method_bind;
	GDExtensionInterfaceObjectSetInstance object_set_instance;
	GDExtensionInterfaceObjectMethodBindPtrcall object_method_bind_ptrcall;
	GDExtensionInterfaceObjectDestroy object_destroy;
	GDExtensionInterfaceGlobalGetSingleton global_get_singleton;
	GDExtensionInterfaceStringNameNewWithLatin1Chars string_name_new_with_latin1_chars;
	GDExtensionInterfaceStringNewWithLatin1Chars string_new_with_latin1_chars;
	GDExtensionInterfaceVariantDestroy variant_destroy;
	GDExtensionInterfaceVariantNewCopy variant_new_copy;
	GDExtensionInterfaceDictionaryOperatorIndex dictionary_operator_index;
	GDExtensionInterfacePrintError print_error;
	GDExtensionInterfaceRegisterMainLoopCallbacks register_main_loop_callbacks;
	GDExtensionInterfaceVariantGetPtrUtilityFunction variant_get_ptr_utility_function;
	GDExtensionInterfaceVariantGetPtrBuiltinMethod variant_get_ptr_builtin_method;
	GDExtensionInterfaceStringToUtf8Chars string_to_utf8_chars;
	GDExtensionPtrDestructor string_name_destroy;
	GDExtensionPtrDestructor string_destroy;
	GDExtensionPtrDestructor dictionary_destroy;
	GDExtensionPtrConstructor dictionary_construct;
	GDExtensionPtrDestructor array_destroy;
	GDExtensionPtrConstructor array_construct;
	GDExtensionPtrBuiltInMethod array_push_back;
	GDExtensionPtrOperatorEvaluator string_name_equal;
	GDExtensionVariantFromTypeConstructorFunc variant_from_int;
	GDExtensionVariantFromTypeConstructorFunc variant_from_string;
	GDExtensionVariantFromTypeConstructorFunc variant_from_dictionary;
	GDExtensionVariantFromTypeConstructorFunc variant_from_object;
	GDExtensionPtrUtilityFunction print;
} api;

// Method binds.
static struct {
	GDExtensionMethodBindPtr rs_register_temporal_upscaler_extension;
	GDExtensionMethodBindPtr rs_unregister_temporal_upscaler_extension;
	GDExtensionMethodBindPtr rs_register_frame_generation_extension;
	GDExtensionMethodBindPtr rs_unregister_frame_generation_extension;
	GDExtensionMethodBindPtr rs_get_rendering_device;
	GDExtensionMethodBindPtr rs_force_sync;
	GDExtensionMethodBindPtr rd_texture_is_valid;
	GDExtensionMethodBindPtr rd_shader_compile_spirv_from_source;
	GDExtensionMethodBindPtr rd_shader_create_from_spirv;
	GDExtensionMethodBindPtr rd_compute_pipeline_create;
	GDExtensionMethodBindPtr rd_compute_list_begin;
	GDExtensionMethodBindPtr rd_compute_list_bind_compute_pipeline;
	GDExtensionMethodBindPtr rd_compute_list_bind_uniform_set;
	GDExtensionMethodBindPtr rd_compute_list_dispatch;
	GDExtensionMethodBindPtr rd_compute_list_end;
	GDExtensionMethodBindPtr rd_free_rid;
	GDExtensionMethodBindPtr shader_source_set_stage_source;
	GDExtensionMethodBindPtr shader_spirv_get_stage_compile_error;
	GDExtensionMethodBindPtr uniform_set_uniform_type;
	GDExtensionMethodBindPtr uniform_set_binding;
	GDExtensionMethodBindPtr uniform_add_id;
	GDExtensionMethodBindPtr uniform_set_cache_get_cache;
	GDExtensionMethodBindPtr upscaler_inputs_get_color;
	GDExtensionMethodBindPtr upscaler_inputs_get_output;
	GDExtensionMethodBindPtr upscaler_inputs_get_internal_size;
	GDExtensionMethodBindPtr upscaler_inputs_get_output_size;
	GDExtensionMethodBindPtr fg_inputs_get_hudless_color;
	GDExtensionMethodBindPtr fg_inputs_get_ui_color;
	GDExtensionMethodBindPtr fg_inputs_get_depth;
	GDExtensionMethodBindPtr fg_inputs_get_velocity;
	GDExtensionMethodBindPtr refcounted_unreference;
	GDExtensionMethodBindPtr object_notification;
} binds;

typedef enum {
	VIRTUAL_COPY_GET_NAME,
	VIRTUAL_COPY_IS_AVAILABLE,
	VIRTUAL_COPY_GET_SCALE_RANGE,
	VIRTUAL_COPY_GET_MIP_BIAS_OFFSET,
	VIRTUAL_COPY_GET_JITTER_PHASE_COUNT,
	VIRTUAL_COPY_PROCESS,
	VIRTUAL_COPY_RESET_HISTORY,
	VIRTUAL_FG_GET_NAME,
	VIRTUAL_FG_IS_AVAILABLE,
	VIRTUAL_FG_SET_MODE,
	VIRTUAL_FG_ON_FRAME_INPUTS,
	VIRTUAL_FG_ON_PRESENT,
	VIRTUAL_FG_RESET,
	VIRTUAL_MAX,
} SeamVirtual;

typedef struct {
	const char *name;
	SeamVirtual id;
	SeamStringName string_name;
} SeamVirtualEntry;

// The returned call data points into this table, which lives until the library unloads.
static SeamVirtualEntry seam_virtuals[VIRTUAL_MAX] = {
	{ "_get_name", VIRTUAL_COPY_GET_NAME, { NULL } },
	{ "_is_available", VIRTUAL_COPY_IS_AVAILABLE, { NULL } },
	{ "_get_scale_range", VIRTUAL_COPY_GET_SCALE_RANGE, { NULL } },
	{ "_get_mip_bias_offset", VIRTUAL_COPY_GET_MIP_BIAS_OFFSET, { NULL } },
	{ "_get_jitter_phase_count", VIRTUAL_COPY_GET_JITTER_PHASE_COUNT, { NULL } },
	{ "_process", VIRTUAL_COPY_PROCESS, { NULL } },
	{ "_reset_history", VIRTUAL_COPY_RESET_HISTORY, { NULL } },
	{ "_get_name", VIRTUAL_FG_GET_NAME, { NULL } },
	{ "_is_available", VIRTUAL_FG_IS_AVAILABLE, { NULL } },
	{ "_set_mode", VIRTUAL_FG_SET_MODE, { NULL } },
	{ "_on_frame_inputs", VIRTUAL_FG_ON_FRAME_INPUTS, { NULL } },
	{ "_on_present", VIRTUAL_FG_ON_PRESENT, { NULL } },
	{ "_reset", VIRTUAL_FG_RESET, { NULL } },
};

typedef enum {
	SEAM_CLASS_COPY,
	SEAM_CLASS_PASSTHROUGH,
	SEAM_CLASS_MAX,
} SeamClass;

typedef struct {
	const char *name;
	const char *parent;
	const char *provider_name;
	SeamVirtual first_virtual;
	SeamVirtual end_virtual;
	SeamStringName class_name;
	SeamStringName parent_name;
} SeamClassInfo;

static SeamClassInfo seam_classes[SEAM_CLASS_MAX] = {
	{ "SeamCopy", "TemporalUpscalerExtension", "SeamCopy", VIRTUAL_COPY_GET_NAME, VIRTUAL_FG_GET_NAME, { NULL }, { NULL } },
	{ "SeamPassthrough", "FrameGenerationExtension", "SeamPassthrough", VIRTUAL_FG_GET_NAME, VIRTUAL_MAX, { NULL }, { NULL } },
};

typedef struct {
	SeamClass kind;
	GDExtensionObjectPtr owner;
	int64_t name_calls;
	int64_t available_calls;
	// SeamCopy.
	int64_t process_calls;
	int64_t copies;
	int64_t copy_errors;
	int64_t reset_history_calls;
	int64_t last_width;
	int64_t last_height;
	// SeamPassthrough.
	int64_t mode_calls;
	int64_t inputs_calls;
	int64_t reset_calls;
	int64_t invalid_hudless;
	int64_t invalid_ui;
	int64_t invalid_depth;
	int64_t invalid_velocity;
	int64_t present_before;
	int64_t present_after;
	int64_t present_order_errors;
	bool in_present;
} SeamInstance;

static GDExtensionClassLibraryPtr seam_library = NULL;
static bool seam_binds_ok = false;
static bool seam_classes_registered = false;
static int64_t seam_live_instances = 0;
static SeamStringName seam_rendering_server_name;
static SeamStringName seam_get_counters_name;
// The library-owned instances; each holds the reference classdb_construct_object3 returned.
static GDExtensionObjectPtr seam_registered[SEAM_CLASS_MAX] = { NULL, NULL };
// Their extension instances (the counters), and the instance seam_create_instance made last.
static SeamInstance *seam_owned[SEAM_CLASS_MAX] = { NULL, NULL };
static SeamInstance *seam_last_created = NULL;

// SeamCopy's compute copy. The shader and its pipeline are RenderingDevice resources: created on the
// rendering thread at the first _process(), freed in the shutdown callback once no frame uses them.
// The colour input and the output are RGBA16F storage images in Forward+.
static const char *SEAM_COPY_SHADER =
		"#version 450\n"
		"layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n"
		"layout(rgba16f, set = 0, binding = 0) uniform restrict readonly image2D source_color;\n"
		"layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D destination_color;\n"
		"void main() {\n"
		"\tivec2 pixel = ivec2(gl_GlobalInvocationID.xy);\n"
		"\tif (any(greaterThanEqual(pixel, imageSize(destination_color)))) {\n"
		"\t\treturn;\n"
		"\t}\n"
		"\timageStore(destination_color, pixel, imageLoad(source_color, pixel));\n"
		"}\n";
// RenderingDevice enums.
#define SEAM_SHADER_STAGE_COMPUTE 4
#define SEAM_UNIFORM_TYPE_IMAGE 3
#define SEAM_COMPUTE_QUEUE_MAIN 0
static uint64_t seam_copy_shader = 0;
static uint64_t seam_copy_pipeline = 0;
static bool seam_copy_pipeline_failed = false;

static void seam_error(const char *p_message) {
	api.print_error(p_message, "vendor_seam", __FILE__, __LINE__, false);
}

// Prints through the engine's print(), so the line lands in the engine log in order.
static void seam_print(const char *p_message) {
	if (api.print == NULL) {
		printf("%s\n", p_message);
		fflush(stdout);
		return;
	}
	SeamString text;
	SeamVariant argument;
	api.string_new_with_latin1_chars(&text, p_message);
	api.variant_from_string(&argument, &text);
	const GDExtensionConstTypePtr args[1] = { &argument };
	api.print(NULL, args, 1);
	api.variant_destroy(&argument);
	api.string_destroy(&text);
}

static void seam_string_name(SeamStringName *r_name, const char *p_text) {
	api.string_name_new_with_latin1_chars(r_name, p_text, false);
}

static bool seam_string_name_equal(GDExtensionConstStringNamePtr p_a, const SeamStringName *p_b) {
	GDExtensionBool equal = 0;
	api.string_name_equal(p_a, p_b, &equal);
	return equal != 0;
}

static GDExtensionMethodBindPtr seam_bind(const char *p_class, const char *p_method, GDExtensionInt p_hash) {
	SeamStringName class_name;
	SeamStringName method_name;
	seam_string_name(&class_name, p_class);
	seam_string_name(&method_name, p_method);
	GDExtensionMethodBindPtr bind = api.classdb_get_method_bind(&class_name, &method_name, p_hash);
	api.string_name_destroy(&method_name);
	api.string_name_destroy(&class_name);
	if (bind == NULL) {
		char message[256];
		snprintf(message, sizeof(message), "SeamCopy/SeamPassthrough: no method bind %s.%s with hash %lld.", p_class, p_method, (long long)p_hash);
		seam_error(message);
		seam_binds_ok = false;
	}
	return bind;
}

static GDExtensionObjectPtr seam_rendering_server(void) {
	return api.global_get_singleton(&seam_rendering_server_name);
}

static GDExtensionObjectPtr seam_rendering_device(void) {
	GDExtensionObjectPtr rendering_server = seam_rendering_server();
	GDExtensionObjectPtr rendering_device = NULL;
	if (rendering_server != NULL) {
		api.object_method_bind_ptrcall(binds.rs_get_rendering_device, rendering_server, NULL, &rendering_device);
	}
	return rendering_device;
}

static uint64_t seam_get_rid(GDExtensionMethodBindPtr p_getter, GDExtensionObjectPtr p_inputs) {
	uint64_t rid = 0;
	api.object_method_bind_ptrcall(p_getter, p_inputs, NULL, &rid);
	return rid;
}

static bool seam_texture_is_valid(GDExtensionObjectPtr p_rendering_device, uint64_t p_rid) {
	if (p_rendering_device == NULL || p_rid == 0) {
		return false;
	}
	GDExtensionBool valid = 0;
	const GDExtensionConstTypePtr args[1] = { &p_rid };
	api.object_method_bind_ptrcall(binds.rd_texture_is_valid, p_rendering_device, args, &valid);
	return valid != 0;
}

// Calls a RenderingServer method that takes one Ref<> argument.
static void seam_call_with_object(GDExtensionMethodBindPtr p_bind, GDExtensionObjectPtr p_object) {
	GDExtensionObjectPtr rendering_server = seam_rendering_server();
	if (rendering_server == NULL || p_object == NULL) {
		return;
	}
	const GDExtensionConstTypePtr args[1] = { &p_object };
	api.object_method_bind_ptrcall(p_bind, rendering_server, args, NULL);
}

// Drops the reference this library holds, freeing the object when it was the last one.
static void seam_release(GDExtensionObjectPtr *r_object) {
	if (*r_object == NULL) {
		return;
	}
	GDExtensionBool last = 0;
	api.object_method_bind_ptrcall(binds.refcounted_unreference, *r_object, NULL, &last);
	if (last) {
		api.object_destroy(*r_object);
	}
	*r_object = NULL;
}

static void seam_return_string(GDExtensionTypePtr r_ret, const char *p_text) {
	// r_ret is a constructed (empty) String.
	api.string_destroy(r_ret);
	api.string_new_with_latin1_chars(r_ret, p_text);
}

// SeamCopy

static void seam_notify_postinitialize(GDExtensionObjectPtr p_object);

// Constructs an engine RefCounted; the caller owns its reference (seam_release()).
static GDExtensionObjectPtr seam_construct(const char *p_class) {
	SeamStringName class_name;
	seam_string_name(&class_name, p_class);
	GDExtensionObjectPtr object = api.classdb_construct_object3(&class_name);
	api.string_name_destroy(&class_name);
	if (object != NULL) {
		seam_notify_postinitialize(object);
	}
	return object;
}

// Logs the compute stage's compile error, if any; returns whether there was one.
static bool seam_compile_failed(GDExtensionObjectPtr p_spirv) {
	const int64_t stage = SEAM_SHADER_STAGE_COMPUTE;
	const GDExtensionConstTypePtr args[1] = { &stage };
	// A String return is assigned into a constructed (here empty) String.
	SeamString error = { NULL };
	api.object_method_bind_ptrcall(binds.shader_spirv_get_stage_compile_error, p_spirv, args, &error);
	const GDExtensionInt length = api.string_to_utf8_chars(&error, NULL, 0);
	if (length > 0) {
		// string_to_utf8_chars() returns the whole length and writes at most the limit, unterminated.
		char text[512];
		const GDExtensionInt limit = length < (GDExtensionInt)sizeof(text) - 1 ? length : (GDExtensionInt)sizeof(text) - 1;
		api.string_to_utf8_chars(&error, text, limit);
		text[limit] = '\0';
		seam_error(text);
	}
	api.string_destroy(&error);
	return length > 0;
}

// Creates the copy shader and its compute pipeline once. A failure is logged once, and every later
// _process() counts a copy error.
static bool seam_copy_ensure_pipeline(GDExtensionObjectPtr p_rendering_device) {
	if (seam_copy_pipeline != 0) {
		return true;
	}
	if (seam_copy_pipeline_failed) {
		return false;
	}
	seam_copy_pipeline_failed = true;

	GDExtensionObjectPtr source = seam_construct("RDShaderSource");
	if (source == NULL) {
		seam_error("SeamCopy: cannot create an RDShaderSource.");
		return false;
	}
	{
		const int64_t stage = SEAM_SHADER_STAGE_COMPUTE;
		SeamString text;
		api.string_new_with_latin1_chars(&text, SEAM_COPY_SHADER);
		const GDExtensionConstTypePtr args[2] = { &stage, &text };
		api.object_method_bind_ptrcall(binds.shader_source_set_stage_source, source, args, NULL);
		api.string_destroy(&text);
	}
	GDExtensionObjectPtr spirv = NULL;
	{
		const GDExtensionBool allow_cache = 1;
		const GDExtensionConstTypePtr args[2] = { &source, &allow_cache };
		api.object_method_bind_ptrcall(binds.rd_shader_compile_spirv_from_source, p_rendering_device, args, &spirv);
	}
	seam_release(&source);
	if (spirv == NULL || seam_compile_failed(spirv)) {
		seam_error("SeamCopy: the copy shader did not compile.");
		seam_release(&spirv);
		return false;
	}
	{
		SeamString name;
		api.string_new_with_latin1_chars(&name, "SeamCopy");
		const GDExtensionConstTypePtr args[2] = { &spirv, &name };
		api.object_method_bind_ptrcall(binds.rd_shader_create_from_spirv, p_rendering_device, args, &seam_copy_shader);
		api.string_destroy(&name);
	}
	seam_release(&spirv);
	if (seam_copy_shader == 0) {
		seam_error("SeamCopy: cannot create the copy shader.");
		return false;
	}
	{
		// No specialization constants.
		SeamArray constants;
		api.array_construct(&constants, NULL);
		const GDExtensionConstTypePtr args[2] = { &seam_copy_shader, &constants };
		api.object_method_bind_ptrcall(binds.rd_compute_pipeline_create, p_rendering_device, args, &seam_copy_pipeline);
		api.array_destroy(&constants);
	}
	if (seam_copy_pipeline == 0) {
		seam_error("SeamCopy: cannot create the copy pipeline.");
		return false;
	}
	seam_copy_pipeline_failed = false;
	return true;
}

// Appends an RDUniform for one storage image to p_uniforms, which then holds its only reference.
static void seam_push_image_uniform(SeamArray *p_uniforms, int64_t p_binding, uint64_t p_texture) {
	GDExtensionObjectPtr uniform = seam_construct("RDUniform");
	if (uniform == NULL) {
		return;
	}
	const int64_t type = SEAM_UNIFORM_TYPE_IMAGE;
	const GDExtensionConstTypePtr type_args[1] = { &type };
	const GDExtensionConstTypePtr binding_args[1] = { &p_binding };
	const GDExtensionConstTypePtr id_args[1] = { &p_texture };
	api.object_method_bind_ptrcall(binds.uniform_set_uniform_type, uniform, type_args, NULL);
	api.object_method_bind_ptrcall(binds.uniform_set_binding, uniform, binding_args, NULL);
	api.object_method_bind_ptrcall(binds.uniform_add_id, uniform, id_args, NULL);
	SeamVariant value;
	api.variant_from_object(&value, &uniform);
	const GDExtensionConstTypePtr push_args[1] = { &value };
	api.array_push_back(p_uniforms, push_args, NULL, 1);
	api.variant_destroy(&value);
	seam_release(&uniform);
}

static void seam_copy_process(SeamInstance *p_self, GDExtensionObjectPtr p_inputs) {
	p_self->process_calls++;
	if (p_inputs == NULL) {
		p_self->copy_errors++;
		return;
	}
	const uint64_t color = seam_get_rid(binds.upscaler_inputs_get_color, p_inputs);
	const uint64_t output = seam_get_rid(binds.upscaler_inputs_get_output, p_inputs);
	SeamVector2i internal_size = { 0, 0 };
	SeamVector2i output_size = { 0, 0 };
	api.object_method_bind_ptrcall(binds.upscaler_inputs_get_internal_size, p_inputs, NULL, &internal_size);
	api.object_method_bind_ptrcall(binds.upscaler_inputs_get_output_size, p_inputs, NULL, &output_size);
	p_self->last_width = output_size.x;
	p_self->last_height = output_size.y;
	GDExtensionObjectPtr rendering_device = seam_rendering_device();
	// The scale range is exactly 1.0, so the colour input and the output have the same size.
	if (rendering_device == NULL || color == 0 || output == 0 || internal_size.x <= 0 || internal_size.y <= 0 || internal_size.x != output_size.x || internal_size.y != output_size.y) {
		p_self->copy_errors++;
		return;
	}
	if (!seam_copy_ensure_pipeline(rendering_device)) {
		p_self->copy_errors++;
		return;
	}

	// The input textures are the engine's and change when the viewport is reconfigured, so the
	// uniform set is looked up each frame. UniformSetCacheRD frees it with the textures or the shader.
	SeamArray uniforms;
	api.array_construct(&uniforms, NULL);
	seam_push_image_uniform(&uniforms, 0, color);
	seam_push_image_uniform(&uniforms, 1, output);
	const int64_t set = 0;
	uint64_t uniform_set = 0;
	{
		const GDExtensionConstTypePtr args[3] = { &seam_copy_shader, &set, &uniforms };
		// A static method: no instance.
		api.object_method_bind_ptrcall(binds.uniform_set_cache_get_cache, NULL, args, &uniform_set);
	}
	api.array_destroy(&uniforms);
	if (uniform_set == 0) {
		p_self->copy_errors++;
		return;
	}

	const int64_t queue = SEAM_COMPUTE_QUEUE_MAIN;
	int64_t list = 0;
	const GDExtensionConstTypePtr begin_args[1] = { &queue };
	api.object_method_bind_ptrcall(binds.rd_compute_list_begin, rendering_device, begin_args, &list);
	const GDExtensionConstTypePtr pipeline_args[2] = { &list, &seam_copy_pipeline };
	api.object_method_bind_ptrcall(binds.rd_compute_list_bind_compute_pipeline, rendering_device, pipeline_args, NULL);
	const GDExtensionConstTypePtr set_args[3] = { &list, &uniform_set, &set };
	api.object_method_bind_ptrcall(binds.rd_compute_list_bind_uniform_set, rendering_device, set_args, NULL);
	const int64_t groups_x = (internal_size.x + 7) / 8;
	const int64_t groups_y = (internal_size.y + 7) / 8;
	const int64_t groups_z = 1;
	const GDExtensionConstTypePtr dispatch_args[4] = { &list, &groups_x, &groups_y, &groups_z };
	api.object_method_bind_ptrcall(binds.rd_compute_list_dispatch, rendering_device, dispatch_args, NULL);
	api.object_method_bind_ptrcall(binds.rd_compute_list_end, rendering_device, NULL, NULL);
	p_self->copies++;
}

// Frees the copy shader, which also frees its pipeline and the uniform sets made for it.
static void seam_copy_free_pipeline(void) {
	GDExtensionObjectPtr rendering_device = seam_rendering_device();
	if (seam_copy_shader != 0 && rendering_device != NULL) {
		const GDExtensionConstTypePtr args[1] = { &seam_copy_shader };
		api.object_method_bind_ptrcall(binds.rd_free_rid, rendering_device, args, NULL);
	}
	seam_copy_shader = 0;
	seam_copy_pipeline = 0;
	seam_copy_pipeline_failed = false;
}

// SeamPassthrough

static void seam_passthrough_frame_inputs(SeamInstance *p_self, GDExtensionObjectPtr p_inputs) {
	p_self->inputs_calls++;
	GDExtensionObjectPtr rendering_device = seam_rendering_device();
	if (p_inputs == NULL || !seam_texture_is_valid(rendering_device, seam_get_rid(binds.fg_inputs_get_hudless_color, p_inputs))) {
		p_self->invalid_hudless++;
	}
	if (p_inputs == NULL || !seam_texture_is_valid(rendering_device, seam_get_rid(binds.fg_inputs_get_ui_color, p_inputs))) {
		p_self->invalid_ui++;
	}
	if (p_inputs == NULL || !seam_texture_is_valid(rendering_device, seam_get_rid(binds.fg_inputs_get_depth, p_inputs))) {
		p_self->invalid_depth++;
	}
	// Velocity exists while a temporal upscaler runs (here SeamCopy).
	if (p_inputs == NULL || !seam_texture_is_valid(rendering_device, seam_get_rid(binds.fg_inputs_get_velocity, p_inputs))) {
		p_self->invalid_velocity++;
	}
}

static void seam_passthrough_present(SeamInstance *p_self, bool p_before) {
	if (p_before) {
		if (p_self->in_present) {
			p_self->present_order_errors++;
		}
		p_self->in_present = true;
		p_self->present_before++;
	} else {
		if (!p_self->in_present) {
			p_self->present_order_errors++;
		}
		p_self->in_present = false;
		p_self->present_after++;
	}
}

// Virtual dispatch

static void *seam_get_virtual_call_data(void *p_class_userdata, GDExtensionConstStringNamePtr p_name, uint32_t p_hash) {
	// p_hash identifies the virtual's signature; this sample matches by name only.
	(void)p_hash;
	const SeamClassInfo *info = (const SeamClassInfo *)p_class_userdata;
	for (int i = (int)info->first_virtual; i < (int)info->end_virtual; i++) {
		if (seam_string_name_equal(p_name, &seam_virtuals[i].string_name)) {
			return &seam_virtuals[i];
		}
	}
	return NULL;
}

static void seam_call_virtual_with_data(GDExtensionClassInstancePtr p_instance, GDExtensionConstStringNamePtr p_name, void *p_virtual_call_userdata, const GDExtensionConstTypePtr *p_args, GDExtensionTypePtr r_ret) {
	(void)p_name;
	SeamInstance *self = (SeamInstance *)p_instance;
	const SeamVirtualEntry *entry = (const SeamVirtualEntry *)p_virtual_call_userdata;
	switch (entry->id) {
		case VIRTUAL_COPY_GET_NAME:
		case VIRTUAL_FG_GET_NAME:
			self->name_calls++;
			seam_return_string(r_ret, seam_classes[self->kind].provider_name);
			break;
		case VIRTUAL_COPY_IS_AVAILABLE:
			self->available_calls++;
			*(GDExtensionBool *)r_ret = 1;
			break;
		case VIRTUAL_COPY_GET_SCALE_RANGE: {
			const SeamVector2 range = { 1.0f, 1.0f };
			memcpy(r_ret, &range, sizeof(range));
		} break;
		case VIRTUAL_COPY_GET_MIP_BIAS_OFFSET:
			*(double *)r_ret = 0.0;
			break;
		case VIRTUAL_COPY_GET_JITTER_PHASE_COUNT:
			// 0 phases: no jitter.
			*(int64_t *)r_ret = 0;
			break;
		case VIRTUAL_COPY_PROCESS:
			seam_copy_process(self, *(const GDExtensionObjectPtr *)p_args[0]);
			break;
		case VIRTUAL_COPY_RESET_HISTORY:
			self->reset_history_calls++;
			break;
		case VIRTUAL_FG_IS_AVAILABLE:
			self->available_calls++;
			// FrameGenerationExtension.REASON_OK.
			*(int64_t *)r_ret = 0;
			break;
		case VIRTUAL_FG_SET_MODE:
			self->mode_calls++;
			break;
		case VIRTUAL_FG_ON_FRAME_INPUTS:
			seam_passthrough_frame_inputs(self, *(const GDExtensionObjectPtr *)p_args[0]);
			break;
		case VIRTUAL_FG_ON_PRESENT:
			seam_passthrough_present(self, *(const GDExtensionBool *)p_args[1] != 0);
			break;
		case VIRTUAL_FG_RESET:
			self->reset_calls++;
			break;
		case VIRTUAL_MAX:
			break;
	}
}

// Instances

// classdb_construct_object3() leaves NOTIFICATION_POSTINITIALIZE to its caller.
static void seam_notify_postinitialize(GDExtensionObjectPtr p_object) {
	const int64_t what = 0; // Object.NOTIFICATION_POSTINITIALIZE
	const GDExtensionBool reversed = 0;
	const GDExtensionConstTypePtr args[2] = { &what, &reversed };
	api.object_method_bind_ptrcall(binds.object_notification, p_object, args, NULL);
}

static GDExtensionObjectPtr seam_create_instance(void *p_class_userdata, GDExtensionBool p_notify_postinitialize) {
	const SeamClassInfo *info = (const SeamClassInfo *)p_class_userdata;
	SeamInstance *self = (SeamInstance *)calloc(1, sizeof(SeamInstance));
	if (self == NULL) {
		seam_error("SeamCopy/SeamPassthrough: out of memory.");
		return NULL;
	}
	// The engine parent object; a RefCounted, so it arrives with one reference, which the caller takes.
	GDExtensionObjectPtr object = api.classdb_construct_object3(&info->parent_name);
	self->kind = (SeamClass)(info - seam_classes);
	self->owner = object;
	api.object_set_instance(object, &info->class_name, self);
	seam_live_instances++;
	seam_last_created = self;
	if (p_notify_postinitialize) {
		seam_notify_postinitialize(object);
	}
	return object;
}

static void seam_free_instance(void *p_class_userdata, GDExtensionClassInstancePtr p_instance) {
	(void)p_class_userdata;
	for (int i = 0; i < SEAM_CLASS_MAX; i++) {
		if (seam_owned[i] == p_instance) {
			seam_owned[i] = NULL;
		}
	}
	if (seam_last_created == p_instance) {
		seam_last_created = NULL;
	}
	free(p_instance);
	seam_live_instances--;
}

// get_counters(): a static method returning the library-owned instance's counters.

static void seam_dictionary_set(SeamDictionary *p_dictionary, const char *p_key, int64_t p_value) {
	SeamString key_text;
	SeamVariant key;
	SeamVariant value;
	api.string_new_with_latin1_chars(&key_text, p_key);
	api.variant_from_string(&key, &key_text);
	api.variant_from_int(&value, &p_value);
	// The index operator inserts a Nil Variant for a new key.
	GDExtensionVariantPtr slot = api.dictionary_operator_index(p_dictionary, &key);
	api.variant_new_copy(slot, &value);
	api.variant_destroy(&value);
	api.variant_destroy(&key);
	api.string_destroy(&key_text);
}

// Constructs the dictionary in r_dictionary, which must be uninitialized.
static void seam_build_counters(SeamClass p_kind, GDExtensionUninitializedTypePtr r_dictionary) {
	api.dictionary_construct(r_dictionary, NULL);
	SeamDictionary *dictionary = (SeamDictionary *)r_dictionary;
	const SeamInstance none = { 0 };
	const SeamInstance *self = seam_owned[p_kind] != NULL ? seam_owned[p_kind] : &none;
	seam_dictionary_set(dictionary, "registered", seam_owned[p_kind] != NULL ? 1 : 0);
	seam_dictionary_set(dictionary, "live_instances", seam_live_instances);
	seam_dictionary_set(dictionary, "name_calls", self->name_calls);
	seam_dictionary_set(dictionary, "available_calls", self->available_calls);
	if (p_kind == SEAM_CLASS_COPY) {
		seam_dictionary_set(dictionary, "process_calls", self->process_calls);
		seam_dictionary_set(dictionary, "copies", self->copies);
		seam_dictionary_set(dictionary, "copy_errors", self->copy_errors);
		seam_dictionary_set(dictionary, "reset_history_calls", self->reset_history_calls);
		seam_dictionary_set(dictionary, "last_width", self->last_width);
		seam_dictionary_set(dictionary, "last_height", self->last_height);
	} else {
		seam_dictionary_set(dictionary, "mode_calls", self->mode_calls);
		seam_dictionary_set(dictionary, "inputs_calls", self->inputs_calls);
		seam_dictionary_set(dictionary, "reset_calls", self->reset_calls);
		seam_dictionary_set(dictionary, "invalid_hudless", self->invalid_hudless);
		seam_dictionary_set(dictionary, "invalid_ui", self->invalid_ui);
		seam_dictionary_set(dictionary, "invalid_depth", self->invalid_depth);
		seam_dictionary_set(dictionary, "invalid_velocity", self->invalid_velocity);
		seam_dictionary_set(dictionary, "present_before", self->present_before);
		seam_dictionary_set(dictionary, "present_after", self->present_after);
		seam_dictionary_set(dictionary, "present_order_errors", self->present_order_errors);
	}
}

static void seam_counters_call(void *p_method_userdata, GDExtensionClassInstancePtr p_instance, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	(void)p_instance;
	(void)p_args;
	if (p_argument_count != 0) {
		r_error->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error->argument = 0;
		r_error->expected = 0;
		return;
	}
	SeamDictionary dictionary;
	seam_build_counters(*(const SeamClass *)p_method_userdata, &dictionary);
	api.variant_from_dictionary(r_return, &dictionary);
	api.dictionary_destroy(&dictionary);
	r_error->error = GDEXTENSION_CALL_OK;
}

static void seam_counters_ptrcall(void *p_method_userdata, GDExtensionClassInstancePtr p_instance, const GDExtensionConstTypePtr *p_args, GDExtensionTypePtr r_ret) {
	(void)p_instance;
	(void)p_args;
	// r_ret is a constructed Dictionary: replace it.
	api.dictionary_destroy(r_ret);
	seam_build_counters(*(const SeamClass *)p_method_userdata, r_ret);
}

// Registration

static void seam_register_class(SeamClassInfo *p_info, SeamClass *p_kind_userdata) {
	GDExtensionClassCreationInfo6 creation_info;
	memset(&creation_info, 0, sizeof(creation_info));
	creation_info.is_exposed = 1;
	creation_info.create_instance_func = seam_create_instance;
	creation_info.free_instance_func = seam_free_instance;
	creation_info.get_virtual_call_data_func = seam_get_virtual_call_data;
	creation_info.call_virtual_with_data_func = seam_call_virtual_with_data;
	creation_info.class_userdata = p_info;
	api.classdb_register_extension_class6(seam_library, &p_info->class_name, &p_info->parent_name, &creation_info);

	// static Dictionary get_counters()
	SeamStringName empty_name;
	SeamString empty_text;
	seam_string_name(&empty_name, "");
	api.string_new_with_latin1_chars(&empty_text, "");
	GDExtensionPropertyInfo return_info;
	memset(&return_info, 0, sizeof(return_info));
	return_info.type = GDEXTENSION_VARIANT_TYPE_DICTIONARY;
	return_info.name = &empty_name;
	return_info.class_name = &empty_name;
	return_info.hint_string = &empty_text;
	// PROPERTY_USAGE_DEFAULT.
	return_info.usage = 6;
	GDExtensionClassMethodInfo method_info;
	memset(&method_info, 0, sizeof(method_info));
	method_info.name = &seam_get_counters_name;
	method_info.method_userdata = p_kind_userdata;
	method_info.call_func = seam_counters_call;
	method_info.ptrcall_func = seam_counters_ptrcall;
	method_info.method_flags = GDEXTENSION_METHOD_FLAG_NORMAL | GDEXTENSION_METHOD_FLAG_STATIC;
	method_info.has_return_value = 1;
	method_info.return_value_info = &return_info;
	method_info.return_value_metadata = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
	api.classdb_register_extension_class_method(seam_library, &p_info->class_name, &method_info);
	api.string_destroy(&empty_text);
	api.string_name_destroy(&empty_name);
}

static SeamClass seam_kind_userdata[SEAM_CLASS_MAX] = { SEAM_CLASS_COPY, SEAM_CLASS_PASSTHROUGH };

static void seam_load_binds(void) {
	seam_binds_ok = true;
	binds.rs_register_temporal_upscaler_extension = seam_bind("RenderingServer", "register_temporal_upscaler_extension", HASH_RS_REGISTER_TEMPORAL_UPSCALER_EXTENSION);
	binds.rs_unregister_temporal_upscaler_extension = seam_bind("RenderingServer", "unregister_temporal_upscaler_extension", HASH_RS_UNREGISTER_TEMPORAL_UPSCALER_EXTENSION);
	binds.rs_register_frame_generation_extension = seam_bind("RenderingServer", "register_frame_generation_extension", HASH_RS_REGISTER_FRAME_GENERATION_EXTENSION);
	binds.rs_unregister_frame_generation_extension = seam_bind("RenderingServer", "unregister_frame_generation_extension", HASH_RS_UNREGISTER_FRAME_GENERATION_EXTENSION);
	binds.rs_get_rendering_device = seam_bind("RenderingServer", "get_rendering_device", HASH_RS_GET_RENDERING_DEVICE);
	binds.rs_force_sync = seam_bind("RenderingServer", "force_sync", HASH_RS_FORCE_SYNC);
	binds.rd_texture_is_valid = seam_bind("RenderingDevice", "texture_is_valid", HASH_RD_TEXTURE_IS_VALID);
	binds.rd_shader_compile_spirv_from_source = seam_bind("RenderingDevice", "shader_compile_spirv_from_source", HASH_RD_SHADER_COMPILE_SPIRV_FROM_SOURCE);
	binds.rd_shader_create_from_spirv = seam_bind("RenderingDevice", "shader_create_from_spirv", HASH_RD_SHADER_CREATE_FROM_SPIRV);
	binds.rd_compute_pipeline_create = seam_bind("RenderingDevice", "compute_pipeline_create", HASH_RD_COMPUTE_PIPELINE_CREATE);
	binds.rd_compute_list_begin = seam_bind("RenderingDevice", "compute_list_begin", HASH_RD_COMPUTE_LIST_BEGIN);
	binds.rd_compute_list_bind_compute_pipeline = seam_bind("RenderingDevice", "compute_list_bind_compute_pipeline", HASH_RD_COMPUTE_LIST_BIND_COMPUTE_PIPELINE);
	binds.rd_compute_list_bind_uniform_set = seam_bind("RenderingDevice", "compute_list_bind_uniform_set", HASH_RD_COMPUTE_LIST_BIND_UNIFORM_SET);
	binds.rd_compute_list_dispatch = seam_bind("RenderingDevice", "compute_list_dispatch", HASH_RD_COMPUTE_LIST_DISPATCH);
	binds.rd_compute_list_end = seam_bind("RenderingDevice", "compute_list_end", HASH_RD_COMPUTE_LIST_END);
	binds.rd_free_rid = seam_bind("RenderingDevice", "free_rid", HASH_RD_FREE_RID);
	binds.shader_source_set_stage_source = seam_bind("RDShaderSource", "set_stage_source", HASH_RD_SHADER_SOURCE_SET_STAGE_SOURCE);
	binds.shader_spirv_get_stage_compile_error = seam_bind("RDShaderSPIRV", "get_stage_compile_error", HASH_RD_SHADER_SPIRV_GET_STAGE_COMPILE_ERROR);
	binds.uniform_set_uniform_type = seam_bind("RDUniform", "set_uniform_type", HASH_RD_UNIFORM_SET_UNIFORM_TYPE);
	binds.uniform_set_binding = seam_bind("RDUniform", "set_binding", HASH_RD_UNIFORM_SET_BINDING);
	binds.uniform_add_id = seam_bind("RDUniform", "add_id", HASH_RD_UNIFORM_ADD_ID);
	binds.uniform_set_cache_get_cache = seam_bind("UniformSetCacheRD", "get_cache", HASH_UNIFORM_SET_CACHE_RD_GET_CACHE);
	binds.upscaler_inputs_get_color = seam_bind("TemporalUpscalerInputs", "get_color", HASH_RID_GETTER);
	binds.upscaler_inputs_get_output = seam_bind("TemporalUpscalerInputs", "get_output", HASH_RID_GETTER);
	binds.upscaler_inputs_get_internal_size = seam_bind("TemporalUpscalerInputs", "get_internal_size", HASH_VECTOR2I_GETTER);
	binds.upscaler_inputs_get_output_size = seam_bind("TemporalUpscalerInputs", "get_output_size", HASH_VECTOR2I_GETTER);
	binds.fg_inputs_get_hudless_color = seam_bind("FrameGenerationInputs", "get_hudless_color", HASH_RID_GETTER);
	binds.fg_inputs_get_ui_color = seam_bind("FrameGenerationInputs", "get_ui_color", HASH_RID_GETTER);
	binds.fg_inputs_get_depth = seam_bind("FrameGenerationInputs", "get_depth", HASH_RID_GETTER);
	binds.fg_inputs_get_velocity = seam_bind("FrameGenerationInputs", "get_velocity", HASH_RID_GETTER);
	binds.refcounted_unreference = seam_bind("RefCounted", "unreference", HASH_REFCOUNTED_UNREFERENCE);
	binds.object_notification = seam_bind("Object", "notification", HASH_OBJECT_NOTIFICATION);
}

static void seam_initialize(void *p_userdata, GDExtensionInitializationLevel p_level) {
	(void)p_userdata;
	if (p_level != GDEXTENSION_INITIALIZATION_SCENE) {
		return;
	}
	seam_string_name(&seam_rendering_server_name, "RenderingServer");
	seam_string_name(&seam_get_counters_name, "get_counters");
	for (int i = 0; i < VIRTUAL_MAX; i++) {
		seam_string_name(&seam_virtuals[i].string_name, seam_virtuals[i].name);
	}
	SeamStringName print_name;
	seam_string_name(&print_name, "print");
	api.print = api.variant_get_ptr_utility_function(&print_name, HASH_UTILITY_PRINT);
	api.string_name_destroy(&print_name);
	SeamStringName push_back_name;
	seam_string_name(&push_back_name, "push_back");
	api.array_push_back = api.variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_ARRAY, &push_back_name, HASH_ARRAY_PUSH_BACK);
	api.string_name_destroy(&push_back_name);

	seam_load_binds();
	if (api.array_push_back == NULL) {
		seam_error("SeamCopy/SeamPassthrough: the engine lacks Array.push_back with the expected hash.");
		seam_binds_ok = false;
	}
	if (!seam_binds_ok) {
		seam_error("SeamCopy/SeamPassthrough: the engine lacks a method this sample needs; the classes are not registered.");
		return;
	}
	for (int i = 0; i < SEAM_CLASS_MAX; i++) {
		seam_string_name(&seam_classes[i].class_name, seam_classes[i].name);
		seam_string_name(&seam_classes[i].parent_name, seam_classes[i].parent);
		seam_register_class(&seam_classes[i], &seam_kind_userdata[i]);
	}
	seam_classes_registered = true;
}

static void seam_deinitialize(void *p_userdata, GDExtensionInitializationLevel p_level) {
	(void)p_userdata;
	if (p_level != GDEXTENSION_INITIALIZATION_SCENE) {
		return;
	}
	if (seam_classes_registered) {
		// The shutdown callback already released this library's instances.
		char message[128];
		snprintf(message, sizeof(message), "VENDOR_SEAM_DEINIT live_instances=%lld", (long long)seam_live_instances);
		seam_print(message);
		if (seam_live_instances != 0) {
			seam_error("SeamCopy/SeamPassthrough: instances are still alive while the library unloads.");
		}
		for (int i = SEAM_CLASS_MAX - 1; i >= 0; i--) {
			api.classdb_unregister_extension_class(seam_library, &seam_classes[i].class_name);
			api.string_name_destroy(&seam_classes[i].parent_name);
			api.string_name_destroy(&seam_classes[i].class_name);
		}
		seam_classes_registered = false;
	}
	for (int i = 0; i < VIRTUAL_MAX; i++) {
		api.string_name_destroy(&seam_virtuals[i].string_name);
	}
	api.string_name_destroy(&seam_get_counters_name);
	api.string_name_destroy(&seam_rendering_server_name);
	seam_library = NULL;
}

// Main loop callbacks: the RenderingServer singleton exists from startup until shutdown.

static void seam_startup(void) {
	if (!seam_classes_registered || seam_registered[SEAM_CLASS_COPY] != NULL || seam_registered[SEAM_CLASS_PASSTHROUGH] != NULL) {
		return;
	}
	if (seam_rendering_server() == NULL) {
		seam_error("SeamCopy/SeamPassthrough: no RenderingServer; the providers are not registered.");
		return;
	}
	for (int i = 0; i < SEAM_CLASS_MAX; i++) {
		seam_last_created = NULL;
		seam_registered[i] = api.classdb_construct_object3(&seam_classes[i].class_name);
		seam_owned[i] = seam_last_created;
		if (seam_registered[i] != NULL) {
			seam_notify_postinitialize(seam_registered[i]);
		}
	}
	// The RenderingServer keeps its own references; it calls _get_name() here.
	seam_call_with_object(binds.rs_register_temporal_upscaler_extension, seam_registered[SEAM_CLASS_COPY]);
	seam_call_with_object(binds.rs_register_frame_generation_extension, seam_registered[SEAM_CLASS_PASSTHROUGH]);
	char message[128];
	snprintf(message, sizeof(message), "VENDOR_SEAM_STARTUP registered=%d live_instances=%lld", (seam_registered[SEAM_CLASS_COPY] != NULL) + (seam_registered[SEAM_CLASS_PASSTHROUGH] != NULL), (long long)seam_live_instances);
	seam_print(message);
}

static void seam_shutdown(void) {
	if (!seam_classes_registered) {
		return;
	}
	GDExtensionObjectPtr rendering_server = seam_rendering_server();
	if (rendering_server != NULL) {
		seam_call_with_object(binds.rs_unregister_temporal_upscaler_extension, seam_registered[SEAM_CLASS_COPY]);
		seam_call_with_object(binds.rs_unregister_frame_generation_extension, seam_registered[SEAM_CLASS_PASSTHROUGH]);
		// With a separate render thread the unregister calls are queued: apply them before the
		// last references go, so no instance outlives its class.
		api.object_method_bind_ptrcall(binds.rs_force_sync, rendering_server, NULL, NULL);
	}
	// No frame calls SeamCopy any more. RenderingDevice frees the shader once the GPU is done with it.
	seam_copy_free_pipeline();
	for (int i = 0; i < SEAM_CLASS_MAX; i++) {
		seam_owned[i] = NULL;
		seam_release(&seam_registered[i]);
	}
	char message[128];
	snprintf(message, sizeof(message), "VENDOR_SEAM_SHUTDOWN live_instances=%lld", (long long)seam_live_instances);
	seam_print(message);
}

static const GDExtensionMainLoopCallbacks seam_main_loop_callbacks = {
	seam_startup,
	seam_shutdown,
	NULL,
};

// p_get_proc_address returns void (*)(); GCC's -Wcast-function-type accepts a cast through void (*)(void).
typedef void (*SeamAnyFunction)(void);
#define SEAM_FUNCTION(m_type, m_pointer) ((m_type)(SeamAnyFunction)(m_pointer))

#define SEAM_LOAD(m_name, m_type) \
	api.m_name = SEAM_FUNCTION(m_type, p_get_proc_address(#m_name)); \
	if (api.m_name == NULL) { \
		fprintf(stderr, "vendor_seam: no interface function %s\n", #m_name); \
		return false; \
	}

static bool seam_load_interface(GDExtensionInterfaceGetProcAddress p_get_proc_address) {
	SEAM_LOAD(classdb_register_extension_class6, GDExtensionInterfaceClassdbRegisterExtensionClass6);
	SEAM_LOAD(classdb_register_extension_class_method, GDExtensionInterfaceClassdbRegisterExtensionClassMethod);
	SEAM_LOAD(classdb_unregister_extension_class, GDExtensionInterfaceClassdbUnregisterExtensionClass);
	SEAM_LOAD(classdb_construct_object3, GDExtensionInterfaceClassdbConstructObject3);
	SEAM_LOAD(classdb_get_method_bind, GDExtensionInterfaceClassdbGetMethodBind);
	SEAM_LOAD(object_set_instance, GDExtensionInterfaceObjectSetInstance);
	SEAM_LOAD(object_method_bind_ptrcall, GDExtensionInterfaceObjectMethodBindPtrcall);
	SEAM_LOAD(object_destroy, GDExtensionInterfaceObjectDestroy);
	SEAM_LOAD(global_get_singleton, GDExtensionInterfaceGlobalGetSingleton);
	SEAM_LOAD(string_name_new_with_latin1_chars, GDExtensionInterfaceStringNameNewWithLatin1Chars);
	SEAM_LOAD(string_new_with_latin1_chars, GDExtensionInterfaceStringNewWithLatin1Chars);
	SEAM_LOAD(variant_destroy, GDExtensionInterfaceVariantDestroy);
	SEAM_LOAD(variant_new_copy, GDExtensionInterfaceVariantNewCopy);
	SEAM_LOAD(dictionary_operator_index, GDExtensionInterfaceDictionaryOperatorIndex);
	SEAM_LOAD(print_error, GDExtensionInterfacePrintError);
	SEAM_LOAD(register_main_loop_callbacks, GDExtensionInterfaceRegisterMainLoopCallbacks);
	SEAM_LOAD(variant_get_ptr_utility_function, GDExtensionInterfaceVariantGetPtrUtilityFunction);
	SEAM_LOAD(variant_get_ptr_builtin_method, GDExtensionInterfaceVariantGetPtrBuiltinMethod);
	SEAM_LOAD(string_to_utf8_chars, GDExtensionInterfaceStringToUtf8Chars);

	GDExtensionInterfaceVariantGetPtrDestructor get_destructor = SEAM_FUNCTION(GDExtensionInterfaceVariantGetPtrDestructor, p_get_proc_address("variant_get_ptr_destructor"));
	GDExtensionInterfaceVariantGetPtrConstructor get_constructor = SEAM_FUNCTION(GDExtensionInterfaceVariantGetPtrConstructor, p_get_proc_address("variant_get_ptr_constructor"));
	GDExtensionInterfaceVariantGetPtrOperatorEvaluator get_operator = SEAM_FUNCTION(GDExtensionInterfaceVariantGetPtrOperatorEvaluator, p_get_proc_address("variant_get_ptr_operator_evaluator"));
	GDExtensionInterfaceGetVariantFromTypeConstructor get_variant_from = SEAM_FUNCTION(GDExtensionInterfaceGetVariantFromTypeConstructor, p_get_proc_address("get_variant_from_type_constructor"));
	if (get_destructor == NULL || get_constructor == NULL || get_operator == NULL || get_variant_from == NULL) {
		fprintf(stderr, "vendor_seam: missing Variant interface functions\n");
		return false;
	}
	api.string_name_destroy = get_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME);
	api.string_destroy = get_destructor(GDEXTENSION_VARIANT_TYPE_STRING);
	api.dictionary_destroy = get_destructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY);
	api.dictionary_construct = get_constructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0);
	api.array_destroy = get_destructor(GDEXTENSION_VARIANT_TYPE_ARRAY);
	api.array_construct = get_constructor(GDEXTENSION_VARIANT_TYPE_ARRAY, 0);
	api.string_name_equal = get_operator(GDEXTENSION_VARIANT_OP_EQUAL, GDEXTENSION_VARIANT_TYPE_STRING_NAME, GDEXTENSION_VARIANT_TYPE_STRING_NAME);
	api.variant_from_int = get_variant_from(GDEXTENSION_VARIANT_TYPE_INT);
	api.variant_from_string = get_variant_from(GDEXTENSION_VARIANT_TYPE_STRING);
	api.variant_from_dictionary = get_variant_from(GDEXTENSION_VARIANT_TYPE_DICTIONARY);
	api.variant_from_object = get_variant_from(GDEXTENSION_VARIANT_TYPE_OBJECT);
	return api.string_name_destroy != NULL && api.string_destroy != NULL && api.dictionary_destroy != NULL && api.dictionary_construct != NULL && api.array_destroy != NULL && api.array_construct != NULL && api.string_name_equal != NULL && api.variant_from_int != NULL && api.variant_from_string != NULL && api.variant_from_dictionary != NULL && api.variant_from_object != NULL;
}

SEAM_EXPORT GDExtensionBool vendor_seam_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	// Two .gdextension files that name this binary share one copy of it and its static variables: only
	// the first load registers the classes and the providers.
	if (seam_library != NULL) {
		fprintf(stderr, "vendor_seam: the library is already loaded by another .gdextension file; not loading it again\n");
		return false;
	}
	if (!seam_load_interface(p_get_proc_address)) {
		return false;
	}
	seam_library = p_library;
	api.register_main_loop_callbacks(p_library, &seam_main_loop_callbacks);
	r_initialization->initialize = seam_initialize;
	r_initialization->deinitialize = seam_deinitialize;
	r_initialization->userdata = NULL;
	r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	return true;
}
