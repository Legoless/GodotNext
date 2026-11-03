#!/usr/bin/env python3
"""Check a GodotNext export pack for .ngm sidecars.

The printed ratio is sidecar bytes over the sum of source_vertex_index_bytes.
G4-4 also counts mesh-resource bytes and is measured in PQ, so this script
prints the ratio and does not fail on it.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zipfile

PACK_MAGIC = 0x43504447
PACK_DIR_ENCRYPTED = 1 << 0
PACK_FILE_ENCRYPTED = 1 << 0
ZIP_STORED = 0
ZIP_DEFLATED = 8


def md5_text(text):
    return hashlib.md5(text.encode("utf-8")).hexdigest()


def fixture_ngm_paths():
    embedded = "res://tests/export/embedded.gltf"
    prop = "res://tests/export/prop.obj"
    key = embedded + "::Embed_Box"
    embedded_ngm = f".godot/imported/embedded.gltf-{md5_text(embedded)}-{md5_text(key)[:16]}.ngm"
    prop_ngm = f".godot/imported/prop.obj-{md5_text(prop)}.ngm"
    saved_ngm = "tests/export/saved_mesh.res.ngm"
    return [embedded_ngm, prop_ngm, saved_ngm]


def custom_features(blob):
    if len(blob) < 12 or blob[:4] != b"ECFG":
        raise SystemExit("project.binary is not an ECFG file")
    key_len = struct.unpack_from("<I", blob, 8)[0]
    key_end = 12 + key_len
    if key_end > len(blob):
        raise SystemExit("project.binary key overruns the file")
    if blob[12:key_end] != b"_custom_features":
        return ""
    if key_end + 4 > len(blob):
        raise SystemExit("project.binary feature value is truncated")
    value_len = struct.unpack_from("<I", blob, key_end)[0]
    value = blob[key_end + 4 : key_end + 4 + value_len]
    if len(value) < 8:
        raise SystemExit("project.binary feature variant is truncated")
    header = struct.unpack_from("<I", value, 0)[0]
    if (header & 0xFF) != 4:
        raise SystemExit("project.binary custom features are not a string")
    text_len = struct.unpack_from("<I", value, 4)[0]
    return value[8 : 8 + text_len].decode("utf-8")


def has_feature(text, feature):
    parts = [part.strip() for part in text.split(",")]
    return feature in parts


class PackFile:
    def __init__(self, path, size, flags, compress_type, payload):
        self.path = path
        self.size = size
        self.flags = flags
        self.compress_type = compress_type
        self.payload = payload


def parse_pck(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 40:
        raise SystemExit("pck is too small")
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != PACK_MAGIC:
        raise SystemExit("pck magic is not GDPC")
    if version < 2:
        raise SystemExit(f"pck version {version} is too old")
    flags = struct.unpack_from("<I", data, 20)[0]
    if flags & PACK_DIR_ENCRYPTED:
        raise SystemExit("pck directory is encrypted; this check leaves encrypt_directory off")
    file_base = struct.unpack_from("<Q", data, 24)[0]
    dir_base = struct.unpack_from("<Q", data, 32)[0]
    if dir_base + 4 > len(data):
        raise SystemExit("pck directory offset is past the end of the file")
    count = struct.unpack_from("<I", data, dir_base)[0]
    pos = dir_base + 4
    files = []
    for _index in range(count):
        if pos + 4 > len(data):
            raise SystemExit("pck directory entry is truncated")
        padded = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        raw = data[pos : pos + padded]
        pos += padded
        entry_path = raw.split(b"\x00", 1)[0].decode("utf-8")
        offset, size = struct.unpack_from("<QQ", data, pos)
        pos += 16
        pos += 16  # md5
        file_flags = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        payload_off = file_base + offset
        payload = b""
        if entry_path == "project.binary" and not (file_flags & PACK_FILE_ENCRYPTED):
            payload = data[payload_off : payload_off + size]
        files.append(PackFile(entry_path, size, file_flags, None, payload))
    return files


def parse_zip(path):
    files = []
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            payload = b""
            if info.filename == "project.binary":
                payload = archive.read(info)
            files.append(PackFile(info.filename, info.file_size, 0, info.compress_type, payload))
    return files


def iter_manifests(root):
    root = os.path.abspath(root)
    for dirpath, dirnames, filenames in os.walk(root):
        kept = []
        for name in dirnames:
            child = os.path.join(dirpath, name)
            if os.path.abspath(child) != root and os.path.isfile(os.path.join(child, "project.godot")):
                continue
            kept.append(name)
        dirnames[:] = kept
        for name in filenames:
            if name.endswith(".ngm_manifest.json"):
                yield os.path.join(dirpath, name)


def source_vertex_index_bytes(project):
    total = 0
    for manifest in iter_manifests(project):
        with open(manifest, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        for entry in data.get("entries", []):
            if not isinstance(entry, dict):
                continue
            value = entry.get("source_vertex_index_bytes", 0)
            total += int(value or 0)
    return total


def main():
    parser = argparse.ArgumentParser(description="Check .ngm files in a Godot export pack.")
    parser.add_argument("pack")
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--fixtures", action="store_true", help="Expect the three tests/export fixture sidecars.")
    parser.add_argument("--expect-count", type=int)
    parser.add_argument("--project", help="Project root used to sum source_vertex_index_bytes.")
    parser.add_argument("--require-feature")
    parser.add_argument("--forbid-feature")
    parser.add_argument("--require-ngm-unencrypted", action="store_true")
    args = parser.parse_args()

    expected = list(args.expect)
    if args.fixtures:
        expected.extend(fixture_ngm_paths())

    if args.pack.endswith(".zip"):
        files = parse_zip(args.pack)
    else:
        files = parse_pck(args.pack)

    ngm = [item for item in files if item.path.endswith(".ngm")]
    print("ngm files:")
    for item in ngm:
        print(f"  {item.path} ({item.size} bytes)")

    failed = False
    seen = {}
    for item in ngm:
        seen[item.path] = seen.get(item.path, 0) + 1
    for path, count in sorted(seen.items()):
        if count != 1:
            print(f"duplicate .ngm path {path} appears {count} times", file=sys.stderr)
            failed = True
    for path in expected:
        if seen.get(path, 0) != 1:
            print(f"missing .ngm path {path}", file=sys.stderr)
            failed = True
    if args.expect_count is not None and len(ngm) != args.expect_count:
        print(f"expected {args.expect_count} .ngm files, found {len(ngm)}", file=sys.stderr)
        failed = True

    ngm_bytes = 0
    for item in ngm:
        ngm_bytes += item.size
    if args.project:
        source_bytes = source_vertex_index_bytes(args.project)
        if source_bytes:
            ratio = float(ngm_bytes) / float(source_bytes)
            print(f"ngm_bytes={ngm_bytes} source_vertex_index_bytes={source_bytes} ratio={ratio:.4f}")
        else:
            print(f"ngm_bytes={ngm_bytes} source_vertex_index_bytes=0 ratio=n/a")
    else:
        print(f"ngm_bytes={ngm_bytes}")

    if args.pack.endswith(".zip"):
        for item in files:
            if item.path.endswith(".ngm"):
                if item.compress_type != ZIP_STORED:
                    print(f"{item.path} is compressed ({item.compress_type}), expected stored", file=sys.stderr)
                    failed = True
            elif item.compress_type != ZIP_DEFLATED:
                print(f"{item.path} is not deflated ({item.compress_type})", file=sys.stderr)
                failed = True

    if args.require_ngm_unencrypted:
        for item in ngm:
            if item.flags & PACK_FILE_ENCRYPTED:
                print(f"{item.path} is encrypted", file=sys.stderr)
                failed = True

    if args.require_feature or args.forbid_feature:
        blob = b""
        for item in files:
            if item.path == "project.binary":
                blob = item.payload
                break
        if not blob:
            print("project.binary is missing or encrypted", file=sys.stderr)
            return 1
        features = custom_features(blob)
        print(f"custom_features={features}")
        if args.require_feature and not has_feature(features, args.require_feature):
            print(f"custom features lack {args.require_feature}", file=sys.stderr)
            failed = True
        if args.forbid_feature and has_feature(features, args.forbid_feature):
            print(f"custom features include {args.forbid_feature}", file=sys.stderr)
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
