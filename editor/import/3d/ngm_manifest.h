/**************************************************************************/
/*  ngm_manifest.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/list.h"
#include "scene/resources/3d/importer_mesh.h"

// `<base>-<md5(key) first 16 hex chars>.ngm`. `p_key` is `<source path>::<mesh name>`.
String ngm_embedded_path(const String &p_base_path, const String &p_key);

// Builds virtual geometry at `p_ngm_path`. A builder error is warned and skipped so one
// mesh does not fail the scene import. `p_strip_authored` replaces virtualized surfaces
// with the coarse root cut (OBJ `strip_authored_to_coarse` only). A mesh with no eligible
// surface, or a null hook, adds no entry.
Error ngm_build_mesh(const Ref<ImporterMesh> &p_mesh, const String &p_ngm_path, const String &p_key, bool p_strip_authored, Array &r_entries, bool p_skinned = false);

// Writes `<base>.ngm_manifest.json` and drops `.ngm` files listed in the previous manifest
// that this import no longer produces, including `save_to_file` siblings. When generation
// is off and no manifest exists, nothing is written. `r_gen_files` may be null.
void ngm_manifest_commit(const String &p_base_path, const String &p_source, const Array &p_entries, List<String> *r_gen_files, bool p_generate);

// Deletes the manifest next to the imported resource and every `.ngm` it lists whose path
// contains `.godot/imported/`. `res://` siblings are left in place. Call this before the
// source's `.import` file is removed.
void ngm_delete_imported_sidecars(const String &p_source_file);
