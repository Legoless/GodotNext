# Reference path tracer v0

Analytic path tracer for G0-3 (issue #19, plan D16). GDScript builds a TLAS from a
triangle soup on a local `RenderingDevice` and dispatches `ref_pt.glsl`. The kernel
is near-unbiased: Lambert or Burley diffuse, GGX specular, Russian roulette from
bounce 4 (the first four hits are always kept), a hard stop at 64 bounces, no
clamp and no denoiser. Emissive triangles contribute only when a BSDF sample hits
them. Omni lights use next-event estimation and an opaque shadow ray. Non-physical
Godot units multiply the authored light energy by pi.

A device without `SUPPORTS_RAY_QUERY` prints `GODOTNEXT_RESULT` with
`fallback: NO_RAY_QUERY` and exits 0. The same payload is what `furnace_cpu.gd`
checks. Vulkan on the RTX 4080 SUPER machine is the P2 port (#49).

## Variance

In the furnace every hit adds the current throughput and then multiplies it by
ρ = 0.8. Roulette starts at bounce index 4 with p = min(max channel, 0.95).
A one-million-path double-precision mirror of that estimator has a per-sample
relative standard deviation of about 0.75. At 131072 samples a pixel's standard
deviation is about 0.21 percent of the expected value 5, so the 1 percent gate
is about 4.8 standard deviations.

The cap is the only bias. After 64 Lambert bounces the expected remaining
throughput is ρ^64, about 6.3e-7. The missing tail is that throughput times
1 / (1 − ρ), about 3.1e-6, which is under 1e-5 of the equilibrium radiance.

## Commands

CPU, on this machine or the Linux VM. No window and no ray query:

```
godot --headless --path godotnext/testbed --script res://tests/reference/furnace_cpu.gd
```

The mean must be within 0.2 percent of 5. The script also checks the fallback
payload and that sealed boxes A and B hide the omni light from every dark patch.

GPU, windowed. A headless run has no local `RenderingDevice`. On the Apple M5 Max,
set `GODOTNEXT_BACKGROUND=1` and the Metal validation variables:

```
GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  godot --path godotnext/testbed --rendering-driver metal \
  res://tests/reference/reference.tscn -- \
  --fixture=furnace --spp=131072 --seed=1 --out=/tmp/godotnext-ref-pt/furnace
```

Every pixel of the EXR must be within 1 percent of 5.

```
godot --path godotnext/testbed --rendering-driver metal \
  res://tests/reference/reference.tscn -- \
  --fixture=sealed_a --spp=65536 --seed=1 --out=/tmp/godotnext-ref-pt/sealed-a
```

Repeat with `--fixture=sealed_b`. Q3 is the mean indirect luminance of the 16
dark patches divided by that of the 16 lit patches. It must be under 0.001.
Indirect is the full estimator minus `direct_only`. For a patch, `direct_only`
is emission plus next-event estimation at the patch; the full estimator then
walks cosine-hemisphere paths. For a camera pixel, `direct_only` stops after
emission and next-event estimation on the first hit.

Same seed, twice. The SHA-256 of the saved output must match:

```
godot --path godotnext/testbed --rendering-driver metal \
  res://tests/reference/reference.tscn -- \
  --fixture=furnace --spp=256 --seed=1 --runs=2
```

```
godot --path godotnext/testbed --rendering-driver metal \
  res://tests/reference/reference.tscn -- \
  --fixture=sealed_b --spp=65536 --seed=1 --runs=2
```

The 256-spp determinism run reports `"pass": false`, because the 1 percent
furnace gate is meant for 131072 spp. Its pass rule is the two identical
`sha256` values.

On the RTX 4080 SUPER machine the same fixtures run under Vulkan (#49), through
the Session-1 runner from the M5 Max. The runner adds `--rendering-driver` and
`--gpu-index`, and a `validation` job also sets the validation-layer environment:

```json
{"name": "ref-furnace", "device": "NVIDIA GeForce RTX 4080 SUPER", "driver": "vulkan", "kind": "run",
 "args": ["--path", "godotnext/testbed", "res://tests/reference/reference.tscn", "--",
          "--fixture=furnace", "--spp=131072", "--seed=1", "--out={out}/furnace"],
 "out": "godotnext/testbed/captures/remote/ref-furnace", "timeout_minutes": 30}
```

```sh
godotnext/testbed/harness/runners/run_remote.sh job ref_furnace.json /tmp/ref_remote
```

The same form runs `sealed_a` and `sealed_b` at 65536 spp, the determinism runs,
and a 256-spp furnace as a `validation` job with `--gpu-validation`.

`--rendering-driver d3d12` has no ray query in this build, so it takes the
fallback and exits 0.

## Stored results

`results/metal/` and `results/vulkan/` keep the outputs of the three gated
fixtures under the same names (`furnace.exr`, `furnace.json`, `sealed_a.json`,
`sealed_b.json`): seed 1, furnace at 131072 spp, sealed boxes at 65536 spp.
`results/README.md` records where and when each set was rendered, and the
Metal-versus-Vulkan deltas (#51 reports them in PQ).
