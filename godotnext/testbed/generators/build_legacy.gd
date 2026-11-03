extends RefCounted
## Legacy baseline scenes (#10 step 8): generated originals that exercise
## Godot's built-in GI paths with NextGI off. The G0 baselines are captured
## on these scenes in PQ; nothing here is baked by the generator (VoxelGI
## and LightmapGI bakes are editor operations, recorded in PROVENANCE.md).

const Scaffold := preload("res://generators/scaffold.gd")


static func build_sdfgi() -> Dictionary:
	var built := _build_hall("LegacySdfgi")
	var root: Node3D = built["root"]
	var environment: Environment = built["environment"]
	environment.sdfgi_enabled = true
	return {"root": root, "cameras": ["CaptureCam"]}


static func build_voxelgi() -> Dictionary:
	var built := _build_hall("LegacyVoxelgi")
	var root: Node3D = built["root"]
	var environment: Environment = built["environment"]
	environment.ssr_enabled = true
	environment.ssao_enabled = true
	environment.ssil_enabled = true

	var voxel := VoxelGI.new()
	voxel.name = "VoxelGI"
	voxel.size = Vector3(14.0, 6.0, 10.0)
	voxel.subdiv = VoxelGI.SUBDIV_128
	voxel.position = Vector3(0.0, 2.0, 0.0)
	Scaffold.add_owned(root, voxel, root)

	var probe := ReflectionProbe.new()
	probe.name = "ReflectionProbe"
	probe.size = Vector3(10.0, 4.0, 8.0)
	probe.position = Vector3(0.0, 2.0, 0.0)
	probe.update_mode = ReflectionProbe.UPDATE_ONCE
	Scaffold.add_owned(root, probe, root)
	return {"root": root, "cameras": ["CaptureCam"]}


static func build_lightmap() -> Dictionary:
	var built := _build_hall("LegacyLightmap")
	var root: Node3D = built["root"]
	var environment: Environment = built["environment"]
	environment.volumetric_fog_enabled = true
	environment.fog_enabled = true

	var lightmap := LightmapGI.new()
	lightmap.name = "LightmapGI"
	Scaffold.add_owned(root, lightmap, root)

	var gradient := Gradient.new()
	gradient.set_color(0, Color(0.9, 0.3, 0.2, 1.0))
	gradient.set_color(1, Color(0.9, 0.3, 0.2, 0.0))
	var decal_texture := GradientTexture2D.new()
	decal_texture.gradient = gradient
	decal_texture.fill = GradientTexture2D.FILL_RADIAL
	decal_texture.fill_from = Vector2(0.5, 0.5)
	decal_texture.fill_to = Vector2(1.0, 0.5)
	var decal := Decal.new()
	decal.name = "FloorDecal"
	decal.texture_albedo = decal_texture
	decal.size = Vector3(3.0, 2.0, 3.0)
	decal.position = Vector3(0.0, 1.0, 0.0)
	Scaffold.add_owned(root, decal, root)

	var fog_material := FogMaterial.new()
	fog_material.density = 0.05
	var fog_volume := FogVolume.new()
	fog_volume.name = "FogVolume"
	fog_volume.material = fog_material
	fog_volume.size = Vector3(6.0, 2.0, 6.0)
	fog_volume.position = Vector3(2.0, 1.0, 0.0)
	Scaffold.add_owned(root, fog_volume, root)

	var sun := root.get_node("Sun") as DirectionalLight3D
	sun.light_bake_mode = Light3D.BAKE_STATIC
	return {"root": root, "cameras": ["CaptureCam"]}


## Shared open hall: floor, four low walls, colored and emissive blocks, a
## sun and an omni light under a procedural sky.
static func _build_hall(p_name: String) -> Dictionary:
	var root := Node3D.new()
	root.name = p_name

	var gray := Scaffold.make_constant_material("Grey075", Color(0.75, 0.75, 0.75))
	var red := Scaffold.make_constant_material("RedBlock", Color(0.75, 0.05, 0.05))
	var green := Scaffold.make_constant_material("GreenBlock", Color(0.05, 0.75, 0.05))
	var emissive := Scaffold.make_constant_material(
			"EmissiveBlock", Color(0.2, 0.2, 0.2), 1.0, 0.0, 0.0, Color(1.0, 0.8, 0.5), 2.0)

	var sky_material := ProceduralSkyMaterial.new()
	var sky := Sky.new()
	sky.sky_material = sky_material
	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	environment.sky = sky
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = environment
	Scaffold.add_owned(root, world_env, root)

	Scaffold.add_box_instance(root, root, "Floor",
			Vector3(0.0, -0.15, 0.0), Vector3(14.0, 0.3, 10.0), gray)
	Scaffold.add_box_instance(root, root, "NorthWall",
			Vector3(0.0, 2.0, -4.85), Vector3(14.0, 4.0, 0.3), gray)
	Scaffold.add_box_instance(root, root, "SouthWall",
			Vector3(0.0, 2.0, 4.85), Vector3(14.0, 4.0, 0.3), gray)
	Scaffold.add_box_instance(root, root, "WestWall",
			Vector3(-6.85, 2.0, 0.0), Vector3(0.3, 4.0, 9.4), red)
	Scaffold.add_box_instance(root, root, "EastWall",
			Vector3(6.85, 2.0, 0.0), Vector3(0.3, 4.0, 9.4), green)
	Scaffold.add_box_instance(root, root, "BlockLarge",
			Vector3(-2.0, 0.6, -1.0), Vector3(1.2, 1.2, 1.2), gray)
	Scaffold.add_box_instance(root, root, "BlockEmissive",
			Vector3(2.5, 0.4, 1.5), Vector3(0.8, 0.8, 0.8), emissive)
	Scaffold.add_box_instance(root, root, "BlockTall",
			Vector3(0.5, 1.0, -2.5), Vector3(0.8, 2.0, 0.8), gray)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	var elevation := deg_to_rad(40.0)
	var azimuth := deg_to_rad(30.0)
	var direction := Vector3(
			-cos(elevation) * sin(azimuth), -sin(elevation), -cos(elevation) * cos(azimuth))
	sun.transform = Transform3D(Basis.looking_at(direction.normalized(), Vector3.UP), Vector3.ZERO)
	sun.light_energy = 1.0
	sun.shadow_enabled = true
	Scaffold.add_owned(root, sun, root)

	var omni := OmniLight3D.new()
	omni.name = "HallLight"
	omni.position = Vector3(0.0, 3.0, 0.0)
	omni.light_energy = 1.0
	omni.omni_range = 10.0
	omni.shadow_enabled = true
	Scaffold.add_owned(root, omni, root)

	Scaffold.make_camera(root, root, "CaptureCam",
			Vector3(-5.5, 3.5, 5.5), Vector3(0.0, 0.8, 0.0), 100.0, 65.0, true)
	return {"root": root, "environment": environment}
