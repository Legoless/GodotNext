/**************************************************************************/
/*  world_3d.cpp                                                          */
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

#include "world_3d.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

void World3D::_register_camera(Camera3D *p_camera) {
	cameras.insert(p_camera);
}

void World3D::_remove_camera(Camera3D *p_camera) {
	cameras.erase(p_camera);
}

RID World3D::get_space() const {
#ifndef PHYSICS_3D_DISABLED
	if (space.is_null()) {
		space = PhysicsServer3D::get_singleton()->space_create();
		PhysicsServer3D::get_singleton()->space_set_active(space, true);
		PhysicsServer3D::get_singleton()->area_set_param(space, PS3DE::AREA_PARAM_GRAVITY, GLOBAL_GET("physics/3d/default_gravity"));
		PhysicsServer3D::get_singleton()->area_set_param(space, PS3DE::AREA_PARAM_GRAVITY_VECTOR, GLOBAL_GET("physics/3d/default_gravity_vector"));
		PhysicsServer3D::get_singleton()->area_set_param(space, PS3DE::AREA_PARAM_LINEAR_DAMP, GLOBAL_GET("physics/3d/default_linear_damp"));
		PhysicsServer3D::get_singleton()->area_set_param(space, PS3DE::AREA_PARAM_ANGULAR_DAMP, GLOBAL_GET("physics/3d/default_angular_damp"));
	}
#endif // PHYSICS_3D_DISABLED
	return space;
}

#ifndef NAVIGATION_3D_DISABLED
RID World3D::get_navigation_map() const {
	if (navigation_map.is_null()) {
		navigation_map = NavigationServer3D::get_singleton()->map_create();
		NavigationServer3D::get_singleton()->map_set_active(navigation_map, true);
		NavigationServer3D::get_singleton()->map_set_cell_size(navigation_map, GLOBAL_GET("navigation/3d/default_cell_size"));
		NavigationServer3D::get_singleton()->map_set_cell_height(navigation_map, GLOBAL_GET("navigation/3d/default_cell_height"));
		NavigationServer3D::get_singleton()->map_set_up(navigation_map, GLOBAL_GET("navigation/3d/default_up"));
		NavigationServer3D::get_singleton()->map_set_merge_rasterizer_cell_scale(navigation_map, GLOBAL_GET("navigation/3d/merge_rasterizer_cell_scale"));
		NavigationServer3D::get_singleton()->map_set_use_edge_connections(navigation_map, GLOBAL_GET("navigation/3d/use_edge_connections"));
		NavigationServer3D::get_singleton()->map_set_edge_connection_margin(navigation_map, GLOBAL_GET("navigation/3d/default_edge_connection_margin"));
		NavigationServer3D::get_singleton()->map_set_link_connection_radius(navigation_map, GLOBAL_GET("navigation/3d/default_link_connection_radius"));
	}
	return navigation_map;
}
#endif // NAVIGATION_3D_DISABLED

RID World3D::get_scenario() const {
	return scenario;
}

void World3D::set_environment(const Ref<Environment> &p_environment) {
	if (environment == p_environment) {
		return;
	}

	environment = p_environment;
	if (environment.is_valid()) {
		RS::get_singleton()->scenario_set_environment(scenario, environment->get_rid());
	} else {
		RS::get_singleton()->scenario_set_environment(scenario, RID());
	}

	emit_changed();
}

Ref<Environment> World3D::get_environment() const {
	return environment;
}

void World3D::set_fallback_environment(const Ref<Environment> &p_environment) {
	if (fallback_environment == p_environment) {
		return;
	}

	fallback_environment = p_environment;
	if (fallback_environment.is_valid()) {
		RS::get_singleton()->scenario_set_fallback_environment(scenario, p_environment->get_rid());
	} else {
		RS::get_singleton()->scenario_set_fallback_environment(scenario, RID());
	}

	emit_changed();
}

Ref<Environment> World3D::get_fallback_environment() const {
	return fallback_environment;
}

void World3D::set_camera_attributes(const Ref<CameraAttributes> &p_camera_attributes) {
	camera_attributes = p_camera_attributes;
	if (camera_attributes.is_valid()) {
		RS::get_singleton()->scenario_set_camera_attributes(scenario, camera_attributes->get_rid());
	} else {
		RS::get_singleton()->scenario_set_camera_attributes(scenario, RID());
	}
}

Ref<CameraAttributes> World3D::get_camera_attributes() const {
	return camera_attributes;
}

void World3D::set_compositor(const Ref<Compositor> &p_compositor) {
	compositor = p_compositor;
	if (compositor.is_valid()) {
		RS::get_singleton()->scenario_set_compositor(scenario, compositor->get_rid());
	} else {
		RS::get_singleton()->scenario_set_compositor(scenario, RID());
	}
}

Ref<Compositor> World3D::get_compositor() const {
	return compositor;
}

void World3D::_update_wind() {
	if (scenario.is_valid() && RS::get_singleton() != nullptr) {
		RS::get_singleton()->scenario_set_wind(scenario, wind_direction, wind_strength, wind_gust_frequency);
	}
}

void World3D::set_wind_direction(const Vector3 &p_direction) {
	if (wind_direction == p_direction) {
		return;
	}
	wind_direction = p_direction;
	_update_wind();
	emit_changed();
}

Vector3 World3D::get_wind_direction() const {
	return wind_direction;
}

void World3D::set_wind_strength(float p_strength) {
	const float clamped = MAX(p_strength, 0.0f);
	if (wind_strength == clamped) {
		return;
	}
	wind_strength = clamped;
	_update_wind();
	emit_changed();
}

float World3D::get_wind_strength() const {
	return wind_strength;
}

void World3D::set_wind_gust_frequency(float p_frequency) {
	const float clamped = MAX(p_frequency, 0.0f);
	if (wind_gust_frequency == clamped) {
		return;
	}
	wind_gust_frequency = clamped;
	_update_wind();
	emit_changed();
}

float World3D::get_wind_gust_frequency() const {
	return wind_gust_frequency;
}

int World3D::add_wind_force(const Vector3 &p_position, float p_radius, const Vector3 &p_direction, float p_strength, float p_falloff) {
	if (scenario.is_valid() && RS::get_singleton() != nullptr) {
		return RS::get_singleton()->scenario_add_wind_force(scenario, p_position, p_radius, p_direction, p_strength, p_falloff);
	}
	return 0;
}

void World3D::update_wind_force(int p_force_id, const Vector3 &p_position, float p_radius, const Vector3 &p_direction, float p_strength, float p_falloff) {
	if (scenario.is_valid() && RS::get_singleton() != nullptr) {
		RS::get_singleton()->scenario_update_wind_force(scenario, p_force_id, p_position, p_radius, p_direction, p_strength, p_falloff);
	}
}

void World3D::remove_wind_force(int p_force_id) {
	if (scenario.is_valid() && RS::get_singleton() != nullptr) {
		RS::get_singleton()->scenario_remove_wind_force(scenario, p_force_id);
	}
}

void World3D::clear_wind_forces() {
	if (scenario.is_valid() && RS::get_singleton() != nullptr) {
		RS::get_singleton()->scenario_clear_wind_forces(scenario);
	}
}

#ifndef PHYSICS_3D_DISABLED
PhysicsDirectSpaceState3D *World3D::get_direct_space_state() {
	return PhysicsServer3D::get_singleton()->space_get_direct_state(get_space());
}
#endif // PHYSICS_3D_DISABLED

void World3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_space"), &World3D::get_space);
#ifndef NAVIGATION_3D_DISABLED
	ClassDB::bind_method(D_METHOD("get_navigation_map"), &World3D::get_navigation_map);
#endif // NAVIGATION_3D_DISABLED
	ClassDB::bind_method(D_METHOD("get_scenario"), &World3D::get_scenario);
	ClassDB::bind_method(D_METHOD("set_environment", "env"), &World3D::set_environment);
	ClassDB::bind_method(D_METHOD("get_environment"), &World3D::get_environment);
	ClassDB::bind_method(D_METHOD("set_fallback_environment", "env"), &World3D::set_fallback_environment);
	ClassDB::bind_method(D_METHOD("get_fallback_environment"), &World3D::get_fallback_environment);
	ClassDB::bind_method(D_METHOD("set_camera_attributes", "attributes"), &World3D::set_camera_attributes);
	ClassDB::bind_method(D_METHOD("get_camera_attributes"), &World3D::get_camera_attributes);
	// GodotNext: nextgeo_foliage_wind (#152)
	ClassDB::bind_method(D_METHOD("set_wind_direction", "direction"), &World3D::set_wind_direction);
	ClassDB::bind_method(D_METHOD("get_wind_direction"), &World3D::get_wind_direction);
	ClassDB::bind_method(D_METHOD("set_wind_strength", "strength"), &World3D::set_wind_strength);
	ClassDB::bind_method(D_METHOD("get_wind_strength"), &World3D::get_wind_strength);
	ClassDB::bind_method(D_METHOD("set_wind_gust_frequency", "frequency"), &World3D::set_wind_gust_frequency);
	ClassDB::bind_method(D_METHOD("get_wind_gust_frequency"), &World3D::get_wind_gust_frequency);
	// GodotNext: nextgeo_dynamic_wind (#296)
	ClassDB::bind_method(D_METHOD("add_wind_force", "position", "radius", "direction", "strength", "falloff"), &World3D::add_wind_force, DEFVAL(Vector3()), DEFVAL(1.0f), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("update_wind_force", "force_id", "position", "radius", "direction", "strength", "falloff"), &World3D::update_wind_force, DEFVAL(Vector3()), DEFVAL(1.0f), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("remove_wind_force", "force_id"), &World3D::remove_wind_force);
	ClassDB::bind_method(D_METHOD("clear_wind_forces"), &World3D::clear_wind_forces);
#ifndef PHYSICS_3D_DISABLED
	ClassDB::bind_method(D_METHOD("get_direct_space_state"), &World3D::get_direct_space_state);
#endif // PHYSICS_3D_DISABLED
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment", PROPERTY_HINT_RESOURCE_TYPE, Environment::get_class_static()), "set_environment", "get_environment");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fallback_environment", PROPERTY_HINT_RESOURCE_TYPE, Environment::get_class_static()), "set_fallback_environment", "get_fallback_environment");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "camera_attributes", PROPERTY_HINT_RESOURCE_TYPE, "CameraAttributesPractical,CameraAttributesPhysical"), "set_camera_attributes", "get_camera_attributes");
	ADD_GROUP("Wind", "wind_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind_direction"), "set_wind_direction", "get_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_strength", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_wind_strength", "get_wind_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_gust_frequency", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_wind_gust_frequency", "get_wind_gust_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::RID, "space", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_space");
#ifndef NAVIGATION_3D_DISABLED
	ADD_PROPERTY(PropertyInfo(Variant::RID, "navigation_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_navigation_map");
#endif // NAVIGATION_3D_DISABLED
	ADD_PROPERTY(PropertyInfo(Variant::RID, "scenario", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_scenario");
#ifndef PHYSICS_3D_DISABLED
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "direct_space_state", PROPERTY_HINT_RESOURCE_TYPE, PhysicsDirectSpaceState3D::get_class_static(), PROPERTY_USAGE_NONE), "", "get_direct_space_state");
#endif // PHYSICS_3D_DISABLED
}

World3D::World3D() {
	scenario = RenderingServer::get_singleton()->scenario_create();
}

World3D::~World3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());

#ifndef PHYSICS_3D_DISABLED
	ERR_FAIL_NULL(PhysicsServer3D::get_singleton());
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
#endif // NAVIGATION_3D_DISABLED

	RenderingServer::get_singleton()->free_rid(scenario);

#ifndef PHYSICS_3D_DISABLED
	if (space.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(space);
	}
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
	if (navigation_map.is_valid()) {
		NavigationServer3D::get_singleton()->free_rid(navigation_map);
	}
#endif // NAVIGATION_3D_DISABLED
}
