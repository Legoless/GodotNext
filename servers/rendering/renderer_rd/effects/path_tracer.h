/**************************************************************************/
/*  path_tracer.h                                                         */
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
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

class NextGIHitMaterials;
class NextGIRTScene;

// GodotNext: path_tracer (#167, plan §21.0.6). The product path tracer: a progressive Viewport mode
// (Viewport.path_tracing_enabled, or the VIEWPORT_DEBUG_DRAW_PATH_TRACER debug draw) that traces the shared
// NextGI TLAS and accumulates one sample per pixel each frame into an RGBA32F history at the viewport's
// output size, until Viewport.path_tracing_samples. Its estimator is the D16 reference tracer's
// (nextgi_reference_pt_inc.glsl: the BRDF, the light units, Russian roulette from bounce 4), and the D16
// material and light tables are built here for both: NextGIReference (debug/reference_mode, the harness's
// --reference=pt) builds its tables through PathTracer::build_reference_tables() as it always did.
//
// Two backends trace the same paths:
// - HIT_GROUPS, where the #130 material hit groups run for it (NextGI::get_hit_availability_for() with
//   NEXTGI_HIT_CONSUMER_PATH_TRACER: hardware_ray_tracing_mode Full, #286): a ray generation stage of the hit
//   pipeline finds each hit with inline ray query and evaluates its material through the hit group (a
//   closest-hit retrace of the same ray), so textures, normal maps and material code shade the hits.
//   Fallback-group hits (a shader still compiling, an ineligible material, a geometry without an attribute
//   stream) take the D16 table material.
// - INLINE, everywhere else the hardware tier runs (no RT pipelines, Apple8 without Metal ray shaders, or
//   hardware_ray_tracing_mode Simplified Material or Inline, whose pipelines have no material groups): a
//   compute shader with inline ray query and the D16 table materials (StandardMaterial3D parameters, no
//   textures).
// The product's table holds the parameters' linear values, as the material shaders read them; the D16
// reference's keeps the raw parameter values it always had (debug/path_tracer_reference_match uses it too).
// Camera rays come from the view's projection (perspective, orthogonal and off-axis frustum cameras), and the
// random numbers of a path are decorrelated across pixels and samples; the reference match keeps the D16
// reference's camera block and random numbers.
// Camera and bounce rays skip the hits a material's cull mode culls (cull_back hides back faces, cull_front
// front faces, cull_disabled neither), as UE culls the backfaces of one-sided materials; shadow rays are not
// culled. NextGeo meshes are traced through whatever the TLAS holds: their RT fallback by default, the native
// sets when rendering/nextgeo/ray_tracing_mode is Native (Experimental), as UE's path tracer does.
//
// Everything above PathTracer is device-free so doctests run it without a GPU.

// Russian roulette starts at this bounce, as in D16 (nextgi_reference_pt_inc.glsl).
static constexpr int PATH_TRACER_ROULETTE_FROM = 4;
// Culled hits a ray goes through before it gives up (a ray that crosses more one-sided surfaces from behind
// stops there, as if it had left the scene).
static constexpr int PATH_TRACER_MAX_CULL_SKIPS = 8;

// The settings of a view. The limits and their validation are RenderingServerTypes::path_tracing_*(), shared
// with Viewport and RendererViewport.
struct PathTracerSettings {
	bool enabled = false;
	int samples = RenderingServerTypes::PATH_TRACING_DEFAULT_SAMPLES;
	int max_bounces = RenderingServerTypes::PATH_TRACING_DEFAULT_MAX_BOUNCES;
	float firefly_clamp = 0.0f;
	// GodotNext: path_tracer_denoise (#168). Off, built-in screen-space JNLM, or optional OIDN module.
	RSE::ViewportPathTracingDenoiser denoiser = RenderingServerTypes::PATH_TRACING_DEFAULT_DENOISER;
	bool denoise_requested = false;
	// debug/path_tracer_reference_match: trace as the D16 reference does (the INLINE backend with the D16 table
	// materials, pixel centers, no culling), so the product's history can be compared with the reference's.
	bool reference_match = false;
};

// GodotNext: path_tracer_denoise (#168). Denoiser status reported in get_path_tracing_info().
enum PathTracerDenoiseStatus {
	PATH_TRACER_DENOISE_STATUS_OFF,
	PATH_TRACER_DENOISE_STATUS_PENDING, // Denoiser is enabled, accumulating; runs on convergence or on demand.
	PATH_TRACER_DENOISE_STATUS_DENOISED, // Denoised image is current and composited by the requested backend.
	PATH_TRACER_DENOISE_STATUS_FALLBACK_BUILTIN, // OIDN was requested, unavailable or failed; fell back to built-in JNLM.
	PATH_TRACER_DENOISE_STATUS_MAX,
};
const char *path_tracer_denoise_status_name(PathTracerDenoiseStatus p_status);
const char *path_tracer_denoiser_name(RSE::ViewportPathTracingDenoiser p_denoiser);

// Parameters of the built-in screen-space JNLM denoiser (adapted from LightmapperRD::_denoise).
static constexpr int PATH_TRACER_DENOISE_MAX_HALF_SEARCH = 5;
struct PathTracerDenoiseParams {
	float spatial_bandwidth = 5.0f;
	float light_bandwidth = 0.1f;
	float albedo_bandwidth = 0.2f;
	float normal_bandwidth = 0.1f;
	float filter_strength = 10.0f;
	int half_search_window = 5;
};
PathTracerDenoiseParams path_tracer_validate_denoise_params(const PathTracerDenoiseParams &p_params);

// Pure scheduling rule: returns true when denoising should run on this frame (at convergence or on demand,
// whenever the current sample count or requested denoiser mode has not yet been denoised).
bool path_tracer_should_denoise(RSE::ViewportPathTracingDenoiser p_denoiser, uint32_t p_samples, uint32_t p_target, bool p_on_demand, bool p_denoised_valid, uint32_t p_denoised_samples, RSE::ViewportPathTracingDenoiser p_denoised_requested);

// Optional OIDN denoiser seam registered by modules/oidn at MODULE_INITIALIZATION_LEVEL_SERVERS (§19).
// Inert (null) in default MIT builds where modules/oidn is disabled.
struct PathTracerOIDNInput {
	uint32_t width = 0;
	uint32_t height = 0;
	// Tightly packed RGB32F buffers (width * height * 3 floats): linear HDR radiance, [0, 1] first-hit albedo
	// (optional), [-1, 1] first-hit unit normal (optional), and output denoised RGB32F radiance.
	const float *color = nullptr;
	const float *albedo = nullptr;
	const float *normal = nullptr;
	float *output = nullptr;
	bool hdr = true;
	bool srgb = false;
	bool clean_aux = true;
};

struct PathTracerOIDNBackend {
	const char *name = nullptr; // "oidn" or "mock".
	Error (*denoise)(const PathTracerOIDNInput &p_input, String &r_error) = nullptr;
};

// Device-free RGBA32F accumulation/guide resolution into tightly packed RGB32F OIDN buffers, and repacking
// of denoised RGB32F back into RGBA32F bytes (alpha = 1.0, non-finite/negative values sanitized to 0.0).
bool path_tracer_prepare_oidn_buffers(uint32_t p_width, uint32_t p_height, const float *p_accum_rgba, const float *p_albedo_rgba, const float *p_normal_rgba, LocalVector<float> &r_color_rgb, LocalVector<float> &r_albedo_rgb, LocalVector<float> &r_normal_rgb);
void path_tracer_pack_denoised_rgba(uint32_t p_width, uint32_t p_height, const float *p_rgb, Vector<uint8_t> &r_rgba_bytes);

// The settings a viewport traces with: its own, or on while its debug draw is VIEWPORT_DEBUG_DRAW_PATH_TRACER.
PathTracerSettings path_tracer_effective_settings(const PathTracerSettings &p_settings, RSE::ViewportDebugDraw p_debug_draw);

enum PathTracerStatus {
	PATH_TRACER_STATUS_OFF,
	PATH_TRACER_STATUS_ACTIVE,
	PATH_TRACER_STATUS_CONVERGED, // The history holds the target sample count; nothing more is traced.
	PATH_TRACER_STATUS_NO_NEXTGI, // The environment's NextGI is off, so there is no shared TLAS.
	PATH_TRACER_STATUS_NOT_HWRT, // NextGI runs on another tier than hardware ray tracing.
	PATH_TRACER_STATUS_NO_TLAS, // No TLAS, or it is not traceable this frame (a BLAS it references was rebuilt after it).
	PATH_TRACER_STATUS_MULTIVIEW, // XR and other multiview rendering are not path traced.
	PATH_TRACER_STATUS_REFERENCE_MODE, // debug/reference_mode is on and owns the HDR color.
	PATH_TRACER_STATUS_UNSUPPORTED_DEVICE, // Metal before Apple8, or no ray query (path_tracer_device_support()).
	PATH_TRACER_STATUS_MAX,
};
const char *path_tracer_status_name(PathTracerStatus p_status);

enum PathTracerBackend {
	PATH_TRACER_BACKEND_NONE,
	PATH_TRACER_BACKEND_HIT_GROUPS,
	PATH_TRACER_BACKEND_INLINE,
	PATH_TRACER_BACKEND_MAX,
};
const char *path_tracer_backend_name(PathTracerBackend p_backend);

// HIT_GROUPS where the hit groups run this frame and their pipeline has the path tracer's ray generation
// stage; INLINE otherwise.
PathTracerBackend path_tracer_backend(bool p_hit_groups_traceable, bool p_hit_raygen);

// Where the path tracer runs (plan §21.0.6, §26.2): every device with ray query on Vulkan and D3D12; on Metal,
// Apple8 and later, labeled Experimental (inline ray query and the D16 table materials until Metal ray shaders,
// Apple9 and later, #172).
enum PathTracerDeviceSupport {
	PATH_TRACER_DEVICE_SUPPORTED,
	PATH_TRACER_DEVICE_EXPERIMENTAL,
	PATH_TRACER_DEVICE_NO_RAY_QUERY,
	PATH_TRACER_DEVICE_APPLE_FAMILY,
};
// p_apple_family is the Apple GPU family number (7 for Apple7) on Metal, ignored elsewhere.
PathTracerDeviceSupport path_tracer_device_support(bool p_metal, int p_apple_family, bool p_ray_query);
const char *path_tracer_device_support_name(PathTracerDeviceSupport p_support);
// Whether the path tracer runs on a device with that support (SUPPORTED or EXPERIMENTAL).
_FORCE_INLINE_ bool path_tracer_device_runs(PathTracerDeviceSupport p_support) {
	return p_support == PATH_TRACER_DEVICE_SUPPORTED || p_support == PATH_TRACER_DEVICE_EXPERIMENTAL;
}

// Whether a hit is culled: the ray goes on through it. cull_back culls back faces, cull_front front faces and
// cull_disabled none.
_FORCE_INLINE_ bool path_tracer_cull_skips_hit(RSE::CullMode p_cull_mode, bool p_front_face) {
	if (p_cull_mode == RSE::CULL_MODE_BACK) {
		return !p_front_face;
	}
	if (p_cull_mode == RSE::CULL_MODE_FRONT) {
		return p_front_face;
	}
	return false;
}

// The side of a triangle a ray hits, as the rasterizer decides it: Godot's front faces wind clockwise as seen
// from the viewer, so (p2 - p0) x (p1 - p0) points out of the front side in object space. The outward normal goes
// to world space through the inverse transpose, so a mirrored instance keeps its sides, as the rasterizer
// reverses its cull mode for mirrored instances. path_tracer_inc.glsl implements the same rule.
bool path_tracer_hit_is_front(const Vector3 &p_p0, const Vector3 &p_p1, const Vector3 &p_p2, const Basis &p_object_to_world, const Vector3 &p_ray_dir);

// Why the history restarted (bits). Within one frame of each: the change is seen on the frame it happens, and
// that frame's sample is the first one of the new history.
enum PathTracerResetReason : uint32_t {
	PATH_TRACER_RESET_HISTORY = 1u << 0, // No history yet: the first frame, a new size, or path tracing turned on.
	PATH_TRACER_RESET_SETTINGS = 1u << 1, // Max bounces, the firefly clamp (and the exposure it is in), the backend or the reference match.
	PATH_TRACER_RESET_CAMERA = 1u << 2, // The camera's transform or projection.
	PATH_TRACER_RESET_INSTANCES = 1u << 3, // NextGI dirty events: an instance moved, entered, left or was hidden.
	PATH_TRACER_RESET_LIGHTS = 1u << 4, // NextGI light events, or the view's lights (any type) were added, removed, moved or changed.
	PATH_TRACER_RESET_MATERIALS = 1u << 5, // A traced material changed: its parameters, its shader, which material a surface draws, or a global shader parameter its hit group reads.
	PATH_TRACER_RESET_SCENE = 1u << 6, // What the tracer reads changed: the TLAS records, a traced mesh's vertices or pose updated in place, the sky.
	PATH_TRACER_RESET_HIT_GROUPS = 1u << 7, // The hit pipeline was linked again.
};
static constexpr uint32_t PATH_TRACER_RESET_REASON_COUNT = 8;
const char *path_tracer_reset_reason_name(uint32_t p_reason_index);

// What one frame of a view looks like to the reset rule.
struct PathTracerFrameState {
	Size2i size;
	uint64_t settings = 0;
	uint64_t camera = 0;
	uint64_t scene = 0;
	uint64_t lights = 0;
	uint64_t material_serial = 0;
	uint64_t hit_groups_serial = 0;
	// This frame's NextGI events (RenderSDFGIUpdateData): instance dirty volumes, positional light volumes and
	// the directional-light flag.
	uint32_t instance_events = 0;
	uint32_t light_events = 0;
	bool directional_light_event = false;
};

// The reasons the history of p_current restarts, given the previous traced frame. 0 keeps accumulating.
// Raising or lowering the sample target is not a reset: the history continues, or stops at the new target.
uint32_t path_tracer_reset_reasons(bool p_has_history, const PathTracerFrameState &p_previous, const PathTracerFrameState &p_current);

uint64_t path_tracer_hash_bytes(uint64_t p_hash, const void *p_data, size_t p_size);
uint64_t path_tracer_hash_camera(const Transform3D &p_camera, const Projection &p_projection);
// p_exposure counts only while the firefly clamp is on: the clamp applies to the exposed radiance.
uint64_t path_tracer_hash_settings(const PathTracerSettings &p_settings, PathTracerBackend p_backend, float p_exposure = 1.0f);
// Every light of the view, of any type: its transform, color and parameters.
uint64_t path_tracer_hash_light(uint64_t p_hash, uint32_t p_type, const Transform3D &p_transform, const Color &p_color, const float *p_params, uint32_t p_param_count);

// Samples to add this frame: one, until the history holds p_target.
_FORCE_INLINE_ uint32_t path_tracer_samples_this_frame(uint32_t p_samples, uint32_t p_target) {
	return p_samples < p_target ? 1u : 0u;
}

// One D16 material table entry (NextGIRefMaterial in the shaders, 48 bytes).
struct PathTracerGPUMaterial {
	float albedo_metal[4] = { 0.8f, 0.8f, 0.8f, 0.0f };
	float emission_roughness[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	// specular, mode (0 Lambert, 1 Burley), cull mode (RSE::CullMode, read by the product only), unused.
	float specular_mode[4] = { 0.5f, 1.0f, float(RSE::CULL_MODE_BACK), 0.0f };
};
static_assert(sizeof(PathTracerGPUMaterial) == 48, "The D16 material table entry is 48 bytes (NextGIRefMaterial).");

// The D16 tables of one frame: a material per TLAS surface record, in record order, and up to 8 omni lights
// (12 floats each).
struct PathTracerTables {
	LocalVector<PathTracerGPUMaterial> materials;
	LocalVector<float> lights;
	uint32_t surface_count = 0;
	uint32_t light_count = 0;
	uint32_t textured_materials = 0; // Materials with an albedo or emission texture the table cannot hold.
	uint64_t hash = 0; // The instance transforms and the table bytes (#263's reset key).
};
static constexpr uint32_t PATH_TRACER_MAX_LIGHTS = 8;
static constexpr uint32_t PATH_TRACER_LIGHT_FLOATS = 12;

// The camera block both estimator shaders read (the first 64 bytes of their params).
struct PathTracerGPUCamera {
	float cam_origin_fov[4] = {};
	float cam_forward_aspect[4] = {};
	float cam_up[4] = {};
	float cam_right[4] = {};
};
PathTracerGPUCamera path_tracer_camera(const Transform3D &p_camera, const Projection &p_projection, const Size2i &p_size);

// The product's camera rays (path_tracer_inc.glsl): the world-space point on the near plane at the view's center
// and its change per unit of normalized device x and y, from the inverse projection, so orthogonal and off-axis
// frustum cameras trace what they rasterize. center.w is 1 for an orthogonal camera (rays start on the near plane
// and go forward) and 0 for a perspective one (rays start at the camera). dx.w and dy.w are the ray cone's width
// at its origin and its spread per unit of length, one pixel's (#130's texture level of detail).
struct PathTracerGPURays {
	float center[4] = {};
	float dx[4] = {};
	float dy[4] = {};
};
PathTracerGPURays path_tracer_rays(const Transform3D &p_camera, const Projection &p_projection, bool p_orthogonal, const Size2i &p_size);

// The per-view state, in the view's render buffers (RB_SCOPE_PATH_TRACER).
class PathTracerView : public RenderBufferCustomDataRD {
	GDCLASS(PathTracerView, RenderBufferCustomDataRD);

public:
	RID accumulation; // RGBA32F: the radiance sum and the sample count.
	// GodotNext: path_tracer_denoise (#168). First-hit guides and denoised linear radiance (RGBA32F, alpha = 1).
	RID guide_albedo; // RGBA32F: first-hit albedo sum and hit count.
	RID guide_normal; // RGBA32F: first-hit world normal sum and hit count.
	RID denoised; // RGBA32F: denoised linear mean radiance, allocated on demand when denoising runs.
	Size2i size;
	uint32_t samples = 0;
	uint32_t target = 0;
	uint32_t resets = 0;
	uint32_t last_reset_reasons = 0;
	uint32_t reset_counts[PATH_TRACER_RESET_REASON_COUNT] = {};
	uint64_t last_reset_frame = 0;
	uint64_t last_traced_frame = 0;
	uint64_t frames_traced = 0;
	PathTracerFrameState state;
	bool has_state = false;
	PathTracerStatus status = PATH_TRACER_STATUS_OFF;
	PathTracerBackend backend = PATH_TRACER_BACKEND_NONE;
	float exposure = 1.0f;
	uint32_t textured_materials = 0;
	PathTracerDeviceSupport device_support = PATH_TRACER_DEVICE_SUPPORTED;
	PathTracerSettings settings;
	// GodotNext: path_tracer_denoise (#168). Per-view denoising state.
	bool denoised_valid = false;
	bool pending_denoise_request = false;
	uint32_t denoised_samples = 0;
	RSE::ViewportPathTracingDenoiser denoised_requested = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
	RSE::ViewportPathTracingDenoiser denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
	PathTracerDenoiseStatus denoise_status = PATH_TRACER_DENOISE_STATUS_OFF;
	uint64_t denoise_runs = 0;
	// debug/path_tracer_readback: the raw history after the last traced frame, its mean luminance and how many
	// of its values are not finite.
	PackedByteArray read_accumulation;
	float read_mean = 0.0f;
	uint32_t read_nonfinite = 0;
	uint64_t read_hash = 0;
	// GodotNext: path_tracer_denoise (#168). Denoised output readback when debug/path_tracer_readback is on.
	PackedByteArray read_denoised;
	float read_denoised_mean = 0.0f;
	uint32_t read_denoised_nonfinite = 0;
	uint64_t read_denoised_hash = 0;

	void clear_history();
	void fill_info(Dictionary &r_info) const;

	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void free_data() override;

	~PathTracerView();
};

// The path tracer's shaders and frame work. Owned by NextGI; created on the hardware tier only.
class PathTracer {
	struct ShaderState;
	ShaderState *shader = nullptr;
	RID dummy_buffer;
	RID material_buffer;
	RID light_buffer;
	uint32_t material_capacity = 0;
	uint32_t light_capacity = 0;
	bool logged_compile_failure = false;
	bool logged_denoise_compile_failure = false;
	bool logged_oidn_fallback = false;

	bool _ensure_shader();
	bool _ensure_denoise_shader();
	void _upload_tables(const PathTracerTables &p_tables);
	void _trace_inline(PathTracerView *p_view, NextGIRTScene *p_rt_scene);
	void _update_denoise(PathTracerView *p_view);
	bool _denoise_builtin(PathTracerView *p_view);
	bool _denoise_oidn(PathTracerView *p_view);
	void _readback(PathTracerView *p_view);

public:
	// GodotNext: path_tracer_denoise (#168). Optional OIDN backend seam (modules/oidn).
	static void register_oidn_backend(const PathTracerOIDNBackend *p_backend);
	static void unregister_oidn_backend(const PathTracerOIDNBackend *p_backend);
	static bool is_oidn_available();
	static const char *get_oidn_backend_name();
	static Error run_oidn_denoise(const PathTracerOIDNInput &p_input, String &r_error);
	static Vector<String> get_denoise_shader_variant_defines();
	// The D16 reference's tables (NextGIReference, debug/reference_mode): the fed instances' parts that enter the
	// TLAS, walked as #27 and #263 walk them, so the reference images stay what the harness captured.
	static void build_reference_tables(const RenderDataRD *p_render_data, PathTracerTables &r_tables);
	// The product's tables: one entry per surface record of this frame's TLAS phase, from what the record draws.
	// p_linear_colors converts the color parameters to linear, as the material shaders read them (the product);
	// false keeps the D16 reference's raw values (the reference match).
	static void build_tables(const RenderDataRD *p_render_data, const NextGIRTScene *p_rt_scene, PathTracerTables &r_tables, bool p_linear_colors);
	// The view's PathTracerView, or null when the render buffers do not path trace.
	static PathTracerView *get_view(RenderSceneBuffersRD *p_render_buffers, bool p_create);
	// The view's settings, as RendererViewport set them on the render buffers.
	static PathTracerSettings get_settings(const RenderSceneBuffersRD *p_render_buffers);
	// Marks a view that wants path tracing but cannot have it this frame.
	static void set_status(RenderSceneBuffersRD *p_render_buffers, PathTracerStatus p_status);
	// This device's support (RenderingDevice's capabilities and ray query).
	static PathTracerDeviceSupport get_device_support();
	// The compute shader's variant defines, shared with the shader baker's bake-only instance.
	static Vector<String> get_shader_variant_defines();

	// Adds this frame's sample, after the TLAS phase and before compaction (NextGI::render_frame()).
	void render(const RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, NextGIHitMaterials *p_hit_materials);
	// Writes the history's mean times the exposure into the internal HDR color.
	void composite(RenderSceneBuffersRD *p_render_buffers, RID p_color);
	void free();

	PathTracer() {}
	~PathTracer();
};

} // namespace RendererRD
