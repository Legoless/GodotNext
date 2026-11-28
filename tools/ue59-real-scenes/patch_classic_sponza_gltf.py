#!/usr/bin/env python3
"""Patch ClassicSponza.gltf exported from ClassicSponzaUnreal with all 68 exported
Unreal Texture2D PNGs (Base Map -> baseColorTexture, Normal Map -> normalTexture,
HDRP MaskMap (R=Metal, G=AO, A=Smoothness) -> glTF metallicRoughnessTexture,
plus alphaMode=MASK for foliage/ivy) and strip SM_SkySphere."""
import concurrent.futures
import json
import os
import shutil
import sys
from pathlib import Path

for dist_dir in ("/usr/lib/python3/dist-packages", "/usr/local/lib/python3.12/dist-packages"):
    if os.path.isdir(dist_dir) and dist_dir not in sys.path:
        sys.path.append(dist_dir)

import numpy as np
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = Path(os.environ.get("GODOTNEXT_ROOT") or SCRIPT_DIR.parents[1])
TOOLS_DIR = Path(os.environ.get("GODOTNEXT_TOOLS_DIR") or REPO_ROOT / "local/godotnext-tools")
SRC_DIR = TOOLS_DIR / "ue59-real-scenes/exports/classic_sponza_tex"
RAW_TEX = SRC_DIR / "raw_tex"
DST_DIR = TOOLS_DIR / "ue59-real-scenes/classic_sponza_pbr"
TEX_DIR = DST_DIR / "textures"


def find_exported_png(ue_pkg: str) -> Path | None:
    # /Game/ClassicSponza/Art/.../Foo -> raw_tex/Game/ClassicSponza/Art/.../Foo.PNG
    rel = ue_pkg.lstrip("/")
    for ext in (".PNG", ".png", ".TGA", ".tga"):
        p = RAW_TEX / (rel + ext)
        if p.exists():
            return p
    # Fallback by stem
    stem = Path(rel).name.lower()
    for p in RAW_TEX.rglob("*"):
        if p.is_file() and p.stem.lower() == stem:
            return p
    return None


def convert_tex(task):
    kind, src, dst, has_alpha = task
    if dst.exists():
        return str(dst)
    im = Image.open(src)
    if max(im.size) > 2048:
        scale = 2048.0 / float(max(im.size))
        im = im.resize((max(1, int(im.size[0] * scale)), max(1, int(im.size[1] * scale))), Image.Resampling.LANCZOS)
    if kind == "diffuse":
        im = im.convert("RGBA" if has_alpha else "RGB")
        im.save(dst, compress_level=3)
    elif kind == "normal":
        arr = np.asarray(im.convert("RGB"), dtype=np.uint8).copy()
        # UE exports DX-style normal maps (green channel inverted relative to glTF OpenGL normal)
        arr[:, :, 1] = 255 - arr[:, :, 1]
        Image.fromarray(arr, mode="RGB").save(dst, compress_level=3)
    elif kind == "maskmap":
        # HDRP/ClassicSponza MaskMap: R=Metallic, G=AO, B=Detail, A=Smoothness
        arr = np.asarray(im.convert("RGBA"), dtype=np.uint8)
        out = np.empty((arr.shape[0], arr.shape[1], 3), dtype=np.uint8)
        out[:, :, 0] = arr[:, :, 1]  # R = AO
        out[:, :, 1] = np.clip(255 - arr[:, :, 3], 20, 255)  # G = Roughness = 1 - Smoothness
        out[:, :, 2] = arr[:, :, 0]  # B = Metallic
        Image.fromarray(out, mode="RGB").save(dst, compress_level=3)
    return str(dst)


def main():
    DST_DIR.mkdir(parents=True, exist_ok=True)
    TEX_DIR.mkdir(parents=True, exist_ok=True)

    # Copy binary buffer
    src_bin = SRC_DIR / "ClassicSponza.bin"
    dst_bin = DST_DIR / "ClassicSponza.bin"
    if not dst_bin.exists():
        os.symlink(src_bin, dst_bin)

    g = json.load(open(SRC_DIR / "ClassicSponza.gltf"))
    ue_mats = json.load(open(SRC_DIR / "ue_materials.json"))
    mesh_mats_data = json.load(open(SRC_DIR / "mesh_mats.json"))
    mesh_mats = mesh_mats_data["mesh_mats"]

    # Map glTF material index -> UE material name via mesh name & primitive slot index!
    gltf_mat_to_ue = {}
    for m in g.get("meshes", []):
        mname = m.get("name", "")
        # Strip trailing .001 or numeric suffix if needed
        ue_list = mesh_mats.get(mname)
        if ue_list is None:
            # Try stripping digits at end (e.g. Vase_Round_Plants_B003 -> Vase_Round_Plants_B)
            base = mname.rstrip("0123456789")
            ue_list = mesh_mats.get(base)
        if ue_list:
            for pi, prim in enumerate(m.get("primitives", [])):
                mi = prim.get("material")
                if mi is not None and pi < len(ue_list) and ue_list[pi]:
                    gltf_mat_to_ue.setdefault(mi, ue_list[pi])

    print(f"Mapped {len(gltf_mat_to_ue)}/{len(g.get('materials', []))} glTF materials to UE MaterialInstances")
    for k, v in sorted(gltf_mat_to_ue.items()):
        print(f"  mat[{k:2d}] -> {v}")

    # Hide/remove SM_SkySphere nodes so it never blocks sun/sky in GodotNext
    sky_mesh_idxs = {i for i, m in enumerate(g.get("meshes", [])) if "skysphere" in m.get("name", "").lower()}
    for n in g.get("nodes", []):
        if n.get("mesh") in sky_mesh_idxs:
            n.pop("mesh", None)
            print(f"Stripped mesh from sky node: {n.get('name')}")

    images = []
    textures = []
    uri_to_tex = {}
    tasks = {}

    def reg_tex(kind: str, ue_pkg: str, has_alpha: bool = False) -> int | None:
        src = find_exported_png(ue_pkg)
        if src is None:
            return None
        stem = src.stem
        out_name = f"{kind}_{stem}.png"
        dst = TEX_DIR / out_name
        tasks[str(dst)] = (kind, src, dst, has_alpha)
        rel = f"textures/{out_name}"
        if rel not in uri_to_tex:
            img_i = len(images)
            images.append({"uri": rel, "mimeType": "image/png"})
            tex_i = len(textures)
            textures.append({"source": img_i, "sampler": 0})
            uri_to_tex[rel] = tex_i
        return uri_to_tex[rel]

    new_mats = []
    for mi, old_m in enumerate(g.get("materials", [])):
        ue_name = gltf_mat_to_ue.get(mi, "")
        uinfo = ue_mats.get(ue_name)
        nm = {"name": ue_name or f"mat_{mi}", "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}}
        if uinfo:
            texs = uinfo.get("textures", {})
            is_foliage = any(k in ue_name.lower() for k in ("ivy", "plant", "leaf", "foliage", "bush"))
            if is_foliage:
                nm["alphaMode"] = "MASK"
                nm["alphaCutoff"] = 0.45
                nm["doubleSided"] = True
            if "fabric" in ue_name.lower() or "curtain" in ue_name.lower():
                nm["doubleSided"] = True

            # Diffuse / Base Map
            base_pkg = texs.get("Base Map") or texs.get("Diffuse") or texs.get("BaseColor") or next((v for k, v in texs.items() if "diff" in v.lower() or "base" in k.lower()), None)
            if base_pkg:
                ti = reg_tex("diffuse", base_pkg, has_alpha=is_foliage)
                if ti is not None:
                    nm["pbrMetallicRoughness"]["baseColorTexture"] = {"index": ti}

            # Normal Map
            nrm_pkg = texs.get("Normal Map") or texs.get("Normal") or next((v for k, v in texs.items() if "normal" in v.lower()), None)
            if nrm_pkg:
                ti = reg_tex("normal", nrm_pkg, has_alpha=False)
                if ti is not None:
                    nm["normalTexture"] = {"index": ti}

            # Mask Map (HDRP -> glTF MetallicRoughness + Occlusion)
            mask_pkg = texs.get("Mask Map") or next((v for k, v in texs.items() if "maskmap" in v.lower()), None)
            if mask_pkg:
                ti = reg_tex("maskmap", mask_pkg, has_alpha=False)
                if ti is not None:
                    nm["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {"index": ti}
                    nm["pbrMetallicRoughness"]["metallicFactor"] = 1.0
                    nm["pbrMetallicRoughness"]["roughnessFactor"] = 1.0
                    nm["occlusionTexture"] = {"index": ti, "strength": 0.65}
        new_mats.append(nm)

    print(f"Converting {len(tasks)} ClassicSponza textures to standard glTF 2.0 PNGs...", flush=True)
    with concurrent.futures.ProcessPoolExecutor(max_workers=min(24, os.cpu_count() or 8)) as ex:
        list(ex.map(convert_tex, tasks.values()))

    g["materials"] = new_mats
    g["textures"] = textures
    g["images"] = images
    g["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
    out_gltf = DST_DIR / "ClassicSponza.gltf"
    out_gltf.write_text(json.dumps(g, separators=(",", ":")))
    print(f"Wrote {out_gltf}: {len(new_mats)} mats, {len(textures)} textures, {len(images)} PNGs.")


if __name__ == "__main__":
    main()
