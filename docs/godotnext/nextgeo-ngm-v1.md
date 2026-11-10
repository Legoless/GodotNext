# NextGeo `.ngm` v1 format

This is the on-disk contract for NextGeo virtual geometry, as the code implements it. It covers the `.ngm` v1 page file, the 80-byte cluster record, the DAG header and its BVH8 node, the fixed RT proxy layout and the import manifest (#6 exit criteria; plan §6.3, §7.1, §7.2, §7.5). The code is the authority. If this file and the code disagree, the code wins and this file is fixed.

#6 froze these layouts before P5. Two values stay provisional (plan §22.10, v3.4.2): the page size, 64 KiB, and the codec, raw. G4 (#73) measures them in PQ, and #72 records the frozen choice here.

| Part | Where it lives | Code |
|---|---|---|
| Page file (header, page table, pages) | `<mesh>.ngm` sidecar, one per virtualized mesh | `core/io/resource_format_ngm.{h,cpp}` (`NGMWriter`, `NGMFile`, `NGMHandlePool`) |
| Page payload, cluster record, skinning section | Inside each `.ngm` page | `modules/nextgeo/nextgeo_serialize.cpp` (`nextgeo_write_pages`), `modules/nextgeo/nextgeo_dag_builder.h` (`NextGeoCluster`) |
| DAG header (groups and BVH8 nodes), root pages, coarse fallback | `VirtualGeometryData`, saved with the mesh | `modules/nextgeo/nextgeo_serialize.cpp` (`nextgeo_fill_vgd`), `scene/resources/3d/virtual_geometry_data.{h,cpp}` |
| Fixed RT proxy and Stage-2 card descriptors | `VirtualGeometryData` | `modules/nextgeo/nextgeo_rt_proxy.{h,cpp}` |
| Manifest | `<import base path>.ngm_manifest.json` | `editor/import/3d/ngm_manifest.{h,cpp}` |
| Export | The PCK | `editor/export/nextgeo_export_plugin.{h,cpp}` |

## Conventions

- Every multi-byte field is little-endian. `f32` is IEEE 754 binary32, also in double-precision builds.
- `u8`, `u16`, `u32`, `u64`, `i32` and `snorm8` are unsigned, signed and normalized integers of that width.
- Offsets are in bytes from the start of the structure they belong to.
- `NGMWriter` and the payload writer encode byte by byte, so the bytes do not depend on the host. Some readers outside `core/io` (`NGMFile::read_page_cluster_records` for the cluster count, and the runtime in `servers/rendering/storage/virtual_mesh_storage.cpp`) decode with `memcpy` and assume a little-endian host.

## The `.ngm` file

`.ngm` is a sidecar, not a resource. No `ResourceFormatLoader` or saver handles it, and `ngm` is not a resource extension, so the editor file scan and the stock export loop ignore it.

### File layout

```
[header, 128 bytes]
[page table, page_count * 32 bytes]
[page 0 payload][zero padding to 16 bytes]
[page 1 payload][zero padding to 16 bytes]
...
```

The page table always starts at byte 128, directly after the header. Page 0 starts at the first 16-byte boundary after the table. Each later page starts at the first 16-byte boundary after the previous payload. The file ends at the boundary after the last payload. There are no gaps and no trailing bytes.

### Header (128 bytes)

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | `char[4]` | magic | `NGM1` |
| 4 | `u32` | version | 1 |
| 8 | `u32` | header_size | 128 |
| 12 | `u32` | flags | Bit 0 `NGM_FLAG_MESHOPT` (meshopt codec): always clear, because the codec is raw. Bit 1 `NGM_FLAG_SKINNED`: the pages carry the skinning section. Other bits are 0. |
| 16 | `u32` | page_size | The page budget the build used, in bytes. Not a format limit. The importer passes `NGM_PROVISIONAL_PAGE_SIZE` (65536). |
| 20 | `u32` | page_count | Pages in the file |
| 24 | `u32` | root_page_count | Pages `[0, root_page_count)` hold the root groups |
| 28 | `u32` | cluster_count | Clusters in the whole DAG |
| 32 | `u32` | group_count | Cluster groups in the whole DAG |
| 36 | `u32` | dag_depth | DAG levels (maximum group depth + 1) |
| 40 | `u32` | vertex_layout | `NextGeoVertexLayout` bits (below) |
| 44 | `u32` | skin_influences | 0 when unskinned; 4 or 8 when the skinned flag is set |
| 48 | `u64` | page_table_offset | 128 |
| 56 | `u8[32]` | source_sha256 | SHA-256 of the page payloads concatenated in page order, without padding |
| 88 | `u8[40]` | reserved | Zero |

### Page table entry (32 bytes)

| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | `u64` | offset | File offset of the payload, a multiple of 16 |
| 8 | `u32` | stored_size | Payload bytes in the file, without padding |
| 12 | `u32` | raw_size | Payload bytes after decoding. Equal to `stored_size` for the raw codec. |
| 16 | `u32` | first_group | First group in the page, an index into the DAG's group order |
| 20 | `u32` | group_count | Groups in the page. They are `[first_group, first_group + group_count)`. |
| 24 | `u32` | crc32 | ISO-HDLC CRC-32 (zlib `crc32`) of the stored payload bytes, without padding. An empty payload hashes to 0. |
| 28 | `u32` | reserved | Zero |

### Page payload

A page holds whole cluster groups. `NGMFile::read_page` returns this payload after it checks the CRC.

| Order | Content | Size in bytes |
|---|---|---|
| 1 | Page header: `u32` cluster_count, `u32` vertex_count, `u32` triangle_count, `u32` skin_bytes | 16 |
| 2 | `cluster_count` cluster records (next section), in group order | 80 × cluster_count |
| 3 | Vertices: each cluster's vertices in record order, `stride` `f32` values per vertex | 4 × stride × vertex_count |
| 4 | Triangles: each cluster's triangles in record order, three `u8` cluster-local vertex indices per triangle | 3 × triangle_count |
| 5 | Skinning section, only when the skinned flag is set | skin_bytes |

`skin_bytes` is 0 in an unskinned file. (The comment beside `NEXTGEO_PAGE_HEADER_BYTES` in `nextgeo_dag_builder.h` still calls this word "reserved"; the writer stores `skin_bytes` there.) There is no padding inside the payload. `raw_size` is the sum of the five parts.

**Vertex layout.** `vertex_layout` selects the attributes, and the stride is the sum of their widths in floats. They are stored in this order:

| Bit | Attribute | Floats |
|---|---|---|
| 0 | Position | 3 |
| 1 | Normal | 3 |
| 2 | Tangent (w is the binormal sign) | 4 |
| 3 | UV0 | 2 |
| 4 | UV2 | 2 |
| 5 | Color | 4 |

The builder always sets bits 0 to 3. It sets UV2 and color when any eligible surface has them, so the stride is 12, 14, 16 or 18 floats.

**Skinning section.** For each cluster of the page, in record order:

| Content | Size in bytes |
|---|---|
| `u8` bone_count, `u8` influences (4 or 8), `u16` reserved (0) | 4 |
| bone_count entries: `u16` global bone index, then the bone's bind-space AABB of this cluster's vertices that it weights, as `f32` min x, y, z and max x, y, z | 26 × bone_count |
| vertex_count × influences `u8` cluster-local bone indices | vertex_count × influences |
| vertex_count × influences `u16` unorm16 weights | 2 × vertex_count × influences |

A cluster holds at most 16 bones (`NEXTGEO_MAX_CLUSTER_BONES`), except a lone triangle that touches more. An index whose weight is 0 is stored as 0. Bones are sorted by global index. The section sits inside the page so that every stored page, skinning included, stays within `page_size`. The version stays 1: a file without the skinned flag has no section, and a v1 reader that predates the section rejects a skinned file on its unknown flag.

**Page budget.** The builder keeps `align16(16 + the sum of the clusters' bytes)` at most `page_size`. A cluster's bytes are its 80-byte record, its vertices and triangles, and its skinning bytes. So `raw_size` is at most `page_size` too. A build fails when `page_size` cannot hold one full cluster: 128 vertices, 128 triangles and, when skinned, 16 bones.

### Page order and groups

- Root (terminal) groups come first, then the other groups by DAG depth, coarsest first, then in build order. `first_group` indexes this order, and so do the DAG header and `refined_group`.
- Pages `[0, root_page_count)` hold only root groups. The first non-root group opens a new page.
- A group never spans two pages. A new page opens when the next group does not fit in the current one.
- Clusters are numbered in group order, so the clusters of one group, and of one page, are contiguous.
- A parent's page never comes after its child's page.

## The cluster record (80 bytes)

`NextGeoCluster` in `modules/nextgeo/nextgeo_dag_builder.h`. `static_assert(sizeof(NextGeoCluster) == 80)` keeps its size. In a page the writer stores the fields one by one in this order, so the layout does not depend on struct padding.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `f32[4]` | bounds | Culling sphere of the cluster's own geometry: center x, y, z, radius |
| 16 | `snorm8[3]` | cone_axis | Normal cone axis (value / 127) |
| 19 | `snorm8` | cone_cutoff | Normal cone cutoff |
| 20 | `f32[4]` | group_lod | Own LOD sphere: the `parent_lod` of `refined_group`, or the culling sphere for a source cluster |
| 36 | `f32` | group_error | Own error; 0 for a source cluster |
| 40 | `f32[4]` | parent_lod | LOD sphere of the group the cluster belongs to |
| 56 | `f32` | parent_error | Error of that group; `FLT_MAX` for a root group |
| 60 | `i32` | refined_group | The group whose simplification produced this cluster; -1 for a source cluster |
| 64 | `u32` | page_id | The page that holds this cluster's group |
| 68 | `u32` | vertex_offset | First vertex. Page-local in a stored page; global in the builder output. |
| 72 | `u32` | triangle_offset | First triangle. Page-local in a stored page; global in the builder output. |
| 76 | `u8` | vertex_count | At most 128 |
| 77 | `u8` | triangle_count | At most 128 |
| 78 | `u16` | surface_id | Source surface (material) index |

A cluster's triangle indices are relative to its own first vertex. Every cluster of a group carries the group's LOD bound as `parent_lod` and `parent_error` (plan §6.3, v3.4.1).

## The DAG header

`VirtualGeometryData.dag_header`, a `PackedByteArray`:

| Content | Size in bytes |
|---|---|
| `u32` group_count, `u32` bvh_node_count | 8 |
| group_count group records | 60 × group_count |
| bvh_node_count BVH8 nodes | 80 × bvh_node_count |

### Group record (60 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u32` | cluster_offset | First cluster of the group, in global cluster order |
| 4 | `u32` | cluster_count | Clusters in the group |
| 8 | `u32` | page_id | The page that holds the group |
| 12 | `u32` | depth | DAG level the group was formed at; 0 = source clusters |
| 16 | `f32[4]` | parent_lod | Sphere that encloses the members' `group_lod` spheres |
| 32 | `f32` | parent_error | max(simplification error, the members' `group_error`); `FLT_MAX` for a root group |
| 36 | `f32[3]` | aabb_min | Bounds of the members' geometry |
| 48 | `f32[3]` | aabb_max | |

### BVH8 node (80 bytes)

`NextGeoBVHNode` in `modules/nextgeo/nextgeo_bvh.h`, with `static_assert(sizeof(NextGeoBVHNode) == 80)`. Node 0 is the root. The BVH is built over the groups, sorted by the Morton code of their LOD-sphere centers and split 8 ways. It is empty when there are no groups.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `f32[4]` | lod_sphere | Encloses the LOD spheres of every child |
| 16 | `f32` | max_parent_error | Largest parent error in the subtree; `FLT_MAX` if it holds a root group |
| 20 | `f32[3]` | aabb_min | Encloses the geometry of every child |
| 32 | `f32[3]` | aabb_max | |
| 44 | `u32` | child_count | At most 8 |
| 48 | `u32[8]` | children | Bit 31 set (`0x80000000`): a group index (a leaf). Bit 31 clear: a node index. Unused slots: `0xFFFFFFFF`. |

Child spheres are merged with a radius widened in double precision and rounded up, so a parent sphere strictly encloses its children (`nextgeo_enclose_spheres`).

## `VirtualGeometryData`

The sub-resource on `ArrayMesh` that points at the `.ngm`. It is not a `Mesh` subclass. Its `FORMAT_VERSION` is 2; the builder sets it, and no loader checks it today. Its saved fields:

| Field | Content |
|---|---|
| `format_version` | 2 |
| `dag_header` | The DAG header above |
| `root_pages` | The first `root_page_count` page payloads, byte for byte the same as in the `.ngm` |
| `coarse_fallback` | One `Mesh::ARRAY_*` array per source surface: the root cut of each eligible surface, with global bones and float weights for skinned surfaces. An ineligible surface gets an array of `Mesh::ARRAY_MAX` empty entries. |
| `surface_eligibility` | One byte per source surface: 1 = virtualized, 0 = legacy |
| `surface_fallback_reasons` | One int per source surface: 0 when virtualized, otherwise the import-time Fallback value (§6.3) |
| `rt_proxy_*`, `card_descriptors` | The fixed RT proxy (next section) |
| `ngm_path` | Path of the `.ngm` |
| `ngm_sha256` | SHA-256 of the whole `.ngm` file. This differs from the header's `source_sha256`, which covers only the payloads. |
| `source_triangle_count`, `source_vertex_index_bytes` | Source triangles, and 12 bytes per source position plus 4 per source index |
| `cluster_count`, `group_count`, `dag_depth`, `page_count`, `page_size`, `skin_influences` | Copies of the header values |

The importer also keeps the vertex layout and each page's `(first_group, group_count)` pair on the resource for `NGMWriter`. They are not saved.

## The fixed RT proxy

One view-independent proxy per mesh, stored in `VirtualGeometryData` (plan §7.5, D13).

- **Cut.** eps_rt = min(max_extent / 256, `rendering/global_illumination/nextgi/rt_max_error`), in mesh-local units, where max_extent is the largest extent of the AABB of every source surface. Cluster c is selected when `c.group_error <= eps_rt` and `c.parent_error > eps_rt`.
- **Parts.** The selected triangles are split into BLAS parts of at most 131,072 triangles (`NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES`). The cut is never coarsened to meet the cap.
- **Order.** Clusters are packed in BVH8 leaf order, then by cluster index. Within a part, triangles are sorted by surface, stably, so a BLAS geometry index equals the surface index. Positions are welded by exact float32 bit equality, in first-occurrence order.
- **Determinism.** The proxy depends only on the DAG, the AABB and `rt_max_error`. Two builds of the same input are byte-identical.

| Field | Type | Layout |
|---|---|---|
| `rt_proxy_positions` | `PackedFloat32Array` | `f32` x, y, z per vertex, tightly packed |
| `rt_proxy_indices` | `PackedByteArray` | `u32` index per entry, part-local |
| `rt_proxy_parts` | `PackedInt32Array` | Four per BLAS part: vertex_offset, vertex_count, index_offset, index_count. Counted in elements: a part's vertices start at float `3 × vertex_offset`, its indices at byte `4 × index_offset`. |
| `rt_proxy_surface_ranges` | `PackedInt32Array` | Two per (part, surface), part-major: first_index, index_count, relative to the part. A surface with no triangles in the part, every legacy surface included, is [0, 0]. The ranges cover every `ArrayMesh` surface. |
| `rt_proxy_clusters` | `PackedInt32Array` | Selected cluster IDs, in packing order |
| `rt_proxy_triangle_groups` | `PackedByteArray` | `u16` proxy-cut group ID per proxy triangle, in final part and surface order |
| `rt_proxy_eps` | `float` | The eps_rt the cut was taken at |
| `rt_proxy_error` | `float` | Largest `group_error` of the selected clusters; never above eps_rt |
| `card_descriptors` | `PackedByteArray` | 32-byte Stage-2 card descriptors (below), sorted by (group, axis) |

A DAG with no clusters gives an empty proxy: no parts, error 0, eps still set.

**Stage-2 card descriptor (32 bytes).** `NextGeoCardDescriptor`, with `static_assert(sizeof(NextGeoCardDescriptor) == 32)`, plan §8.3 Stage 2, #77.

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u16` | group | Proxy-cut group ID |
| 2 | `u8` | axis | 0 +X, 1 −X, 2 +Y, 3 −Y, 4 +Z, 5 −Z, matching the NextGI Stage-1 axis seeds |
| 3 | `u8` | flags | 0 |
| 4 | `f32[3]` | center | Card center, mesh-local |
| 16 | `f32[3]` | half_extents | Card half extents |
| 28 | `u16` | res_u | Card texels along u |
| 30 | `u16` | res_v | Card texels along v |

## File names and the manifest

### Where each `.ngm` is written

| Import | `.ngm` path |
|---|---|
| Scene, mesh embedded in the scene | `<import base path>-<first 16 hex digits of md5(key)>.ngm` |
| Scene, mesh with `save_to_file` | `<save_to_file path>.ngm` (a `uid://` path is resolved first) |
| Scene imported as a single `ArrayMesh` | `<import base path>.ngm` |
| OBJ | `<import base path>.ngm` |

The import base path is `res://.godot/imported/<file name>-<md5 of the source path>`. The key is `<source path>::<mesh name>`, with the source path localized to `res://`. `NGMWriter` writes `<path>.tmp` first and renames it over the target.

### Manifest

`<import base path>.ngm_manifest.json`, written by `ngm_manifest_commit` as compact JSON on one line, with sorted keys, followed by a newline. With placeholder values:

```json
{"entries":[{"key":"<source path>::<mesh name>","ngm":"<.ngm path>","sha256":"<hex SHA-256 of the whole .ngm file>","source_triangles":0,"source_vertex_index_bytes":0}],"source":"<source path>","version":1}
```

| Key | Value |
|---|---|
| `version` | 1 |
| `source` | The source file, localized to `res://` |
| `entries[].key` | `<source path>::<mesh name>` |
| `entries[].ngm` | The `.ngm` path |
| `entries[].sha256` | Lowercase hex of `VirtualGeometryData.ngm_sha256`, the SHA-256 of the whole `.ngm` file |
| `entries[].source_triangles`, `entries[].source_vertex_index_bytes` | The `VirtualGeometryData` values of the same names |

One entry is written per mesh that produced virtual geometry. A mesh whose build fails imports without virtual geometry, gets a warning and adds no entry.

- **Reimport.** `.ngm` files listed in the previous manifest and not produced again are deleted, `save_to_file` siblings included.
- **Generation off.** With virtual geometry generation off and no entries, an existing manifest is deleted and no manifest is written.
- **Source removed.** `ngm_delete_imported_sidecars` deletes the manifest and every listed `.ngm` under `.godot/imported/`. `res://` siblings stay.

### Export

`NextGeoExportPlugin` adds each `.ngm` to the pack exactly once with `add_file(path, bytes, false)`. For an exported file that has a `.import` file, it reads the manifest at `<import base path>.ngm_manifest.json`. For every exported file it also adds `<file path>.ngm` when that file exists, which covers `save_to_file` meshes. The plugin never calls `skip()`. An encrypted preset that would encrypt a manifest-listed `.ngm` fails before any pack byte is written, with a message to add `*.ngm` to the encryption exclude filters.

## Reading rules

`NGMFile::open` accepts a file only when all of these hold. Otherwise it returns the error shown.

| Check | Error |
|---|---|
| The magic is `NGM1`, and version is 1 | `ERR_FILE_UNRECOGNIZED` |
| No flag bit other than bits 0 and 1 is set | `ERR_FILE_UNRECOGNIZED` |
| The meshopt flag (bit 0) is clear | `ERR_UNAVAILABLE` |
| The file is not an encrypted pack entry | `ERR_UNAVAILABLE` |
| The file holds a full header, `header_size` is 128, `page_size` is not 0, and `root_page_count` is at most `page_count` | `ERR_FILE_CORRUPT` |
| The skinned flag is set exactly when `skin_influences` is 4 or 8 | `ERR_FILE_CORRUPT` |
| Header bytes 88 to 127 are zero, and the page table offset is 128 | `ERR_FILE_CORRUPT` |
| The page table fits in the file. Each entry has reserved 0, `stored_size == raw_size`, a 16-byte-aligned offset after the table and after the previous page, a range inside the file, and a group range inside `group_count`. | `ERR_FILE_CORRUPT` |
| Pages are packed exactly as in the file layout above, the padding bytes are zero, and the file ends at the last page's boundary | `ERR_FILE_CORRUPT` |

`read_page` reads one payload and returns `ERR_FILE_CORRUPT` on a range or CRC failure. It never crashes. `read_page_cluster_records` reads only a page's 16-byte header and its cluster records, without a CRC check. Inside a PCK, reads seek to the packed file's offset plus the page offset. ZIP and directory entries, and loose files, are opened by path. `NGMHandlePool` keeps at most 16 open handles (`NGM_MAX_HANDLES`), as an LRU, and a PCK uses one handle.

`NGMWriter` refuses a header whose version is not 1, whose `page_size` is 0, whose `page_count` does not match the pages, whose `root_page_count` exceeds `page_count`, whose page group ranges leave `group_count`, or whose flags are unknown or inconsistent with `skin_influences`. A meshopt flag returns `ERR_UNAVAILABLE`.

## Determinism

The same header and pages give the same file bytes. The DAG builder gives the same output whether or not it uses threads, and the RT proxy builder gives the same proxy for the same input. So two imports of the same source give the same `.ngm` SHA-256 and the same `VirtualGeometryData` (gate G4-3, measured in PQ).

## Tests that pin this format

- `tests/core/io/test_ngm_file.cpp`: round trip, determinism, rejection of corrupt, truncated and version-mismatched files, and the handle pool.
- `tests/scene/test_virtual_geometry_data.cpp`: the build output in `VirtualGeometryData` and a deterministic `.ngm`, unskinned and skinned.
- `tests/scene/test_importer_mesh_virtual_geometry.cpp`: embedded `.ngm` names and stale-file removal on reimport.
- `tests/editor/test_nextgeo_export.cpp`: the manifest scan, the encryption pre-check and stripping.
- `modules/nextgeo/tests/test_nextgeo_cards.h`: two imports give a bit-identical `.ngm` and card table.
- `tests/servers/rendering/test_nextgeo_residency.cpp`: `.ngm` streaming.
