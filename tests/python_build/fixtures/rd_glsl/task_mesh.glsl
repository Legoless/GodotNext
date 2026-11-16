#[task]

#version 460

#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

layout(local_size_x = 32) in;

void main() {
	EmitMeshTasksEXT(1, 1, 1);
}

#[mesh]

#version 460

#extension GL_EXT_mesh_shader : require

#VERSION_DEFINES

#include "_included.glsl"

layout(local_size_x = 32) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;

void main() {
	SetMeshOutputsEXT(3, 1);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec4 color;

void main() {
	color = vec4(1.0);
}
