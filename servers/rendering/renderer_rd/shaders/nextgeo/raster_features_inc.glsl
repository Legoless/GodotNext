// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N3)
// The raster-feature table. Copy of RendererVirtualMeshStorage::RasterFeature, RasterPass and
// RasterBin in servers/rendering/storage/virtual_mesh_storage.h: change both in the same commit.
// The VSM page raster (#217) shares it, so each raster feature adds its VSM entry in the same change.

const uint NEXTGEO_RASTER_FEATURE_MASKED = 1u << 0u;
const uint NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET = 1u << 1u;
const uint NEXTGEO_RASTER_FEATURE_DEPTH_OFFSET = 1u << 2u;
const uint NEXTGEO_RASTER_FEATURE_TWO_SIDED = 1u << 3u;
const uint NEXTGEO_RASTER_FEATURE_SKINNED = 1u << 4u;
const uint NEXTGEO_RASTER_FEATURE_DISPLACED = 1u << 5u;
const uint NEXTGEO_RASTER_FEATURE_SPLINE = 1u << 6u;
const uint NEXTGEO_RASTER_FEATURE_VOXEL = 1u << 7u;
const uint NEXTGEO_RASTER_FEATURE_ASSEMBLY = 1u << 8u;

const uint NEXTGEO_RASTER_PASS_VISIBILITY = 0u;
const uint NEXTGEO_RASTER_PASS_VSM = 1u;

// Bins of the visibility pass; the fixed-function bin has no feature bit.
const uint NEXTGEO_RASTER_BIN_FIXED = 0u;
const uint NEXTGEO_RASTER_BIN_COUNT = 1u;

// raster_args: per phase and bin, one software dispatch (uvec4: x and y groups, 1, 0) and one hardware
// draw record (VkDrawIndirectCommand). raster_counts: the hardware draw count of each phase.
const uint NEXTGEO_RASTER_ARGS_SW = 0u;
const uint NEXTGEO_RASTER_ARGS_HW = 1u;
// Software dispatches split their workgroup count over x and y to stay under the portable limit.
const uint NEXTGEO_SW_DISPATCH_WIDTH = 32768u;

// The per-cluster raster lists hold visible-cluster indices into the compacted cluster list, so the
// visibility ID keeps indexing that list. Within a phase's region, software entries grow up from the
// region base and hardware entries grow down from its top; the phase never compacts more clusters
// than the region holds.
uint nextgeo_raster_args_word(uint p_phase, uint p_bin, uint p_kind) {
	return ((p_phase * NEXTGEO_RASTER_BIN_COUNT + p_bin) * 2u + p_kind) * 4u;
}
