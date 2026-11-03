# Metal RHI source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for the Metal ray-query driver: what it is, where
it came from, its license, and how it was used.

## Approved inputs

### SPIRV-Cross commit df0828ca, MSL AABB-opaque fix

| Field | Value |
|---|---|
| What | One-line MSL fix: `OpRayQueryGetIntersectionCandidateAABBOpaqueKHR` must invert `is_candidate_non_opaque_bounding_box()` |
| Upstream | https://github.com/KhronosGroup/SPIRV-Cross/commit/df0828ca34d6e94ce23ccf211abe6967591dfdda |
| Version | df0828ca34d6e94ce23ccf211abe6967591dfdda (2026-09-01), applied onto vendored snapshot fb0c1a307cca4b4a9d891837bf4c44d17fe2d324 |
| License | Apache-2.0 |
| Read on | 2026-11-03, for issue #48 |
| Consulted by | The #48 implementer |
| Used in | `thirdparty/spirv-cross/spirv_msl.cpp`, recorded as `thirdparty/spirv-cross/patches/0001-msl-aabb-opaque.patch` |
| Copied files | The `spirv_msl.cpp` hunk only. That commit also updates two reference `.comp` shaders; those files are not part of the vendored snapshot and were not copied. |

How it was used: the public commit was fetched and the `spirv_msl.cpp` change was applied verbatim. The vendored file's line number differs from upstream (10396 here, 10542 in that commit) because this tree is snapshot fb0c1a30. `0002-msl-argument-buffer-padding-as.patch` is local and is not an outside source. It treats `AccelerationStructure` as a buffer in `add_msl_resource_binding` and in the argument-buffer padding switch, which this snapshot does not do.
