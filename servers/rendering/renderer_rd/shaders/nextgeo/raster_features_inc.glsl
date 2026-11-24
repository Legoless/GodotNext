// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N3)
// The raster-feature table. Copy of RendererVirtualMeshStorage::RasterFeature, RasterPass and
// RasterBin in servers/rendering/storage/virtual_mesh_storage.h: change both in the same commit.
// The VSM page raster (#217) shares it, so each raster feature adds its VSM entry in the same change.

const uint NEXTGEO_RASTER_FEATURE_MASKED = 1u << 0u; // #153: visibility + VSM depth-only coverage (alpha scissor, alpha hash, discard)
const uint NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET = 1u << 1u;
const uint NEXTGEO_RASTER_FEATURE_DEPTH_OFFSET = 1u << 2u;
const uint NEXTGEO_RASTER_FEATURE_TWO_SIDED = 1u << 3u; // #153: visibility + VSM cull_disabled / cull_front (FIXED when opaque, programmable when combined)
const uint NEXTGEO_RASTER_FEATURE_SKINNED = 1u << 4u;
const uint NEXTGEO_RASTER_FEATURE_DISPLACED = 1u << 5u;
const uint NEXTGEO_RASTER_FEATURE_SPLINE = 1u << 6u;
const uint NEXTGEO_RASTER_FEATURE_VOXEL = 1u << 7u;
const uint NEXTGEO_RASTER_FEATURE_ASSEMBLY = 1u << 8u;

// GodotNext: nextgeo_masked_two_sided (#153). Pixel-programmable features demote to FIXED bin past
// programmable_raster_distance unless paired with vertex-programmable features.
const uint NEXTGEO_RASTER_PIXEL_PROGRAMMABLE_FEATURES = NEXTGEO_RASTER_FEATURE_MASKED | NEXTGEO_RASTER_FEATURE_DEPTH_OFFSET;
const uint NEXTGEO_RASTER_VERTEX_PROGRAMMABLE_FEATURES = NEXTGEO_RASTER_FEATURE_VERTEX_OFFSET | NEXTGEO_RASTER_FEATURE_DISPLACED;
const uint NEXTGEO_RASTER_PROGRAM_FEATURES = NEXTGEO_RASTER_PIXEL_PROGRAMMABLE_FEATURES | NEXTGEO_RASTER_VERTEX_PROGRAMMABLE_FEATURES;

const uint NEXTGEO_RASTER_PASS_VISIBILITY = 0u;
const uint NEXTGEO_RASTER_PASS_VSM = 1u;

// Bins of the visibility pass; the fixed-function bin has no feature bit.
const uint NEXTGEO_RASTER_BIN_FIXED = 0u;
const uint NEXTGEO_RASTER_BIN_COUNT = 1u;

// raster_args: per phase and bin, one software dispatch (uvec4: x and y groups, 1, 0), one hardware
// VS draw record (VkDrawIndirectCommand), and one hardware mesh-task dispatch record
// (VkDrawMeshTasksIndirectCommandEXT: ceil(hw_clusters / 32), 1, 1, 0; #154).
// raster_counts: the hardware draw count of each phase (shared by VS and mesh indirect-count draws).
const uint NEXTGEO_RASTER_ARGS_SW = 0u;
const uint NEXTGEO_RASTER_ARGS_HW = 1u;
// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9).
const uint NEXTGEO_RASTER_ARGS_HW_MESH = 2u;
const uint NEXTGEO_RASTER_ARGS_KINDS = 3u;
// Software dispatches split their workgroup count over x and y to stay under the portable limit.
const uint NEXTGEO_SW_DISPATCH_WIDTH = 32768u;

// The per-cluster raster lists hold visible-cluster indices into the compacted cluster list, so the
// visibility ID keeps indexing that list. Within a phase's region, software entries grow up from the
// region base and hardware entries grow down from its top; the phase never compacts more clusters
// than the region holds.
uint nextgeo_raster_args_word(uint p_phase, uint p_bin, uint p_kind) {
	return ((p_phase * NEXTGEO_RASTER_BIN_COUNT + p_bin) * NEXTGEO_RASTER_ARGS_KINDS + p_kind) * 4u;
}

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). Programmable raster bins: bin 0 is the
// fixed-function bin above; bins 1 to NEXTGEO_RASTER_BIN_LIMIT - 1 run a material's raster program
// (raster_material_inc.glsl). Copies of nextgeo_raster_bins.h and
// RendererVirtualMeshStorage::RASTER_BIN_LIMIT: change both in the same commit.
// A VSM page raster (#217) iterates the same bins with NEXTGEO_RASTER_DEPTH_ONLY: vertex() plus
// coverage, no visibility ID.
const uint NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX = 64u;
const uint NEXTGEO_RASTER_BIN_LIMIT = NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + 1u;
const uint NEXTGEO_RASTER_BIN_KIND_SW = 0u;
const uint NEXTGEO_RASTER_BIN_KIND_HW = 1u;
// Per (instance, surface): one byte per surface, indexed like the bucket table (bucket_base + surface).
const uint NEXTGEO_RASTER_BIN_TABLE_WORDS_PER_RECORD = 8u;
// A cull entry: visible-cluster index | kind << 21 | (bin - 1) << 22.
const uint NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS = 21u;
const uint NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT = 21u;
const uint NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT = 22u;
// Words of the bin state buffer and the dedicated indirect-arguments buffer.
const uint NEXTGEO_RASTER_BIN_STATE_COUNTS = 0u; // [phase][bin - 1][kind] cluster counts
const uint NEXTGEO_RASTER_BIN_STATE_CURSORS = 256u; // [phase][bin - 1][kind] range starts in the phase's region
const uint NEXTGEO_RASTER_BIN_STATE_FILL = 512u; // [phase][bin - 1][kind] scatter fill counters
const uint NEXTGEO_RASTER_BIN_STATE_APPENDED = 768u; // [phase] appended entries
const uint NEXTGEO_RASTER_BIN_ARGS_BINS = 0u; // [phase][bin - 1] SW uvec4 dispatch, then HW uvec4 draw
const uint NEXTGEO_RASTER_BIN_ARGS_SCATTER = 1024u; // [phase] uvec4 scatter dispatch
// GodotNext: nextgeo_mesh_raster (#154). [phase][bin - 1] HW mesh-task uvec4 indirect dispatch.
const uint NEXTGEO_RASTER_BIN_ARGS_HW_MESH = 1032u;
const uint NEXTGEO_RASTER_BIN_ARGS_WORDS = 1544u;
const uint NEXTGEO_RASTER_BIN_HW_COUNTS_WORDS = 128u; // [phase][bin - 1] indirect draw count (0 or 1)

uint nextgeo_raster_bin_table_word(uint p_bucket_base, uint p_surface) {
	return (p_bucket_base + p_surface) >> 2u;
}

uint nextgeo_raster_bin_table_shift(uint p_surface) {
	return (p_surface & 3u) * 8u;
}

uint nextgeo_raster_bin_count_word(uint p_phase, uint p_bin, uint p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_COUNTS + (p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

uint nextgeo_raster_bin_cursor_word(uint p_phase, uint p_bin, uint p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_CURSORS + (p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

uint nextgeo_raster_bin_fill_word(uint p_phase, uint p_bin, uint p_kind) {
	return NEXTGEO_RASTER_BIN_STATE_FILL + (p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 2u + p_kind;
}

uint nextgeo_raster_bin_args_word(uint p_phase, uint p_bin, uint p_kind) {
	return NEXTGEO_RASTER_BIN_ARGS_BINS + (p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 8u + p_kind * 4u;
}

uint nextgeo_raster_bin_scatter_args_word(uint p_phase) {
	return NEXTGEO_RASTER_BIN_ARGS_SCATTER + p_phase * 4u;
}

uint nextgeo_raster_bin_hw_mesh_args_word(uint p_phase, uint p_bin) {
	return NEXTGEO_RASTER_BIN_ARGS_HW_MESH + (p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u)) * 4u;
}

uint nextgeo_raster_bin_hw_count_word(uint p_phase, uint p_bin) {
	return p_phase * NEXTGEO_RASTER_BIN_PROGRAMMABLE_MAX + (p_bin - 1u);
}

uint nextgeo_raster_bin_pack_entry(uint p_visible_index, uint p_bin, uint p_kind) {
	return (p_visible_index & ((1u << NEXTGEO_RASTER_BIN_ENTRY_INDEX_BITS) - 1u)) | ((p_kind & 1u) << NEXTGEO_RASTER_BIN_ENTRY_KIND_SHIFT) | (((p_bin - 1u) & 0x3Fu) << NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT);
}

uint nextgeo_raster_bin_entry_bin(uint p_entry) {
	return ((p_entry >> NEXTGEO_RASTER_BIN_ENTRY_BIN_SHIFT) & 0x3Fu) + 1u;
}
