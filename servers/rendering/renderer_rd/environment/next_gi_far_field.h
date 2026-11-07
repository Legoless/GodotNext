/**************************************************************************/
/*  next_gi_far_field.h                                                   */
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

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_types.h"

// GodotNext: pl_far_field (#126, plan §21.2 L8).
// Device-free cell selection, proxy simplification and visibility rules.
struct NextGIFarSource {
	RID mesh;
	Transform3D transform;
	AABB bounds;
	Vector<RID> materials;
	RSE::FarFieldMode mode = RSE::FAR_FIELD_AUTO;
	float visibility_end = 0.0f;
	uint64_t revision = 0;
};

class RenderDataRD;
class RenderGeometryInstance;
namespace RendererRD {
class NextGIRTScene;
struct NextGIInstanceRecord;
struct NextGISurfaceRecord;
} //namespace RendererRD

class NextGIFarField {
public:
	static constexpr float CELL_SIZE = 64.0f;
	static constexpr float DEFAULT_DISTANCE = 1000.0f;
	static constexpr uint64_t MAX_PROXY_BYTES = 128ull * 1024ull * 1024ull;
	static constexpr uint32_t MAX_CELL_TRIANGLES = 128 * 1024;

	using Source = NextGIFarSource;

	struct ProxySurface {
		LocalVector<float> positions;
		LocalVector<float> normals;
		LocalVector<float> uvs;
		LocalVector<uint32_t> indices;
		RID material;
		uint64_t bytes() const;
	};

	static Vector3i cell_coordinate(const Vector3 &p_position);
	static Transform3D cell_transform(const Vector3i &p_cell);
	static bool include_source(const Source &p_source, bool p_visible, bool p_gi_contributing, bool p_opaque, bool p_skinned, bool p_dynamic, float p_near_distance);
	static bool intersects_shell(const AABB &p_bounds, const Vector3 &p_camera, float p_near_distance, float p_far_distance);
	static float occlusion_visibility(bool p_hit);
	static uint64_t fingerprint(const Source &p_source);
	static bool simplify_surface(ProxySurface &r_surface, uint32_t p_target_index_count, float p_error);

	struct CaptureSource {
		RenderGeometryInstance *instance = nullptr;
		RID mesh;
		Transform3D transform;
		float angular_radius = 0.0f;
	};

	struct Stats {
		uint32_t cell_count = 0;
		uint32_t pending_count = 0;
		uint32_t tlas_build_count = 0;
		uint64_t proxy_bytes = 0;
		bool budget_exceeded = false;
	};

	void init(RendererRD::NextGIRTScene *p_owner);
	void update(const RenderDataRD *p_render_data, LocalVector<RendererRD::NextGIInstanceRecord> &r_records, LocalVector<RendererRD::NextGISurfaceRecord> &r_surfaces);
	void free();
	void capture_sources(LocalVector<CaptureSource> &r_sources) const;
	bool is_trace_ready() const { return tlas_ready && !budget_exceeded; }
	RID get_tlas() const { return tlas; }
	const Stats &get_stats() const { return stats; }

private:
	struct Work {
		Vector3i coordinate;
		uint64_t fingerprint = 0;
		LocalVector<Source> sources;
		LocalVector<ProxySurface> surfaces;
		WorkerThreadPool::TaskID task = 0;
		uint64_t bytes = 0;
	};

	struct Cell {
		Vector3i coordinate;
		uint64_t fingerprint = 0;
		AABB bounds;
		RID mesh;
		RID blas;
		RenderGeometryInstance *instance = nullptr;
		uint64_t bytes = 0;
		uint64_t geometry_bytes = 0;
		bool building = false;
		bool compacted = false;
		bool failed = false;
	};

	RendererRD::NextGIRTScene *owner = nullptr;
	HashMap<uint64_t, Cell> cells;
	LocalVector<Work *> pending_work;
	LocalVector<Work *> completed_work;
	Mutex completed_mutex;
	RID tlas;
	bool tlas_ready = false;
	bool tlas_dirty = true;
	bool budget_exceeded = false;
	uint64_t proxy_bytes = 0;
	Vector3 camera;
	Vector3i camera_cell = Vector3i(INT32_MIN, INT32_MIN, INT32_MIN);
	uint32_t last_record_base = 0;
	uint32_t last_tlas_count = 0;
	Stats stats;

	static uint64_t _cell_key(const Vector3i &p_cell);
	void _build_work(void *p_userdata);
	void _collect_geometry(const Source &p_source, const Vector3i &p_cell, LocalVector<ProxySurface> &r_surfaces);
	void _finish_work(Work *p_work);
	void _release_cell(Cell &r_cell);
	void _update_tlas(const Vector3 &p_camera, float p_near_distance, float p_far_distance, LocalVector<RendererRD::NextGIInstanceRecord> &r_records, LocalVector<RendererRD::NextGISurfaceRecord> &r_surfaces);
};
