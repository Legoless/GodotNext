/**************************************************************************/
/*  mesh_sdf_builder.cpp                                                  */
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

#include "mesh_sdf_builder.h"

#include "core/math/math_funcs.h"
#include "core/math/static_raycaster.h"
#include "core/math/triangle_mesh.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/local_vector.h"
#include "core/templates/sort_array.h"

struct MeshSDFFaceSort {
	uint32_t axis = 0;
	bool operator()(const MeshSDFFacePos &p_left, const MeshSDFFacePos &p_right) const {
		return p_left.center[axis] < p_right.center[axis];
	}
};

uint32_t MeshSDFBVH::create_bvh(LocalVector<MeshSDFBVHNode> &bvh_tree, MeshSDFFacePos *p_faces, uint32_t p_face_count, const Face3 *p_triangles, float p_thickness) {
	if (p_face_count == 1) {
		return MeshSDFBVHNode::LEAF_BIT | p_faces[0].index;
	}

	uint32_t index = bvh_tree.size();
	{
		MeshSDFBVHNode bvh;

		for (uint32_t i = 0; i < p_face_count; i++) {
			const Face3 &f = p_triangles[p_faces[i].index];
			AABB aabb(f.vertex[0], Vector3());
			aabb.expand_to(f.vertex[1]);
			aabb.expand_to(f.vertex[2]);
			if (p_thickness > 0.0) {
				Vector3 normal = p_triangles[p_faces[i].index].get_plane().normal;
				aabb.expand_to(f.vertex[0] - normal * p_thickness);
				aabb.expand_to(f.vertex[1] - normal * p_thickness);
				aabb.expand_to(f.vertex[2] - normal * p_thickness);
			}
			if (i == 0) {
				bvh.bounds = aabb;
			} else {
				bvh.bounds.merge_with(aabb);
			}
		}
		bvh_tree.push_back(bvh);
	}

	uint32_t middle = p_face_count / 2;

	SortArray<MeshSDFFacePos, MeshSDFFaceSort> s;
	s.compare.axis = bvh_tree[index].bounds.get_longest_axis_index();
	s.sort(p_faces, p_face_count);

	uint32_t left = create_bvh(bvh_tree, p_faces, middle, p_triangles, p_thickness);
	uint32_t right = create_bvh(bvh_tree, p_faces + middle, p_face_count - middle, p_triangles, p_thickness);

	bvh_tree[index].children[0] = left;
	bvh_tree[index].children[1] = right;

	return index;
}

static _FORCE_INLINE_ real_t Vector3_dot2(const Vector3 &p_vec3) {
	return p_vec3.dot(p_vec3);
}

void MeshSDFBVH::find_closest_distance(const Vector3 &p_pos, const MeshSDFBVHNode *p_bvh, uint32_t p_bvh_cell, const Face3 *p_triangles, float p_thickness, float &r_closest_distance) {
	if (p_bvh_cell & MeshSDFBVHNode::LEAF_BIT) {
		p_bvh_cell &= MeshSDFBVHNode::LEAF_MASK; //remove bit

		Vector3 point = p_pos;
		Plane p = p_triangles[p_bvh_cell].get_plane();
		float d = p.distance_to(point);
		float inside_d = 1e20;
		if (d < 0 && d > -p_thickness) {
			//inside planes, do this in 2D

			Vector3 x_axis = (p_triangles[p_bvh_cell].vertex[0] - p_triangles[p_bvh_cell].vertex[1]).normalized();
			Vector3 y_axis = p.normal.cross(x_axis).normalized();

			Vector2 points[3];
			for (int i = 0; i < 3; i++) {
				points[i] = Vector2(x_axis.dot(p_triangles[p_bvh_cell].vertex[i]), y_axis.dot(p_triangles[p_bvh_cell].vertex[i]));
			}

			Vector2 p2d = Vector2(x_axis.dot(point), y_axis.dot(point));

			{
				// https://www.shadertoy.com/view/XsXSz4

				Vector2 e0 = points[1] - points[0];
				Vector2 e1 = points[2] - points[1];
				Vector2 e2 = points[0] - points[2];

				Vector2 v0 = p2d - points[0];
				Vector2 v1 = p2d - points[1];
				Vector2 v2 = p2d - points[2];

				Vector2 pq0 = v0 - e0 * CLAMP(v0.dot(e0) / e0.dot(e0), 0.0, 1.0);
				Vector2 pq1 = v1 - e1 * CLAMP(v1.dot(e1) / e1.dot(e1), 0.0, 1.0);
				Vector2 pq2 = v2 - e2 * CLAMP(v2.dot(e2) / e2.dot(e2), 0.0, 1.0);

				float s = SIGN(e0.x * e2.y - e0.y * e2.x);
				Vector2 d2 = Vector2(pq0.dot(pq0), s * (v0.x * e0.y - v0.y * e0.x)).min(Vector2(pq1.dot(pq1), s * (v1.x * e1.y - v1.y * e1.x))).min(Vector2(pq2.dot(pq2), s * (v2.x * e2.y - v2.y * e2.x)));

				inside_d = -Math::sqrt(d2.x) * SIGN(d2.y);
			}

			//make sure distance to planes is not shorter if inside
			if (inside_d < 0) {
				inside_d = MAX(inside_d, d);
				inside_d = MAX(inside_d, -(p_thickness + d));
			}

			r_closest_distance = MIN(r_closest_distance, inside_d);
		} else {
			if (d < 0) {
				point -= p.normal * p_thickness; //flatten
			}

			// https://iquilezles.org/www/articles/distfunctions/distfunctions.htm
			Vector3 a = p_triangles[p_bvh_cell].vertex[0];
			Vector3 b = p_triangles[p_bvh_cell].vertex[1];
			Vector3 c = p_triangles[p_bvh_cell].vertex[2];

			Vector3 ba = b - a;
			Vector3 pa = point - a;
			Vector3 cb = c - b;
			Vector3 pb = point - b;
			Vector3 ac = a - c;
			Vector3 pc = point - c;
			Vector3 nor = ba.cross(ac);

			inside_d = Math::sqrt(
					(SIGN(ba.cross(nor).dot(pa)) + SIGN(cb.cross(nor).dot(pb)) + SIGN(ac.cross(nor).dot(pc)) < 2.0)
							? MIN(MIN(
										  Vector3_dot2(ba * CLAMP(ba.dot(pa) / Vector3_dot2(ba), 0.0, 1.0) - pa),
										  Vector3_dot2(cb * CLAMP(cb.dot(pb) / Vector3_dot2(cb), 0.0, 1.0) - pb)),
									  Vector3_dot2(ac * CLAMP(ac.dot(pc) / Vector3_dot2(ac), 0.0, 1.0) - pc))
							: nor.dot(pa) * nor.dot(pa) / Vector3_dot2(nor));

			r_closest_distance = MIN(r_closest_distance, inside_d);
		}

	} else {
		bool pass = true;
		if (!p_bvh[p_bvh_cell].bounds.has_point(p_pos)) {
			//outside, find closest point
			Vector3 he = p_bvh[p_bvh_cell].bounds.size * 0.5f;
			Vector3 center = p_bvh[p_bvh_cell].bounds.position + he;

			Vector3 rel = (p_pos - center).abs();
			Vector3 closest = rel.min(he);
			float d = rel.distance_to(closest);

			if (d >= r_closest_distance) {
				pass = false; //already closer than this aabb, discard
			}
		}

		if (pass) {
			find_closest_distance(p_pos, p_bvh, p_bvh[p_bvh_cell].children[0], p_triangles, p_thickness, r_closest_distance);
			find_closest_distance(p_pos, p_bvh, p_bvh[p_bvh_cell].children[1], p_triangles, p_thickness, r_closest_distance);
		}
	}
}

struct MeshSDFBuilder::ComputeJob {
	const MeshSDFBVHNode *bvh = nullptr;
	uint32_t bvh_root = 0;
	const Face3 *triangles = nullptr;
	int face_count = 0;
	StaticRaycaster *raycaster = nullptr;
	const TriangleMesh *triangle_mesh = nullptr;
	const Vector<Vector3> *directions = nullptr;
	Vector3i dimensions;
	Vector3 origin; // First voxel corner.
	float voxel_size = 0.0f;
	bool two_sided = false;
	float *cells = nullptr;
};

// Issue #184 step 1 sign rule: a voxel is inside when more than 25% of the
// ray hits are back faces. The hit must also cover a majority of the
// directions, which an open sheet can never do (it covers at most half the
// direction sphere from any point), so open geometry gains no false
// interior. On closed meshes the extra condition changes nothing: from
// inside, every ray hits a back face; from outside, every first hit is a
// front face.
bool MeshSDFBuilder::_is_inside_by_back_faces(const ComputeJob *p_job, const Vector3 &p_point) {
	const Vector<Vector3> &directions = *p_job->directions;
	int hits = 0;
	int back_faces = 0;
	for (int i = 0; i < directions.size(); i++) {
		const Vector3 &dir = directions[i];
		// Back-face classification uses the source face's normal, fetched by
		// prim/face index: TriangleMesh::intersect_ray flips its returned
		// normal toward the ray origin, and this does not rely on the
		// raycaster's hit-normal convention either.
		Vector3 hit_normal;
		bool hit = false;
		if (p_job->raycaster != nullptr) {
			StaticRaycaster::Ray ray(p_point, dir);
			hit = p_job->raycaster->intersect(ray);
			if (hit && ray.primID < uint32_t(p_job->face_count)) {
				hit_normal = p_job->triangles[ray.primID].get_plane().get_normal();
			}
		} else if (p_job->triangle_mesh != nullptr) {
			Vector3 hit_point;
			Vector3 oriented_normal;
			int32_t face_index = -1;
			hit = p_job->triangle_mesh->intersect_ray(p_point, dir, hit_point, oriented_normal, nullptr, &face_index);
			if (hit && face_index >= 0) {
				hit_normal = p_job->triangle_mesh->get_triangles()[face_index].normal;
			}
		}
		if (hit) {
			hits++;
			if (hit_normal.dot(dir) > 0.0f) {
				back_faces++;
			}
		}
	}
	return hits * 2 > directions.size() && back_faces * 4 > hits;
}

void MeshSDFBuilder::_compute_sdf_z(uint32_t p_z, ComputeJob *p_job) {
	const int z = int(p_z);
	const Vector3i &dims = p_job->dimensions;
	for (int y = 0; y < dims.y; y++) {
		for (int x = 0; x < dims.x; x++) {
			Vector3 pos = p_job->origin + (Vector3(x, y, z) + Vector3(0.5f, 0.5f, 0.5f)) * p_job->voxel_size;
			float distance = 1e20;
			MeshSDFBVH::find_closest_distance(pos, p_job->bvh, p_job->bvh_root, p_job->triangles, 0.0f, distance);
			if (!p_job->two_sided && distance > 0.0f && _is_inside_by_back_faces(p_job, pos)) {
				distance = -distance;
			}
			p_job->cells[x + dims.x * (y + dims.y * z)] = distance;
		}
	}
}

Vector<Vector3> MeshSDFBuilder::get_ray_directions(int p_ray_count) {
	Vector<Vector3> directions;
	ERR_FAIL_COND_V(p_ray_count <= 0, directions);
	directions.resize(p_ray_count);
	const double golden_angle = Math::PI * (3.0 - Math::sqrt(5.0));
	for (int i = 0; i < p_ray_count; i++) {
		double y = 1.0 - (double(i) / double(MAX(p_ray_count - 1, 1))) * 2.0;
		double radius = Math::sqrt(MAX(0.0, 1.0 - y * y));
		double theta = golden_angle * double(i);
		directions.write[i] = Vector3(Math::cos(theta) * radius, y, Math::sin(theta) * radius).normalized();
	}
	return directions;
}

static uint8_t _encode_distance(float p_distance, float p_band) {
	float normalized = CLAMP(p_distance / p_band, -1.0f, 1.0f) * 0.5f + 0.5f;
	return uint8_t(CLAMP(normalized * 255.0f + 0.5f, 0.0f, 255.0f));
}

static void _brickify(const Vector<float> &p_dense, const Vector3i &p_dimensions, float p_band, float p_allocate_threshold, PackedInt32Array &r_page_table, PackedByteArray &r_brick_data, Vector3i &r_brick_grid_dimensions) {
	const int bs = MeshSDFData::BRICK_SIZE;
	r_brick_grid_dimensions = Vector3i(
			(p_dimensions.x + bs - 1) / bs,
			(p_dimensions.y + bs - 1) / bs,
			(p_dimensions.z + bs - 1) / bs);
	const Vector3i &bg = r_brick_grid_dimensions;
	r_page_table.resize(bg.x * bg.y * bg.z);
	r_brick_data.clear();
	auto voxel_at = [&](int x, int y, int z) -> float {
		x = CLAMP(x, 0, p_dimensions.x - 1);
		y = CLAMP(y, 0, p_dimensions.y - 1);
		z = CLAMP(z, 0, p_dimensions.z - 1);
		return p_dense[x + p_dimensions.x * (y + p_dimensions.y * z)];
	};
	for (int bz = 0; bz < bg.z; bz++) {
		for (int by = 0; by < bg.y; by++) {
			for (int bx = 0; bx < bg.x; bx++) {
				float min_abs = Math::INF;
				for (int lz = 0; lz < bs; lz++) {
					for (int ly = 0; ly < bs; ly++) {
						for (int lx = 0; lx < bs; lx++) {
							int x = bx * bs + lx;
							int y = by * bs + ly;
							int z = bz * bs + lz;
							if (x >= p_dimensions.x || y >= p_dimensions.y || z >= p_dimensions.z) {
								continue;
							}
							min_abs = MIN(min_abs, Math::abs(voxel_at(x, y, z)));
						}
					}
				}
				int page_index = bx + bg.x * (by + bg.y * bz);
				if (min_abs > p_allocate_threshold) {
					// Uniformly far from the surface, so a single sign covers the brick.
					r_page_table.write[page_index] = voxel_at(bx * bs, by * bs, bz * bs) < 0.0f ? MeshSDFData::PAGE_FAR_INSIDE : MeshSDFData::PAGE_FAR_OUTSIDE;
					continue;
				}
				int brick_index = r_brick_data.size() / MeshSDFData::BRICK_VOXELS;
				r_page_table.write[page_index] = brick_index;
				int brick_offset = r_brick_data.size();
				r_brick_data.resize(brick_offset + MeshSDFData::BRICK_VOXELS);
				for (int lz = 0; lz < bs; lz++) {
					for (int ly = 0; ly < bs; ly++) {
						for (int lx = 0; lx < bs; lx++) {
							float d = voxel_at(bx * bs + lx, by * bs + ly, bz * bs + lz);
							r_brick_data.write[brick_offset + lx + bs * (ly + bs * lz)] = _encode_distance(d, p_band);
						}
					}
				}
			}
		}
	}
}

static Vector<float> _downsample(const Vector<float> &p_dense, const Vector3i &p_dimensions, Vector3i &r_dimensions) {
	r_dimensions = Vector3i((p_dimensions.x + 1) / 2, (p_dimensions.y + 1) / 2, (p_dimensions.z + 1) / 2);
	Vector<float> out;
	out.resize(r_dimensions.x * r_dimensions.y * r_dimensions.z);
	for (int z = 0; z < r_dimensions.z; z++) {
		for (int y = 0; y < r_dimensions.y; y++) {
			for (int x = 0; x < r_dimensions.x; x++) {
				float sum = 0.0f;
				int count = 0;
				for (int dz = 0; dz < 2; dz++) {
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							int sx = x * 2 + dx;
							int sy = y * 2 + dy;
							int sz = z * 2 + dz;
							if (sx >= p_dimensions.x || sy >= p_dimensions.y || sz >= p_dimensions.z) {
								continue;
							}
							sum += p_dense[sx + p_dimensions.x * (sy + p_dimensions.y * sz)];
							count++;
						}
					}
				}
				out.write[x + r_dimensions.x * (y + r_dimensions.y * z)] = count > 0 ? sum / float(count) : 0.0f;
			}
		}
	}
	return out;
}

Error MeshSDFBuilder::build(const Vector<Face3> &p_faces, const MeshSDFSettings &p_settings, MeshSDFData &r_data) {
	r_data = MeshSDFData();
	ERR_FAIL_COND_V_MSG(p_faces.is_empty(), ERR_INVALID_DATA, "Cannot build a mesh distance field without faces.");
	ERR_FAIL_COND_V_MSG(p_settings.voxels_per_meter <= 0.0f || p_settings.resolution_scale <= 0.0f, ERR_INVALID_PARAMETER, "Mesh distance field resolution must be positive.");

	Vector<Vector3> face_vertices;
	face_vertices.resize(p_faces.size() * 3);
	AABB aabb;
	for (int i = 0; i < p_faces.size(); i++) {
		for (int j = 0; j < 3; j++) {
			face_vertices.write[i * 3 + j] = p_faces[i].vertex[j];
			if (i == 0 && j == 0) {
				aabb.position = p_faces[i].vertex[j];
				aabb.size = Vector3();
			} else {
				aabb.expand_to(p_faces[i].vertex[j]);
			}
		}
	}

	LocalVector<MeshSDFFacePos> face_pos;
	face_pos.resize(p_faces.size());
	for (int i = 0; i < p_faces.size(); i++) {
		face_pos[i].index = uint32_t(i);
		face_pos[i].center = (p_faces[i].vertex[0] + p_faces[i].vertex[1] + p_faces[i].vertex[2]) / 3;
	}
	LocalVector<MeshSDFBVHNode> bvh;
	uint32_t bvh_root = MeshSDFBVH::create_bvh(bvh, face_pos.ptr(), face_pos.size(), p_faces.ptr(), 0.0f);

	// Sign rays go through StaticRaycaster (Embree) when it is available and
	// fall back to TriangleMesh::intersect_ray where it is not (for example
	// Windows arm64, or non-editor builds without the raycast module).
	Ref<StaticRaycaster> raycaster;
	Ref<TriangleMesh> triangle_mesh;
	if (!p_settings.two_sided) {
		if (p_settings.prefer_static_raycaster) {
			raycaster = StaticRaycaster::create();
		}
		if (raycaster.is_valid()) {
			PackedVector3Array vertices;
			vertices.resize(face_vertices.size());
			memcpy(vertices.ptrw(), face_vertices.ptr(), face_vertices.size() * sizeof(Vector3));
			raycaster->add_mesh(vertices, PackedInt32Array(), 0);
			raycaster->commit();
		} else {
			triangle_mesh.instantiate();
			triangle_mesh->create(face_vertices);
			ERR_FAIL_COND_V_MSG(!triangle_mesh->is_valid(), ERR_CANT_CREATE, "Failed to build a triangle mesh for mesh distance field sign rays.");
		}
	}

	const int pad = MAX(p_settings.band_voxels, 1);
	const int max_voxels = MAX(p_settings.max_voxels_per_axis, MeshSDFData::BRICK_SIZE);
	float voxel_size = 1.0f / (p_settings.voxels_per_meter * p_settings.resolution_scale);
	auto compute_dimensions = [&](float p_voxel_size) -> Vector3i {
		return Vector3i(
				MAX(int(Math::ceil(aabb.size.x / p_voxel_size)) + pad * 2, 1),
				MAX(int(Math::ceil(aabb.size.y / p_voxel_size)) + pad * 2, 1),
				MAX(int(Math::ceil(aabb.size.z / p_voxel_size)) + pad * 2, 1));
	};
	Vector3i dimensions = compute_dimensions(voxel_size);
	int longest = MAX(dimensions.x, MAX(dimensions.y, dimensions.z));
	if (longest > max_voxels) {
		voxel_size *= float(longest) / float(max_voxels);
		dimensions = compute_dimensions(voxel_size);
	}
	Vector3 origin = aabb.position - Vector3(voxel_size, voxel_size, voxel_size) * float(pad);

	Vector<Vector3> directions = get_ray_directions(MAX(p_settings.ray_count, 1));
	Vector<float> dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);

	ComputeJob job;
	job.bvh = bvh.ptr();
	job.bvh_root = bvh_root;
	job.triangles = p_faces.ptr();
	job.face_count = p_faces.size();
	job.raycaster = raycaster.ptr();
	job.triangle_mesh = triangle_mesh.ptr();
	job.directions = &directions;
	job.dimensions = dimensions;
	job.origin = origin;
	job.voxel_size = voxel_size;
	job.two_sided = p_settings.two_sided;
	job.cells = dense.ptrw();

	MeshSDFBuilder builder;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (pool != nullptr && pool->get_thread_index() == -1) {
		WorkerThreadPool::GroupID group = pool->add_template_group_task(&builder, &MeshSDFBuilder::_compute_sdf_z, &job, dimensions.z);
		pool->wait_for_group_task_completion(group);
	} else {
		for (int z = 0; z < dimensions.z; z++) {
			builder._compute_sdf_z(uint32_t(z), &job);
		}
	}

	r_data.bounds = AABB(origin, Vector3(dimensions) * voxel_size);
	r_data.voxel_size = voxel_size;
	if (p_settings.two_sided) {
		r_data.flags |= MeshSDFData::FLAG_TWO_SIDED | MeshSDFData::FLAG_UNSIGNED;
	}

	const int mip_count = CLAMP(p_settings.mip_count, 1, 3);
	Vector<float> dense_mip = dense;
	Vector3i mip_dimensions = dimensions;
	for (int mip = 0; mip < mip_count; mip++) {
		if (mip > 0) {
			dense_mip = _downsample(dense_mip, mip_dimensions, mip_dimensions);
		}
		float mip_voxel_size = voxel_size * float(1 << mip);
		float band = float(pad) * mip_voxel_size;
		float threshold = p_settings.two_sided ? mip_voxel_size : band;
		PackedInt32Array page_table;
		PackedByteArray bricks;
		Vector3i brick_grid;
		_brickify(dense_mip, mip_dimensions, band, threshold, page_table, bricks, brick_grid);
		r_data.mip_dimensions.push_back(mip_dimensions);
		r_data.mip_brick_grid_dimensions.push_back(brick_grid);
		r_data.mip_bands.push_back(band);
		r_data.page_tables.push_back(page_table);
		r_data.brick_data.push_back(bricks);
	}
	return OK;
}

float MeshSDFData::get_mip_voxel_size(int p_mip) const {
	ERR_FAIL_INDEX_V(p_mip, mip_dimensions.size(), 0.0f);
	return voxel_size * float(1 << p_mip);
}

int MeshSDFData::get_brick_count(int p_mip) const {
	ERR_FAIL_INDEX_V(p_mip, brick_data.size(), 0);
	return int(brick_data[p_mip].size() / BRICK_VOXELS);
}

int64_t MeshSDFData::get_data_size() const {
	int64_t size = 0;
	for (int i = 0; i < page_tables.size(); i++) {
		size += page_tables[i].size() * int64_t(sizeof(int32_t));
	}
	for (int i = 0; i < brick_data.size(); i++) {
		size += brick_data[i].size();
	}
	return size;
}

float MeshSDFData::get_voxel_distance(int p_mip, const Vector3i &p_voxel) const {
	ERR_FAIL_INDEX_V(p_mip, mip_dimensions.size(), 0.0f);
	const Vector3i &dims = mip_dimensions[p_mip];
	const Vector3i &bg = mip_brick_grid_dimensions[p_mip];
	float band = mip_bands[p_mip];
	Vector3i v = Vector3i(CLAMP(p_voxel.x, 0, dims.x - 1), CLAMP(p_voxel.y, 0, dims.y - 1), CLAMP(p_voxel.z, 0, dims.z - 1));
	Vector3i brick = v / BRICK_SIZE;
	Vector3i local = v % BRICK_SIZE;
	int32_t entry = page_tables[p_mip][brick.x + bg.x * (brick.y + bg.y * brick.z)];
	if (entry == PAGE_FAR_OUTSIDE) {
		return band;
	}
	if (entry == PAGE_FAR_INSIDE) {
		return -band;
	}
	uint8_t encoded = brick_data[p_mip][entry * BRICK_VOXELS + local.x + BRICK_SIZE * (local.y + BRICK_SIZE * local.z)];
	return (float(encoded) / 255.0f * 2.0f - 1.0f) * band;
}

float MeshSDFData::sample_distance(const Vector3 &p_position, int p_mip) const {
	ERR_FAIL_INDEX_V(p_mip, mip_dimensions.size(), 0.0f);
	const Vector3i &dims = mip_dimensions[p_mip];
	float mip_voxel_size = get_mip_voxel_size(p_mip);
	Vector3 f = (p_position - bounds.position) / mip_voxel_size - Vector3(0.5f, 0.5f, 0.5f);
	f = f.clamp(Vector3(), Vector3(dims.x - 1, dims.y - 1, dims.z - 1).maxf(0.0f));
	Vector3i base = Vector3i(Math::floor(f.x), Math::floor(f.y), Math::floor(f.z));
	Vector3 frac = f - Vector3(base);
	float c000 = get_voxel_distance(p_mip, base);
	float c100 = get_voxel_distance(p_mip, base + Vector3i(1, 0, 0));
	float c010 = get_voxel_distance(p_mip, base + Vector3i(0, 1, 0));
	float c110 = get_voxel_distance(p_mip, base + Vector3i(1, 1, 0));
	float c001 = get_voxel_distance(p_mip, base + Vector3i(0, 0, 1));
	float c101 = get_voxel_distance(p_mip, base + Vector3i(1, 0, 1));
	float c011 = get_voxel_distance(p_mip, base + Vector3i(0, 1, 1));
	float c111 = get_voxel_distance(p_mip, base + Vector3i(1, 1, 1));
	float x00 = Math::lerp(c000, c100, frac.x);
	float x10 = Math::lerp(c010, c110, frac.x);
	float x01 = Math::lerp(c001, c101, frac.x);
	float x11 = Math::lerp(c011, c111, frac.x);
	return Math::lerp(Math::lerp(x00, x10, frac.y), Math::lerp(x01, x11, frac.y), frac.z);
}
