extends Node

## Builds a #version 460 GL_EXT_ray_query compute pipeline with a TLAS binding (#48).
##
## Ray query stays unreported until acceleration structures land (#47), so this does not
## dispatch. It compiles the shader and creates the pipeline, which is what Metal shader
## validation checks.
##
##   GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
##     godot --path godotnext/testbed --rendering-driver metal res://tests/metal_ray_query.tscn

const KERNEL := """#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict writeonly buffer Hits {
	float data[];
}
hits;

void main() {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, vec3(0.0), 0.0, vec3(0.0, 0.0, -1.0), 10.0);
	rayQueryProceedEXT(rq);
	hits.data[gl_GlobalInvocationID.x] = rayQueryGetIntersectionTEXT(rq, true);
}
"""


func _ready() -> void:
	var result := _run()
	print("GODOTNEXT_RESULT " + JSON.stringify(result))
	get_tree().quit(0 if result["pass"] else 1)


func _run() -> Dictionary:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		return { "pass": false, "reason": "NO_RENDERING_DEVICE" }

	var failures := PackedStringArray()
	var ray_query := rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY)
	if ray_query:
		failures.append("SUPPORTS_RAY_QUERY is true before acceleration structures land")
	if rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE):
		failures.append("SUPPORTS_RAYTRACING_PIPELINE is true")

	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = KERNEL
	var spirv := rd.shader_compile_spirv_from_source(source)
	if not spirv.compile_error_compute.is_empty():
		failures.append("compile: " + spirv.compile_error_compute)
	var shader := rd.shader_create_from_spirv(spirv, "metal_ray_query")
	if not shader.is_valid():
		failures.append("shader_create_from_spirv failed")
	else:
		var pipeline := rd.compute_pipeline_create(shader)
		if not pipeline.is_valid():
			failures.append("compute_pipeline_create failed")
		else:
			rd.free_rid(pipeline)
		rd.free_rid(shader)
	rd.free()

	return {
		"pass": failures.is_empty(),
		"device": RenderingServer.get_video_adapter_name(),
		"ray_query": ray_query,
		"failures": failures,
	}
