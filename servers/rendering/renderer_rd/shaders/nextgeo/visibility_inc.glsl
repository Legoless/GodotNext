// GodotNext: nextgeo_tessellation (#163) & nextgeo_voxels (#165) shared 25-bit visibility cluster ID partition.
// 32-bit visibility ID = (25-bit vis_cluster << 7) | (7-bit triangle/micro-triangle index), background = 0xFFFFFFFFu.
//   - Normal clusters:            0x00000000u .. 0x017FFFFFu (up to 24M clusters)
//   - Tessellation diced patches: 0x01800000u .. 0x01BFFFFFu (#163; patch_idx = vis_cluster & 0x003FFFFFu)
//   - Nanite Voxel bricks:        0x01C00000u .. 0x01FFFFFFu (reserved for #165; 0x01FFFFFFu | 127 = background)
// Keep in sync with virtual_mesh_storage.h and nextgeo_visibility.h.

const uint NEXTGEO_VIS_NORMAL_MAX = 0x017FFFFFu;
const uint NEXTGEO_VIS_PATCH_BASE = 0x01800000u;
const uint NEXTGEO_VIS_PATCH_MASK = 0x01C00000u;
const uint NEXTGEO_VIS_PATCH_INDEX_MASK = 0x003FFFFFu;
const uint NEXTGEO_VIS_VOXEL_BASE = 0x01C00000u;
const uint NEXTGEO_VIS_VOXEL_MASK = 0x01C00000u;
const uint NEXTGEO_VIS_VOXEL_INDEX_MASK = 0x003FFFFFu;
// Keep 0x01FFFFFFu unused so (0x01FFFFFFu << 7) | 127 = 0xFFFFFFFFu background sentinel is unreachable.
const uint NEXTGEO_VIS_VOXEL_MAX_COUNT = 0x003FFFFFu;

bool nextgeo_vis_is_normal(uint p_vis_cluster) {
	return p_vis_cluster <= NEXTGEO_VIS_NORMAL_MAX;
}

bool nextgeo_vis_is_patch(uint p_vis_cluster) {
	return (p_vis_cluster & NEXTGEO_VIS_PATCH_MASK) == NEXTGEO_VIS_PATCH_BASE;
}

bool nextgeo_vis_is_voxel(uint p_vis_cluster) {
	return (p_vis_cluster & NEXTGEO_VIS_VOXEL_MASK) == NEXTGEO_VIS_VOXEL_BASE;
}

uint nextgeo_vis_patch_index(uint p_vis_cluster) {
	return p_vis_cluster & NEXTGEO_VIS_PATCH_INDEX_MASK;
}

uint nextgeo_vis_pack_patch_id(uint p_patch_index, uint p_micro_tri) {
	return ((NEXTGEO_VIS_PATCH_BASE | (p_patch_index & NEXTGEO_VIS_PATCH_INDEX_MASK)) << 7u) | (p_micro_tri & 0x7Fu);
}

uint nextgeo_vis_voxel_brick_index(uint p_vis_cluster) {
	return p_vis_cluster & NEXTGEO_VIS_VOXEL_INDEX_MASK;
}

uint nextgeo_vis_pack_voxel_id(uint p_brick_index, uint p_voxel_index) {
	return ((NEXTGEO_VIS_VOXEL_BASE | (p_brick_index & NEXTGEO_VIS_VOXEL_INDEX_MASK)) << 7u) | (p_voxel_index & 0x3Fu);
}
