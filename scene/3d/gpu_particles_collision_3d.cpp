/**************************************************************************/
/*  gpu_particles_collision_3d.cpp                                        */
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

#include "gpu_particles_collision_3d.h"

#include "core/math/geometry_3d.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

void GPUParticlesCollision3D::set_cull_mask(uint32_t p_cull_mask) {
	cull_mask = p_cull_mask;
	RS::get_singleton()->particles_collision_set_cull_mask(collision, p_cull_mask);
}

uint32_t GPUParticlesCollision3D::get_cull_mask() const {
	return cull_mask;
}

void GPUParticlesCollision3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cull_mask", "mask"), &GPUParticlesCollision3D::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &GPUParticlesCollision3D::get_cull_mask);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");
}

GPUParticlesCollision3D::GPUParticlesCollision3D(RSE::ParticlesCollisionType p_type) {
	collision = RS::get_singleton()->particles_collision_create();
	RS::get_singleton()->particles_collision_set_collision_type(collision, p_type);
	set_base(collision);
}

GPUParticlesCollision3D::~GPUParticlesCollision3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(collision);
}

/////////////////////////////////

void GPUParticlesCollisionSphere3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &GPUParticlesCollisionSphere3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &GPUParticlesCollisionSphere3D::get_radius);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_radius", "get_radius");
}

void GPUParticlesCollisionSphere3D::set_radius(real_t p_radius) {
	radius = p_radius;
	RS::get_singleton()->particles_collision_set_sphere_radius(_get_collision(), radius);
	update_gizmos();
}

real_t GPUParticlesCollisionSphere3D::get_radius() const {
	return radius;
}

AABB GPUParticlesCollisionSphere3D::get_aabb() const {
	return AABB(Vector3(-radius, -radius, -radius), Vector3(radius * 2, radius * 2, radius * 2));
}

GPUParticlesCollisionSphere3D::GPUParticlesCollisionSphere3D() :
		GPUParticlesCollision3D(RSE::PARTICLES_COLLISION_TYPE_SPHERE_COLLIDE) {
}

GPUParticlesCollisionSphere3D::~GPUParticlesCollisionSphere3D() {
}

///////////////////////////

void GPUParticlesCollisionBox3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &GPUParticlesCollisionBox3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &GPUParticlesCollisionBox3D::get_size);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
}

#ifndef DISABLE_DEPRECATED
bool GPUParticlesCollisionBox3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool GPUParticlesCollisionBox3D::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

void GPUParticlesCollisionBox3D::set_size(const Vector3 &p_size) {
	size = p_size;
	RS::get_singleton()->particles_collision_set_box_extents(_get_collision(), size / 2);
	update_gizmos();
}

Vector3 GPUParticlesCollisionBox3D::get_size() const {
	return size;
}

AABB GPUParticlesCollisionBox3D::get_aabb() const {
	return AABB(-size / 2, size);
}

GPUParticlesCollisionBox3D::GPUParticlesCollisionBox3D() :
		GPUParticlesCollision3D(RSE::PARTICLES_COLLISION_TYPE_BOX_COLLIDE) {
}

GPUParticlesCollisionBox3D::~GPUParticlesCollisionBox3D() {
}

///////////////////////////////
///////////////////////////

void GPUParticlesCollisionSDF3D::_find_meshes(const AABB &p_aabb, Node *p_at_node, List<PlotMesh> &plot_meshes) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->is_visible_in_tree()) {
		if ((mi->get_layer_mask() & bake_mask) == 0) {
			return;
		}

		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			AABB aabb = mesh->get_aabb();

			Transform3D xf = get_global_transform().affine_inverse() * mi->get_global_transform();

			if (p_aabb.intersects(xf.xform(aabb))) {
				PlotMesh pm;
				pm.local_xform = xf;
				pm.mesh = mesh;
				plot_meshes.push_back(pm);
			}
		}
	}

	Node3D *s = Object::cast_to<Node3D>(p_at_node);
	if (s) {
		if (s->is_visible_in_tree()) {
			Array meshes = p_at_node->call("get_meshes");
			for (int i = 0; i < meshes.size(); i += 2) {
				Transform3D mxf = meshes[i];
				Ref<Mesh> mesh = meshes[i + 1];
				if (mesh.is_null()) {
					continue;
				}

				AABB aabb = mesh->get_aabb();

				Transform3D xf = get_global_transform().affine_inverse() * (s->get_global_transform() * mxf);

				if (p_aabb.intersects(xf.xform(aabb))) {
					PlotMesh pm;
					pm.local_xform = xf;
					pm.mesh = mesh;
					plot_meshes.push_back(pm);
				}
			}
		}
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		Node *child = p_at_node->get_child(i);
		_find_meshes(p_aabb, child, plot_meshes);
	}
}

// GodotNext: mesh_sdf_bvh_extraction
// _create_bvh and _find_closest_distance moved to MeshSDFBVH in
// core/math/mesh_sdf_builder.cpp, shared with the mesh distance field builder.

void GPUParticlesCollisionSDF3D::_compute_sdf_z(uint32_t p_z, ComputeSDFParams *params) {
	int32_t z_ofs = p_z * params->size.y * params->size.x;
	for (int32_t y = 0; y < params->size.y; y++) {
		int32_t y_ofs = z_ofs + y * params->size.x;
		for (int32_t x = 0; x < params->size.x; x++) {
			int32_t x_ofs = y_ofs + x;
			float &cell = params->cells[x_ofs];

			Vector3 pos = params->cell_offset + Vector3(x, y, p_z) * params->cell_size;

			cell = 1e20;

			// GodotNext: mesh_sdf_bvh_extraction
			MeshSDFBVH::find_closest_distance(pos, params->bvh, 0, params->triangles, params->thickness, cell);
		}
	}
}

void GPUParticlesCollisionSDF3D::_compute_sdf(ComputeSDFParams *params) {
	WorkerThreadPool::GroupID group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &GPUParticlesCollisionSDF3D::_compute_sdf_z, params, params->size.z);
	while (!WorkerThreadPool::get_singleton()->is_group_task_completed(group_task)) {
		OS::get_singleton()->delay_usec(10000);
		if (bake_step_function) {
			bake_step_function(WorkerThreadPool::get_singleton()->get_group_processed_element_count(group_task) * 100 / params->size.z, "Baking SDF");
		}
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
}

Vector3i GPUParticlesCollisionSDF3D::get_estimated_cell_size() const {
	static const int subdivs[RESOLUTION_MAX] = { 16, 32, 64, 128, 256, 512 };
	int subdiv = subdivs[get_resolution()];

	AABB aabb(-size / 2, size);

	float cell_size = aabb.get_longest_axis_size() / float(subdiv);

	Vector3i sdf_size = Vector3i(aabb.size / cell_size);
	sdf_size = sdf_size.maxi(1);
	return sdf_size;
}

Ref<Image> GPUParticlesCollisionSDF3D::bake() {
	static const int subdivs[RESOLUTION_MAX] = { 16, 32, 64, 128, 256, 512 };
	int subdiv = subdivs[get_resolution()];

	AABB aabb(-size / 2, size);

	float cell_size = aabb.get_longest_axis_size() / float(subdiv);

	Vector3i sdf_size = Vector3i(aabb.size / cell_size);
	sdf_size = sdf_size.maxi(1);

	if (bake_begin_function) {
		bake_begin_function(100);
	}

	aabb.size = Vector3(sdf_size) * cell_size;

	List<PlotMesh> plot_meshes;
	_find_meshes(aabb, get_parent(), plot_meshes);

	LocalVector<Face3> faces;

	if (bake_step_function) {
		bake_step_function(0, "Finding Meshes");
	}

	for (const PlotMesh &pm : plot_meshes) {
		for (int i = 0; i < pm.mesh->get_surface_count(); i++) {
			if (pm.mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
				continue; //only triangles
			}

			Array a = pm.mesh->surface_get_arrays(i);

			Vector<Vector3> vertices = a[Mesh::ARRAY_VERTEX];
			const Vector3 *vr = vertices.ptr();
			Vector<int> index = a[Mesh::ARRAY_INDEX];

			if (index.size()) {
				int facecount = index.size() / 3;
				const int *ir = index.ptr();

				for (int j = 0; j < facecount; j++) {
					Face3 face;

					for (int k = 0; k < 3; k++) {
						face.vertex[k] = pm.local_xform.xform(vr[ir[j * 3 + k]]);
					}

					//test against original bounds
					if (!Geometry3D::triangle_box_overlap(aabb.get_center(), aabb.size * 0.5, face.vertex)) {
						continue;
					}

					faces.push_back(face);
				}

			} else {
				int facecount = vertices.size() / 3;

				for (int j = 0; j < facecount; j++) {
					Face3 face;

					for (int k = 0; k < 3; k++) {
						face.vertex[k] = pm.local_xform.xform(vr[j * 3 + k]);
					}

					//test against original bounds
					if (!Geometry3D::triangle_box_overlap(aabb.get_center(), aabb.size * 0.5, face.vertex)) {
						continue;
					}

					faces.push_back(face);
				}
			}
		}
	}

	//compute bvh
	if (faces.size() <= 1) {
		ERR_PRINT("No faces detected during GPUParticlesCollisionSDF3D bake. Check whether there are visible meshes matching the bake mask within its extents.");
		if (bake_end_function) {
			bake_end_function();
		}
		return Ref<Image>();
	}

	// GodotNext: mesh_sdf_bvh_extraction
	LocalVector<MeshSDFFacePos> face_pos;

	face_pos.resize(faces.size());

	float th = cell_size * thickness;

	for (uint32_t i = 0; i < faces.size(); i++) {
		face_pos[i].index = i;
		// GodotNext: mesh_sdf_centroid_fix
		face_pos[i].center = (faces[i].vertex[0] + faces[i].vertex[1] + faces[i].vertex[2]) / 3;
		if (th > 0.0) {
			face_pos[i].center -= faces[i].get_plane().normal * th * 0.5;
		}
	}

	if (bake_step_function) {
		bake_step_function(0, "Creating BVH");
	}

	// GodotNext: mesh_sdf_bvh_extraction
	LocalVector<MeshSDFBVHNode> bvh;

	MeshSDFBVH::create_bvh(bvh, face_pos.ptr(), face_pos.size(), faces.ptr(), th);

	Vector<uint8_t> cells_data;
	cells_data.resize(sdf_size.z * sdf_size.y * sdf_size.x * (int)sizeof(float));

	if (bake_step_function) {
		bake_step_function(0, "Baking SDF");
	}

	ComputeSDFParams params;
	params.cells = (float *)cells_data.ptrw();
	params.size = sdf_size;
	params.cell_size = cell_size;
	params.cell_offset = aabb.position + Vector3(cell_size * 0.5, cell_size * 0.5, cell_size * 0.5);
	params.bvh = bvh.ptr();
	params.triangles = faces.ptr();
	params.thickness = th;
	_compute_sdf(&params);

	Ref<Image> ret = Image::create_from_data(sdf_size.x, sdf_size.y * sdf_size.z, false, Image::FORMAT_RF, cells_data);
	ret->convert(Image::FORMAT_RH); //convert to half, save space
	ret->set_meta("depth", sdf_size.z); //hack, make sure to add to the docs of this function

	if (bake_end_function) {
		bake_end_function();
	}

	return ret;
}

PackedStringArray GPUParticlesCollisionSDF3D::get_configuration_warnings() const {
	PackedStringArray warnings = GPUParticlesCollision3D::get_configuration_warnings();

	if (bake_mask == 0) {
		warnings.push_back(RTR("The Bake Mask has no bits enabled, which means baking will not produce any collision for this GPUParticlesCollisionSDF3D.\nTo resolve this, enable at least one bit in the Bake Mask property."));
	}

	return warnings;
}

void GPUParticlesCollisionSDF3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &GPUParticlesCollisionSDF3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &GPUParticlesCollisionSDF3D::get_size);

	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &GPUParticlesCollisionSDF3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &GPUParticlesCollisionSDF3D::get_resolution);

	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &GPUParticlesCollisionSDF3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &GPUParticlesCollisionSDF3D::get_texture);

	ClassDB::bind_method(D_METHOD("set_thickness", "thickness"), &GPUParticlesCollisionSDF3D::set_thickness);
	ClassDB::bind_method(D_METHOD("get_thickness"), &GPUParticlesCollisionSDF3D::get_thickness);

	ClassDB::bind_method(D_METHOD("set_bake_mask", "mask"), &GPUParticlesCollisionSDF3D::set_bake_mask);
	ClassDB::bind_method(D_METHOD("get_bake_mask"), &GPUParticlesCollisionSDF3D::get_bake_mask);
	ClassDB::bind_method(D_METHOD("set_bake_mask_value", "layer_number", "value"), &GPUParticlesCollisionSDF3D::set_bake_mask_value);
	ClassDB::bind_method(D_METHOD("get_bake_mask_value", "layer_number"), &GPUParticlesCollisionSDF3D::get_bake_mask_value);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_ENUM, "16,32,64,128,256,512"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "thickness", PROPERTY_HINT_RANGE, "0.0,2.0,0.01,suffix:m"), "set_thickness", "get_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bake_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_bake_mask", "get_bake_mask");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, Texture3D::get_class_static()), "set_texture", "get_texture");

	BIND_ENUM_CONSTANT(RESOLUTION_16);
	BIND_ENUM_CONSTANT(RESOLUTION_32);
	BIND_ENUM_CONSTANT(RESOLUTION_64);
	BIND_ENUM_CONSTANT(RESOLUTION_128);
	BIND_ENUM_CONSTANT(RESOLUTION_256);
	BIND_ENUM_CONSTANT(RESOLUTION_512);
	BIND_ENUM_CONSTANT(RESOLUTION_MAX);
}

#ifndef DISABLE_DEPRECATED
bool GPUParticlesCollisionSDF3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool GPUParticlesCollisionSDF3D::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

void GPUParticlesCollisionSDF3D::set_thickness(float p_thickness) {
	thickness = p_thickness;
}

float GPUParticlesCollisionSDF3D::get_thickness() const {
	return thickness;
}

void GPUParticlesCollisionSDF3D::set_size(const Vector3 &p_size) {
	size = p_size;
	RS::get_singleton()->particles_collision_set_box_extents(_get_collision(), size / 2);
	update_gizmos();
}

Vector3 GPUParticlesCollisionSDF3D::get_size() const {
	return size;
}

void GPUParticlesCollisionSDF3D::set_resolution(Resolution p_resolution) {
	resolution = p_resolution;
	update_gizmos();
}

GPUParticlesCollisionSDF3D::Resolution GPUParticlesCollisionSDF3D::get_resolution() const {
	return resolution;
}

void GPUParticlesCollisionSDF3D::set_bake_mask(uint32_t p_mask) {
	bake_mask = p_mask;
	update_configuration_warnings();
}

uint32_t GPUParticlesCollisionSDF3D::get_bake_mask() const {
	return bake_mask;
}

void GPUParticlesCollisionSDF3D::set_bake_mask_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1 || p_layer_number > 20, vformat("The render layer number (%d) must be between 1 and 20 (inclusive).", p_layer_number));
	uint32_t mask = get_bake_mask();
	if (p_value) {
		mask |= 1 << (p_layer_number - 1);
	} else {
		mask &= ~(1 << (p_layer_number - 1));
	}
	set_bake_mask(mask);
}

bool GPUParticlesCollisionSDF3D::get_bake_mask_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1 || p_layer_number > 20, false, vformat("The render layer number (%d) must be between 1 and 20 (inclusive).", p_layer_number));
	return bake_mask & (1 << (p_layer_number - 1));
}

void GPUParticlesCollisionSDF3D::set_texture(const Ref<Texture3D> &p_texture) {
	texture = p_texture;
	RID tex = texture.is_valid() ? texture->get_rid() : RID();
	RS::get_singleton()->particles_collision_set_field_texture(_get_collision(), tex);
}

Ref<Texture3D> GPUParticlesCollisionSDF3D::get_texture() const {
	return texture;
}

AABB GPUParticlesCollisionSDF3D::get_aabb() const {
	return AABB(-size / 2, size);
}

GPUParticlesCollisionSDF3D::BakeBeginFunc GPUParticlesCollisionSDF3D::bake_begin_function = nullptr;
GPUParticlesCollisionSDF3D::BakeStepFunc GPUParticlesCollisionSDF3D::bake_step_function = nullptr;
GPUParticlesCollisionSDF3D::BakeEndFunc GPUParticlesCollisionSDF3D::bake_end_function = nullptr;

GPUParticlesCollisionSDF3D::GPUParticlesCollisionSDF3D() :
		GPUParticlesCollision3D(RSE::PARTICLES_COLLISION_TYPE_SDF_COLLIDE) {
}

GPUParticlesCollisionSDF3D::~GPUParticlesCollisionSDF3D() {
}

////////////////////////////
////////////////////////////

void GPUParticlesCollisionHeightField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (update_mode == UPDATE_MODE_ALWAYS) {
				RS::get_singleton()->particles_collision_height_field_update(_get_collision());
			}

			if (follow_camera_mode && get_viewport()) {
				Camera3D *cam = get_viewport()->get_camera_3d();
				if (cam) {
					Transform3D xform = get_global_transform();
					Vector3 x_axis = xform.basis.get_column(Vector3::AXIS_X).normalized();
					Vector3 z_axis = xform.basis.get_column(Vector3::AXIS_Z).normalized();
					float x_len = xform.basis.get_scale().x;
					float z_len = xform.basis.get_scale().z;

					Vector3 cam_pos = cam->get_global_transform().origin;
					Transform3D new_xform = xform;

					while (x_axis.dot(cam_pos - new_xform.origin) > x_len) {
						new_xform.origin += x_axis * x_len;
					}
					while (x_axis.dot(cam_pos - new_xform.origin) < -x_len) {
						new_xform.origin -= x_axis * x_len;
					}

					while (z_axis.dot(cam_pos - new_xform.origin) > z_len) {
						new_xform.origin += z_axis * z_len;
					}
					while (z_axis.dot(cam_pos - new_xform.origin) < -z_len) {
						new_xform.origin -= z_axis * z_len;
					}

					if (new_xform != xform) {
						set_global_transform(new_xform);
						RS::get_singleton()->particles_collision_height_field_update(_get_collision());
					}
				}
			}
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			RS::get_singleton()->particles_collision_height_field_update(_get_collision());
		} break;
	}
}

void GPUParticlesCollisionHeightField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &GPUParticlesCollisionHeightField3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &GPUParticlesCollisionHeightField3D::get_size);

	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &GPUParticlesCollisionHeightField3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &GPUParticlesCollisionHeightField3D::get_resolution);

	ClassDB::bind_method(D_METHOD("set_update_mode", "update_mode"), &GPUParticlesCollisionHeightField3D::set_update_mode);
	ClassDB::bind_method(D_METHOD("get_update_mode"), &GPUParticlesCollisionHeightField3D::get_update_mode);

	ClassDB::bind_method(D_METHOD("set_heightfield_mask", "heightfield_mask"), &GPUParticlesCollisionHeightField3D::set_heightfield_mask);
	ClassDB::bind_method(D_METHOD("get_heightfield_mask"), &GPUParticlesCollisionHeightField3D::get_heightfield_mask);

	ClassDB::bind_method(D_METHOD("set_heightfield_mask_value", "layer_number", "value"), &GPUParticlesCollisionHeightField3D::set_heightfield_mask_value);
	ClassDB::bind_method(D_METHOD("get_heightfield_mask_value", "layer_number"), &GPUParticlesCollisionHeightField3D::get_heightfield_mask_value);

	ClassDB::bind_method(D_METHOD("set_follow_camera_enabled", "enabled"), &GPUParticlesCollisionHeightField3D::set_follow_camera_enabled);
	ClassDB::bind_method(D_METHOD("is_follow_camera_enabled"), &GPUParticlesCollisionHeightField3D::is_follow_camera_enabled);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_ENUM, "256 (Fastest),512 (Fast),1024 (Average),2048 (Slow),4096 (Slower),8192 (Slowest)"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "update_mode", PROPERTY_HINT_ENUM, "When Moved (Fast),Always (Slow)"), "set_update_mode", "get_update_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_camera_enabled"), "set_follow_camera_enabled", "is_follow_camera_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heightfield_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_heightfield_mask", "get_heightfield_mask");

	BIND_ENUM_CONSTANT(RESOLUTION_256);
	BIND_ENUM_CONSTANT(RESOLUTION_512);
	BIND_ENUM_CONSTANT(RESOLUTION_1024);
	BIND_ENUM_CONSTANT(RESOLUTION_2048);
	BIND_ENUM_CONSTANT(RESOLUTION_4096);
	BIND_ENUM_CONSTANT(RESOLUTION_8192);
	BIND_ENUM_CONSTANT(RESOLUTION_MAX);

	BIND_ENUM_CONSTANT(UPDATE_MODE_WHEN_MOVED);
	BIND_ENUM_CONSTANT(UPDATE_MODE_ALWAYS);
}

#ifndef DISABLE_DEPRECATED
bool GPUParticlesCollisionHeightField3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool GPUParticlesCollisionHeightField3D::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

void GPUParticlesCollisionHeightField3D::set_size(const Vector3 &p_size) {
	size = p_size;
	RS::get_singleton()->particles_collision_set_box_extents(_get_collision(), size / 2);
	update_gizmos();
	RS::get_singleton()->particles_collision_height_field_update(_get_collision());
}

Vector3 GPUParticlesCollisionHeightField3D::get_size() const {
	return size;
}

void GPUParticlesCollisionHeightField3D::set_resolution(Resolution p_resolution) {
	resolution = p_resolution;
	RS::get_singleton()->particles_collision_set_height_field_resolution(_get_collision(), RSE::ParticlesCollisionHeightfieldResolution(resolution));
	update_gizmos();
	RS::get_singleton()->particles_collision_height_field_update(_get_collision());
}

GPUParticlesCollisionHeightField3D::Resolution GPUParticlesCollisionHeightField3D::get_resolution() const {
	return resolution;
}

void GPUParticlesCollisionHeightField3D::set_update_mode(UpdateMode p_update_mode) {
	update_mode = p_update_mode;
	set_process_internal(follow_camera_mode || update_mode == UPDATE_MODE_ALWAYS);
}

GPUParticlesCollisionHeightField3D::UpdateMode GPUParticlesCollisionHeightField3D::get_update_mode() const {
	return update_mode;
}

void GPUParticlesCollisionHeightField3D::set_heightfield_mask(uint32_t p_heightfield_mask) {
	heightfield_mask = p_heightfield_mask;
	RS::get_singleton()->particles_collision_set_height_field_mask(_get_collision(), p_heightfield_mask);
}

uint32_t GPUParticlesCollisionHeightField3D::get_heightfield_mask() const {
	return heightfield_mask;
}

void GPUParticlesCollisionHeightField3D::set_heightfield_mask_value(int p_layer_number, bool p_value) {
	ERR_FAIL_COND_MSG(p_layer_number < 1, "Render layer number must be between 1 and 20 inclusive.");
	ERR_FAIL_COND_MSG(p_layer_number > 20, "Render layer number must be between 1 and 20 inclusive.");
	uint32_t mask = get_heightfield_mask();
	if (p_value) {
		mask |= 1 << (p_layer_number - 1);
	} else {
		mask &= ~(1 << (p_layer_number - 1));
	}
	set_heightfield_mask(mask);
}

bool GPUParticlesCollisionHeightField3D::get_heightfield_mask_value(int p_layer_number) const {
	ERR_FAIL_COND_V_MSG(p_layer_number < 1, false, "Render layer number must be between 1 and 20 inclusive.");
	ERR_FAIL_COND_V_MSG(p_layer_number > 20, false, "Render layer number must be between 1 and 20 inclusive.");
	return heightfield_mask & (1 << (p_layer_number - 1));
}

void GPUParticlesCollisionHeightField3D::set_follow_camera_enabled(bool p_enabled) {
	follow_camera_mode = p_enabled;
	set_process_internal(follow_camera_mode || update_mode == UPDATE_MODE_ALWAYS);
}

bool GPUParticlesCollisionHeightField3D::is_follow_camera_enabled() const {
	return follow_camera_mode;
}

AABB GPUParticlesCollisionHeightField3D::get_aabb() const {
	return AABB(-size / 2, size);
}

GPUParticlesCollisionHeightField3D::GPUParticlesCollisionHeightField3D() :
		GPUParticlesCollision3D(RSE::PARTICLES_COLLISION_TYPE_HEIGHTFIELD_COLLIDE) {
}

GPUParticlesCollisionHeightField3D::~GPUParticlesCollisionHeightField3D() {
}

////////////////////////////
////////////////////////////

void GPUParticlesAttractor3D::set_cull_mask(uint32_t p_cull_mask) {
	cull_mask = p_cull_mask;
	RS::get_singleton()->particles_collision_set_cull_mask(collision, p_cull_mask);
}

uint32_t GPUParticlesAttractor3D::get_cull_mask() const {
	return cull_mask;
}

void GPUParticlesAttractor3D::set_strength(real_t p_strength) {
	strength = p_strength;
	RS::get_singleton()->particles_collision_set_attractor_strength(collision, p_strength);
}

real_t GPUParticlesAttractor3D::get_strength() const {
	return strength;
}

void GPUParticlesAttractor3D::set_attenuation(real_t p_attenuation) {
	attenuation = p_attenuation;
	RS::get_singleton()->particles_collision_set_attractor_attenuation(collision, p_attenuation);
}

real_t GPUParticlesAttractor3D::get_attenuation() const {
	return attenuation;
}

void GPUParticlesAttractor3D::set_directionality(real_t p_directionality) {
	directionality = p_directionality;
	RS::get_singleton()->particles_collision_set_attractor_directionality(collision, p_directionality);
	update_gizmos();
}

real_t GPUParticlesAttractor3D::get_directionality() const {
	return directionality;
}

void GPUParticlesAttractor3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cull_mask", "mask"), &GPUParticlesAttractor3D::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &GPUParticlesAttractor3D::get_cull_mask);

	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &GPUParticlesAttractor3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &GPUParticlesAttractor3D::get_strength);

	ClassDB::bind_method(D_METHOD("set_attenuation", "attenuation"), &GPUParticlesAttractor3D::set_attenuation);
	ClassDB::bind_method(D_METHOD("get_attenuation"), &GPUParticlesAttractor3D::get_attenuation);

	ClassDB::bind_method(D_METHOD("set_directionality", "amount"), &GPUParticlesAttractor3D::set_directionality);
	ClassDB::bind_method(D_METHOD("get_directionality"), &GPUParticlesAttractor3D::get_directionality);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "-128,128,0.01,or_greater,or_less"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "attenuation", PROPERTY_HINT_EXP_EASING, "0,8,0.01"), "set_attenuation", "get_attenuation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "directionality", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_directionality", "get_directionality");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");
}

GPUParticlesAttractor3D::GPUParticlesAttractor3D(RSE::ParticlesCollisionType p_type) {
	collision = RS::get_singleton()->particles_collision_create();
	RS::get_singleton()->particles_collision_set_collision_type(collision, p_type);
	set_base(collision);
}
GPUParticlesAttractor3D::~GPUParticlesAttractor3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(collision);
}

/////////////////////////////////

void GPUParticlesAttractorSphere3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &GPUParticlesAttractorSphere3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &GPUParticlesAttractorSphere3D::get_radius);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_radius", "get_radius");
}

void GPUParticlesAttractorSphere3D::set_radius(real_t p_radius) {
	radius = p_radius;
	RS::get_singleton()->particles_collision_set_sphere_radius(_get_collision(), radius);
	update_gizmos();
}

real_t GPUParticlesAttractorSphere3D::get_radius() const {
	return radius;
}

AABB GPUParticlesAttractorSphere3D::get_aabb() const {
	return AABB(Vector3(-radius, -radius, -radius), Vector3(radius * 2, radius * 2, radius * 2));
}

GPUParticlesAttractorSphere3D::GPUParticlesAttractorSphere3D() :
		GPUParticlesAttractor3D(RSE::PARTICLES_COLLISION_TYPE_SPHERE_ATTRACT) {
}

GPUParticlesAttractorSphere3D::~GPUParticlesAttractorSphere3D() {
}

///////////////////////////

void GPUParticlesAttractorBox3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &GPUParticlesAttractorBox3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &GPUParticlesAttractorBox3D::get_size);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
}

#ifndef DISABLE_DEPRECATED
bool GPUParticlesAttractorBox3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool GPUParticlesAttractorBox3D::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

void GPUParticlesAttractorBox3D::set_size(const Vector3 &p_size) {
	size = p_size;
	RS::get_singleton()->particles_collision_set_box_extents(_get_collision(), size / 2);
	update_gizmos();
}

Vector3 GPUParticlesAttractorBox3D::get_size() const {
	return size;
}

AABB GPUParticlesAttractorBox3D::get_aabb() const {
	return AABB(-size / 2, size);
}

GPUParticlesAttractorBox3D::GPUParticlesAttractorBox3D() :
		GPUParticlesAttractor3D(RSE::PARTICLES_COLLISION_TYPE_BOX_ATTRACT) {
}

GPUParticlesAttractorBox3D::~GPUParticlesAttractorBox3D() {
}

///////////////////////////

void GPUParticlesAttractorVectorField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &GPUParticlesAttractorVectorField3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &GPUParticlesAttractorVectorField3D::get_size);

	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &GPUParticlesAttractorVectorField3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &GPUParticlesAttractorVectorField3D::get_texture);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, Texture3D::get_class_static()), "set_texture", "get_texture");
}

#ifndef DISABLE_DEPRECATED
bool GPUParticlesAttractorVectorField3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool GPUParticlesAttractorVectorField3D::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

void GPUParticlesAttractorVectorField3D::set_size(const Vector3 &p_size) {
	size = p_size;
	RS::get_singleton()->particles_collision_set_box_extents(_get_collision(), size / 2);
	update_gizmos();
}

Vector3 GPUParticlesAttractorVectorField3D::get_size() const {
	return size;
}

void GPUParticlesAttractorVectorField3D::set_texture(const Ref<Texture3D> &p_texture) {
	texture = p_texture;
	RID tex = texture.is_valid() ? texture->get_rid() : RID();
	RS::get_singleton()->particles_collision_set_field_texture(_get_collision(), tex);
}

Ref<Texture3D> GPUParticlesAttractorVectorField3D::get_texture() const {
	return texture;
}

AABB GPUParticlesAttractorVectorField3D::get_aabb() const {
	return AABB(-size / 2, size);
}

GPUParticlesAttractorVectorField3D::GPUParticlesAttractorVectorField3D() :
		GPUParticlesAttractor3D(RSE::PARTICLES_COLLISION_TYPE_VECTOR_FIELD_ATTRACT) {
}

GPUParticlesAttractorVectorField3D::~GPUParticlesAttractorVectorField3D() {
}
