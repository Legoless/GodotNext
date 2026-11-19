#[compute]

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The material Stage A/B resolve compiled as
// compute: the Forward+ fragment template itself (forward_clustered/scene_forward_clustered.glsl, with its
// MODE_VISIBILITY_RESOLVE path), so a bin shades with exactly the material code, analytic derivatives and
// Forward+ lighting of the tile quads. Its variants add NEXTGEO_RESOLVE_COMPUTE: one invocation per pixel
// of a shading bin's list (nextgeo/resolve_compute_inc.glsl), outputs written with imageStore.

// clang-format off
#include_stage "../forward_clustered/scene_forward_clustered.glsl" fragment
// clang-format on
