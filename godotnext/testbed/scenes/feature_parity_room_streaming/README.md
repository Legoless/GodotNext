# Feature Parity Room, streaming variant (#74)

A separate folder that instances `scenes/feature_parity_room/feature_parity_room.tscn`.
The #64 room, its `override.cfg` and `project.godot` are unchanged.
This folder's `override.cfg` turns on both

- `rendering/lights_and_shadows/multi_bounce_occlusion/enabled`
- `rendering/textures/streaming/enabled`

The harness copies that file to `res://override.cfg` only for this scene
and removes it afterwards. Do not move either key into `project.godot`.

Cameras stay `cam_0`, `cam_1` and `cam_2` on the instanced room.

```
godot --headless --path godotnext/testbed --script res://generators/build_feature_parity_streaming.gd
godot --headless --path godotnext/testbed --script res://scenes/feature_parity_room_streaming/check_streaming.gd
```

The scene text is written by the generator. Do not edit the `.tscn` by hand.
