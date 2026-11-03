# Slow IO Flight (#74)

A seeded spline through the Dense City control points, 3600 frames at
60 fps (60 s). The delayed variant records a uniform 50–250 ms IO delay
per request, applied in wall time. The nodelay variant is the same path
with the delay fixed at 0 and is the G5-2 crack and hole flight. This
scene does not sleep. `rendering/nextgeo/streaming/debug_io_delay_ms` is
not in the engine yet. When #78 adds it, the delayed variant stores the
lower bound (one integer cannot hold a range) and the report still
carries `min_ms` and `max_ms` for the streamer to sample per request.

The saved scene uses eight stand-in boxes. It does not instance Dense
City. After that generator has run, `--with-city=1` instances
`res://generated/dense_city/dense_city.tscn`. Capture identity runs leave
the flag off.

The virtual-variant working set in `generators/dense_city/expected.json`
is `unique_bytes / geometry_cache_budget_bytes` and is at least 2× the
128 MiB `rendering/limits/nextgeo/geometry_cache_mb` budget. The city
itself is not generated here.

```
godot --headless --path godotnext/testbed --script res://generators/build_slow_io_flight.gd
godot --headless --path godotnext/testbed --script res://scenes/slow_io_flight/check_slow_io.gd
```

Seed stream 75. Do not edit `slow_io_flight.tscn` by hand.

## Reports

An absolute `--out` writes `slow_io_params.json` (schema 1): variant,
seed, frame count, the delay record (`applied_in: wall_time`, `gate:
false`) and `page_upload_ms`. Page-upload samples stay null until #78
exposes `RenderingServer.virtual_geometry_get_routing_counters`.

Frame-time p50, p95 and p99 come from a harness `timing.csv` (`wall_ms`)
when one is present. The p99 ≤ 2.0× median check is PQ (#84) and the
report leaves `pass` null. `record_complete` is true when the delay
record is wall time and the percentile fields exist.

Q15 reads `memory_report.json`. The collector writes the ten category
names with `source: absent` immediately, because
`RenderingServer.virtual_geometry_get_memory_report` is not in this
build. When the method exists it warms up for 120 frames on `cam_main`
and `cam_aerial` and fills one `--export-mode`, merging into an existing
file:

```
godot --path godotnext/testbed --script res://scenes/slow_io_flight/collect_memory.gd -- --out=/tmp/q15 --export-mode=strip_authored_to_coarse
```

Repeat with `discrete_lod` and `retain_authored`. `retain_authored` is
reported and is not part of the stripped/baseline ratio.

## Export presets

`export_presets.cfg` is intentionally absent. Add these presets when the
exported Q15 run happens, then:

```
godot --headless --path godotnext/testbed --export-release "<preset>" <out>
```

| Preset | Scene | Role |
|---|---|---|
| Dense City Stripped | `res://generated/dense_city/dense_city.tscn` | custom feature `nextgeo`; ratio numerator |
| Dense City LOD Baseline | `res://generated/dense_city/dense_city_lod_baseline.tscn` | discrete LODs; ratio denominator |
| Dense City Retain Authored | the retain-authored export | reported only, not gated |
| Slow IO Flight PCK | `res://scenes/slow_io_flight/slow_io_flight.tscn` | exported-PCK flight for #78 |

Those exports, and the Q15 numbers on them, are deferred to the RTX
4080 SUPER machine.
