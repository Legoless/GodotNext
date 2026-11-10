# NextGI Lumen Lite source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for Lumen Lite (#183).

## No outside input

The irradiance-field gather, the 30% inside test, the 0.4× spacing push, the
half-resolution bilateral upsample, the roughness 0.25 SSR split, the half-size
SRC atlases and the skinned-mesh exclusion were written from the plan (§21.2.2 L6)
and the issue. No Unreal Engine source, shader, plugin, NvRTX sample or decompile
was opened.

## Copied files

None.

## Fix pass (2026-11-09)

The review fixes keep the plan's rules (§21.2 L6) and add one outside input:

- **What:** Ravi Ramamoorthi and Pat Hanrahan, "An Efficient Representation for
  Irradiance Environment Maps", SIGGRAPH 2001.
- **URL:** https://graphics.stanford.edu/papers/envmap/ (project page, last modified
  2001-08-29).
- **License:** a public paper. The page states no license. Nothing was copied.
- **Consulted by the #183 fixer** for the irradiance of a spherical-harmonic radiance
  field: band 0 and band 1 convolve with the clamped cosine as π and 2π/3. The project page
  links the derivation but does not print the two constants; they are the paper's standard
  values.
- **How it is used:** each Lite probe stores L0 and L1 radiance (the least-squares fit,
  3 E[L d] for L1), and the gather evaluates L0 + 2/3 L1 · n as irradiance over π at the
  pixel normal (`nextgi_irradiance_field.glsl`, `nextgi_gather_sdf.glsl`).

The other fixes (the radiance-page allocator sized from the atlas, page sets bound to the
current instance records, the reverse-Z unprojection, the WRC sky miss, the box-filtered
WRC mip 2 and the mip-4/mip-2 rough specular) are engine plumbing with no outside input.
No Unreal Engine source, shader, plugin, NvRTX sample or decompile was opened.
