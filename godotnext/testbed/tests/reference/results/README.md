# Reference path tracer v0 results (#19, #49)

Seed 1. Furnace at 131072 spp, `sealed_a` and `sealed_b` at 65536 spp. Every file
passed its gate: furnace pixels within 1 percent of 5, and Q3 under 0.001.

| Folder | Device | Driver | Rendered |
| --- | --- | --- | --- |
| `metal/` | Apple M5 Max | Metal | 2026-11-13 |
| `vulkan/` | NVIDIA GeForce RTX 4080 SUPER (driver 621.14) | Vulkan | 2026-11-13 |

Metal versus Vulkan, reported only (G2-3 and #51 judge them in PQ):

| Fixture | Metal | Vulkan | Delta |
| --- | --- | --- | --- |
| Furnace mean | 4.9998416 | 4.9998413 | per pixel: mean 3.0e-7, p99 9.2e-6, max 4.3e-5 relative |
| Furnace max deviation from 5 | 0.763 % | 0.763 % | equal |
| `sealed_a` lit indirect luminance | 0.7637527 | 0.7637521 | 7.7e-7 relative |
| `sealed_b` lit indirect luminance | 0.7637520 | 0.7637514 | 8.1e-7 relative |
| Dark indirect luminance (Q3) | 0 | 0 | equal |

Re-render a folder after a change to `ref_pt.glsl`, `reference_pt.gd` or
`soups.gd`, with the commands in `../README.md`.
