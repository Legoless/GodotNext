#!/usr/bin/env python3
"""Convert niagara_bistro (MSFT_texture_dds + KHR_materials_pbrSpecularGlossiness)
into a clean, standard glTF 2.0 pbrMetallicRoughness + PNG scene for both
Unreal Engine 5.9 (Nanite + Lumen) and GodotNext (NextGeo + NextGI)."""
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

SRC_DIR = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/niagara_bistro")
DST_DIR = Path("/data2/Projects/legoless/GodotNext/local/godotnext-tools/ue59-real-scenes/bistro_pbr")
TEX_DIR = DST_DIR / "textures"


def srgb_to_linear(c: np.ndarray) -> np.ndarray:
    return np.where(c <= 0.04045, c / 12.92, np.power(np.clip((c + 0.055) / 1.055, 0.0, 1.0), 2.4))


def linear_to_srgb(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def load_rgba_u8(path: Path, max_dim: int = 2048) -> np.ndarray:
    im = Image.open(path).convert("RGBA")
    if max(im.size) > max_dim:
        scale = max_dim / float(max(im.size))
        nw = max(1, int(round(im.size[0] * scale)))
        nh = max(1, int(round(im.size[1] * scale)))
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    return np.asarray(im, dtype=np.uint8)


def convert_normal_or_emissive(task):
    src_path, dst_path, kind = task
    if dst_path.exists():
        return str(dst_path)
    arr = load_rgba_u8(src_path, max_dim=2048)
    rgb = arr[:, :, :3]
    if kind == "normal":
        # Lumberyard _ddna textures store tangent-space XYZ normal in RGB (OpenGL/DX checked; normalize Z).
        # Ensure B is valid positive tangent Z so normal map is well-formed.
        nx = (rgb[:, :, 0].astype(np.float32) / 255.0) * 2.0 - 1.0
        ny = (rgb[:, :, 1].astype(np.float32) / 255.0) * 2.0 - 1.0
        nz = np.sqrt(np.clip(1.0 - nx * nx - ny * ny, 0.0001, 1.0))
        out = np.empty_like(rgb)
        out[:, :, 0] = rgb[:, :, 0]
        out[:, :, 1] = rgb[:, :, 1]
        out[:, :, 2] = np.clip(np.round((nz * 0.5 + 0.5) * 255.0), 128, 255).astype(np.uint8)
        Image.fromarray(out, mode="RGB").save(dst_path, compress_level=3)
    else:
        Image.fromarray(rgb, mode="RGB").save(dst_path, compress_level=3)
    return str(dst_path)


def convert_pbr_pair(task):
    mat_idx, diff_src, spec_src, diff_factor, spec_factor, gloss_factor, alpha_mode, bc_dst, mr_dst = task
    if bc_dst.exists() and (mr_dst is None or mr_dst.exists()):
        return mat_idx, True

    diff_arr = load_rgba_u8(diff_src, max_dim=2048) if diff_src is not None else None
    spec_arr = load_rgba_u8(spec_src, max_dim=2048) if spec_src is not None else None

    if diff_arr is not None and spec_arr is not None and diff_arr.shape[:2] != spec_arr.shape[:2]:
        h, w = diff_arr.shape[:2]
        spec_im = Image.fromarray(spec_arr, mode="RGBA").resize((w, h), Image.Resampling.BILINEAR)
        spec_arr = np.asarray(spec_im, dtype=np.uint8)

    if diff_arr is None and spec_arr is None:
        return mat_idx, False

    if diff_arr is not None:
        h, w = diff_arr.shape[:2]
        diff_lin = srgb_to_linear(diff_arr[:, :, :3].astype(np.float32) / 255.0)
        diff_lin *= np.array(diff_factor[:3], dtype=np.float32)
        alpha_u8 = np.clip(np.round(diff_arr[:, :, 3].astype(np.float32) * float(diff_factor[3])), 0, 255).astype(np.uint8)
    else:
        h, w = spec_arr.shape[:2]
        diff_lin = np.broadcast_to(np.array(diff_factor[:3], dtype=np.float32), (h, w, 3)).copy()
        alpha_u8 = np.full((h, w), int(round(float(diff_factor[3]) * 255.0)), dtype=np.uint8)

    if spec_arr is not None:
        spec_lin = srgb_to_linear(spec_arr[:, :, :3].astype(np.float32) / 255.0)
        spec_lin *= np.array(spec_factor[:3], dtype=np.float32)
        gloss = (spec_arr[:, :, 3].astype(np.float32) / 255.0) * float(gloss_factor)
    else:
        spec_lin = np.broadcast_to(np.array(spec_factor[:3], dtype=np.float32), (h, w, 3))
        gloss = np.full((h, w), float(gloss_factor), dtype=np.float32)

    # Khronos KHR_materials_pbrSpecularGlossiness -> pbrMetallicRoughness conversion
    dielectric_spec = 0.04
    one_minus_spec = np.clip(1.0 - np.max(spec_lin, axis=2), 1e-6, 1.0)
    # Perceived brightness
    p_diff = np.sqrt(0.299 * diff_lin[:, :, 0] ** 2 + 0.587 * diff_lin[:, :, 1] ** 2 + 0.114 * diff_lin[:, :, 2] ** 2)
    p_spec = np.sqrt(0.299 * spec_lin[:, :, 0] ** 2 + 0.587 * spec_lin[:, :, 1] ** 2 + 0.114 * spec_lin[:, :, 2] ** 2)

    # Solve metallic
    a = dielectric_spec
    b = p_diff * one_minus_spec / (1.0 - dielectric_spec) + p_spec - 2.0 * dielectric_spec
    c = dielectric_spec - p_spec
    disc = np.maximum(b * b - 4.0 * a * c, 0.0)
    metallic = np.where(p_spec < dielectric_spec, 0.0, np.clip((-b + np.sqrt(disc)) / (2.0 * a), 0.0, 1.0))

    # BaseColor linear
    base_diel = diff_lin * (one_minus_spec / np.maximum(1.0 - dielectric_spec, 1e-6))[:, :, None]
    base_diel = base_diel / np.maximum(1.0 - metallic, 1e-4)[:, :, None]
    base_met = (spec_lin - dielectric_spec * (1.0 - metallic)[:, :, None]) / np.maximum(metallic, 1e-4)[:, :, None]
    base_lin = np.clip(base_diel * (1.0 - metallic)[:, :, None] + base_met * metallic[:, :, None], 0.0, 1.0)
    base_srgb = np.clip(np.round(linear_to_srgb(base_lin) * 255.0), 0, 255).astype(np.uint8)

    if alpha_mode in ("MASK", "BLEND"):
        bc_rgba = np.dstack([base_srgb, alpha_u8])
        Image.fromarray(bc_rgba, mode="RGBA").save(bc_dst, compress_level=3)
    else:
        Image.fromarray(base_srgb, mode="RGB").save(bc_dst, compress_level=3)

    if mr_dst is not None:
        # glTF metallicRoughnessTexture: G = roughness, B = metallic
        rough_u8 = np.clip(np.round(np.clip(1.0 - gloss, 0.08, 1.0) * 255.0), 0, 255).astype(np.uint8)
        metal_u8 = np.clip(np.round(metallic * 255.0), 0, 255).astype(np.uint8)
        mr_rgb = np.zeros((h, w, 3), dtype=np.uint8)
        mr_rgb[:, :, 0] = 255  # AO = 1
        mr_rgb[:, :, 1] = rough_u8
        mr_rgb[:, :, 2] = metal_u8
        Image.fromarray(mr_rgb, mode="RGB").save(mr_dst, compress_level=3)

    return mat_idx, True


def main():
    DST_DIR.mkdir(parents=True, exist_ok=True)
    TEX_DIR.mkdir(parents=True, exist_ok=True)

    for bin_name in ("bistro.bin", "bistro-anim.bin"):
        src_bin = SRC_DIR / bin_name
        dst_bin = DST_DIR / bin_name
        if src_bin.exists() and not dst_bin.exists():
            os.symlink(src_bin, dst_bin)

    with open(SRC_DIR / "bistro.gltf") as f:
        g = json.load(f)

    old_images = g.get("images", [])
    old_textures = g.get("textures", [])

    def resolve_tex_dds(tex_idx: int) -> Path | None:
        if tex_idx is None or tex_idx < 0 or tex_idx >= len(old_textures):
            return None
        t = old_textures[tex_idx]
        img_idx = None
        if "extensions" in t and "MSFT_texture_dds" in t["extensions"]:
            img_idx = t["extensions"]["MSFT_texture_dds"].get("source")
        if img_idx is None:
            img_idx = t.get("source")
        if img_idx is None:
            return None
        uri = old_images[img_idx]["uri"]
        p = SRC_DIR / uri
        if p.exists():
            return p
        p_dds = p.with_suffix(".dds")
        if p_dds.exists():
            return p_dds
        return None

    new_images = []
    new_textures = []
    uri_to_tex_idx = {}

    def register_png(rel_uri: str, sampler_idx=None) -> int:
        key = (rel_uri, sampler_idx)
        if key in uri_to_tex_idx:
            return uri_to_tex_idx[key]
        img_idx = len(new_images)
        new_images.append({"uri": rel_uri, "mimeType": "image/png"})
        tex_obj = {"source": img_idx}
        if sampler_idx is not None:
            tex_obj["sampler"] = sampler_idx
        tex_idx = len(new_textures)
        new_textures.append(tex_obj)
        uri_to_tex_idx[key] = tex_idx
        return tex_idx

    simple_tasks = {}
    pbr_tasks = []
    new_materials = []

    for midx, m in enumerate(g.get("materials", [])):
        nm = {
            "name": m.get("name", f"mat_{midx}"),
            "alphaMode": m.get("alphaMode", "OPAQUE"),
            "doubleSided": bool(m.get("doubleSided", False)),
        }
        if nm["alphaMode"] == "MASK":
            nm["alphaCutoff"] = float(m.get("alphaCutoff", 0.5))
            nm["doubleSided"] = True

        # Normal map
        if "normalTexture" in m:
            nt = m["normalTexture"]
            dds_p = resolve_tex_dds(nt["index"])
            if dds_p is not None:
                stem = dds_p.stem
                dst_p = TEX_DIR / f"nrm_{stem}.png"
                simple_tasks[str(dst_p)] = (dds_p, dst_p, "normal")
                sampler = old_textures[nt["index"]].get("sampler")
                n_ti = register_png(f"textures/nrm_{stem}.png", sampler)
                nm["normalTexture"] = {"index": n_ti}
                if "scale" in nt:
                    nm["normalTexture"]["scale"] = nt["scale"]

        # Emissive map (keep moderate so bulbs don't blow out GI)
        if "emissiveTexture" in m:
            et = m["emissiveTexture"]
            dds_p = resolve_tex_dds(et["index"])
            if dds_p is not None:
                stem = dds_p.stem
                dst_p = TEX_DIR / f"em_{stem}.png"
                simple_tasks[str(dst_p)] = (dds_p, dst_p, "emissive")
                sampler = old_textures[et["index"]].get("sampler")
                e_ti = register_png(f"textures/em_{stem}.png", sampler)
                nm["emissiveTexture"] = {"index": e_ti}
                nm["emissiveFactor"] = m.get("emissiveFactor", [1.0, 1.0, 1.0])
        elif "emissiveFactor" in m:
            ef = m["emissiveFactor"]
            if any(v > 0 for v in ef):
                nm["emissiveFactor"] = ef

        sg = m.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness")
        trans = m.get("extensions", {}).get("KHR_materials_transmission")
        mat_name_l = nm["name"].lower()
        if trans is not None:
            # Realistic architectural / bottle glass: glossy, dark-tinted translucent glass
            # so interiors/curtains are subtly visible and HWRT Hit Lighting reflects the street & sky cleanly.
            tf = float(trans.get("transmissionFactor", 0.9))
            alpha = max(0.22, min(0.55, 1.0 - tf * 0.70))
            base_in = (m.get("pbrMetallicRoughness") or {}).get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
            tint = [
                max(0.02, float(base_in[0]) * 0.08),
                max(0.03, float(base_in[1]) * 0.11),
                max(0.04, float(base_in[2]) * 0.13),
                alpha,
            ]
            rough = 0.09 if "dirty" in mat_name_l else 0.035
            nm["alphaMode"] = "BLEND"
            nm["doubleSided"] = True
            nm["pbrMetallicRoughness"] = {
                "baseColorFactor": tint,
                "metallicFactor": 0.0,
                "roughnessFactor": rough,
            }
        elif "frosted_glass" in mat_name_l or "frozen_glass" in mat_name_l:
            nm["alphaMode"] = "BLEND"
            nm["doubleSided"] = True
            nm["pbrMetallicRoughness"] = {
                "baseColorFactor": [0.55, 0.62, 0.68, 0.62],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.24,
            }
        elif sg is not None:
            df = sg.get("diffuseFactor", [1.0, 1.0, 1.0, 1.0])
            sf = sg.get("specularFactor", [0.04, 0.04, 0.04])
            gf = float(sg.get("glossinessFactor", 0.5))
            diff_dds = resolve_tex_dds(sg["diffuseTexture"]["index"]) if "diffuseTexture" in sg else None
            spec_dds = resolve_tex_dds(sg["specularGlossinessTexture"]["index"]) if "specularGlossinessTexture" in sg else None
            sampler = None
            if "diffuseTexture" in sg:
                sampler = old_textures[sg["diffuseTexture"]["index"]].get("sampler")
            if diff_dds is not None or spec_dds is not None:
                bc_name = f"bc_m{midx:03d}.png"
                mr_name = f"mr_m{midx:03d}.png"
                bc_dst = TEX_DIR / bc_name
                mr_dst = TEX_DIR / mr_name if spec_dds is not None else None
                pbr_tasks.append((midx, diff_dds, spec_dds, df, sf, gf, nm["alphaMode"], bc_dst, mr_dst))
                bc_ti = register_png(f"textures/{bc_name}", sampler)
                pbr = {
                    "baseColorTexture": {"index": bc_ti},
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                }
                if mr_dst is not None:
                    mr_ti = register_png(f"textures/{mr_name}", sampler)
                    pbr["metallicRoughnessTexture"] = {"index": mr_ti}
                    pbr["metallicFactor"] = 1.0
                    pbr["roughnessFactor"] = 1.0
                else:
                    pbr["metallicFactor"] = 0.0
                    pbr["roughnessFactor"] = max(0.08, min(1.0, 1.0 - gf))
                nm["pbrMetallicRoughness"] = pbr
            else:
                # Constant color material
                max_s = max(sf)
                metal = 0.0 if max_s < 0.08 else min(1.0, max_s)
                nm["pbrMetallicRoughness"] = {
                    "baseColorFactor": df,
                    "metallicFactor": metal,
                    "roughnessFactor": max(0.08, min(1.0, 1.0 - gf)),
                }
        elif "pbrMetallicRoughness" in m:
            nm["pbrMetallicRoughness"] = m["pbrMetallicRoughness"]
        else:
            nm["pbrMetallicRoughness"] = {
                "baseColorFactor": [0.7, 0.7, 0.7, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.8,
            }

        new_materials.append(nm)

    print(f"Converting {len(simple_tasks)} normal/emissive maps and {len(pbr_tasks)} PBR material texture pairs...", flush=True)
    with concurrent.futures.ProcessPoolExecutor(max_workers=min(32, os.cpu_count() or 8)) as ex:
        list(ex.map(convert_normal_or_emissive, simple_tasks.values()))
        list(ex.map(convert_pbr_pair, pbr_tasks))

    # Strip deprecated extensions, keep clean standard glTF 2.0
    g["images"] = new_images
    g["textures"] = new_textures
    g["materials"] = new_materials
    exts = [e for e in g.get("extensionsUsed", []) if e not in ("KHR_materials_pbrSpecularGlossiness", "MSFT_texture_dds", "KHR_materials_transmission")]
    if exts:
        g["extensionsUsed"] = exts
    else:
        g.pop("extensionsUsed", None)
    g.pop("extensionsRequired", None)

    out_gltf = DST_DIR / "bistro_pbr.gltf"
    with open(out_gltf, "w") as f:
        json.dump(g, f, separators=(",", ":"))
    print(f"Wrote {out_gltf}: {len(new_materials)} materials, {len(new_textures)} textures, {len(new_images)} PNG images.", flush=True)


if __name__ == "__main__":
    main()
