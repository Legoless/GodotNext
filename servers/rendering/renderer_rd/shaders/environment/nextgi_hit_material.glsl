#[closest_hit]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_groups (#130). The closest-hit stage of a material's hit group.
#define NEXTGI_HIT_CLOSEST

#include "nextgi_hit_material_inc.glsl"

#[any_hit]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_groups (#130). The any-hit stage of a material's hit group: the alpha part of
// fragment() and the scissor or hash test. Only alpha-tested materials put it in their group.
#define NEXTGI_HIT_ANY

#include "nextgi_hit_material_inc.glsl"
