#!/usr/bin/env python3
"""Install developer-supplied NVIDIA DLSS and Streamline SDKs outside any git work tree (plan §19, #99, #209).

Official GodotNext binaries stay 100% MIT and DLSS/Streamline-free. Developers who wish to compile
modules/dlss with dlss_backend=ngx (and optionally streamline_sdk_path) must accept NVIDIA's RTX SDKs
License directly and store the SDKs outside any git repository.

    python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license
    python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license --streamline
    python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license --tag v310.9.16 --dest ~/.godotnext/dlss-sdk
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

PINNED_TAG = "v310.9.16"
DLSS_REPO_URL = "https://github.com/NVIDIA/DLSS.git"

STREAMLINE_PINNED_TAG = "v2.14.16"
STREAMLINE_RELEASE_URL_TEMPLATE = (
    "https://github.com/NVIDIA-RTX/Streamline/releases/download/{tag}/streamline-sdk-{tag}.zip"
)

# Pinned rel/ SHA-256 digests from docs/godotnext/dlss-license.md (#98).
PINNED_REL_SHA256: dict[str, dict[str, str]] = {
    "v310.9.16": {
        "lib/Windows_x86_64/rel/nvngx_dlss.dll": "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        "lib/Windows_x86_64/rel/nvngx_dlssd.dll": "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee",
        "lib/Windows_x86_64/rel/nvngx_dlssg.dll": "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7",
        "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16": "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6",
        "lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16": "a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96",
        "lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16": "574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7",
    },
}

# Pinned Streamline bin/x64/ rel SHA-256 digests from docs/godotnext/dlss-license.md (#208).
# sl_nvperf.* is explicitly excluded.
PINNED_STREAMLINE_SHA256: dict[str, dict[str, str]] = {
    "v2.14.16": {
        "bin/x64/sl.interposer.dll": "b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf",
        "bin/x64/sl.common.dll": "38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75",
        "bin/x64/sl.dlss_g.dll": "3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f",
        "bin/x64/sl.reflex.dll": "443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122",
        "bin/x64/sl.pcl.dll": "5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770",
        "bin/x64/nvngx_dlssg.dll": "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7",
    },
}


def is_inside_git_work_tree(path: Path) -> bool:
    current = path.expanduser().resolve(strict=False)
    for candidate in (current, *current.parents):
        if (candidate / ".git").exists():
            return True
    probe = current
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    if probe.exists():
        res = subprocess.run(
            ["git", "-C", str(probe), "rev-parse", "--is-inside-work-tree"],
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode == 0 and res.stdout.strip() == "true":
            return True
    return False


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_pins(checkout_dir: Path, expected: dict[str, str] | None, tag: str, label: str) -> bool:
    if not expected:
        print(f"warning: no pinned SHA-256 table for {label} tag '{tag}'; skipping pin verification.", file=sys.stderr)
        return True
    ok = True
    for rel_path, want_hash in expected.items():
        if "nvperf" in rel_path.lower():
            print(f"error: excluded library '{rel_path}' must never be verified or distributed.", file=sys.stderr)
            ok = False
            continue
        full = checkout_dir / rel_path
        if not full.is_file():
            print(f"error: missing expected {label} rel runtime '{rel_path}'", file=sys.stderr)
            ok = False
            continue
        got_hash = sha256_file(full)
        if got_hash != want_hash:
            print(
                f"error: SHA-256 mismatch for '{rel_path}': expected {want_hash}, got {got_hash}",
                file=sys.stderr,
            )
            ok = False
        else:
            print(f"ok: {rel_path} ({got_hash})")
    return ok


def install_streamline(dest_root: Path, tag: str) -> int:
    if is_inside_git_work_tree(dest_root):
        print(
            f"Refusing to install NVIDIA Streamline SDK inside a git work tree: '{dest_root}' (plan §19).",
            file=sys.stderr,
        )
        return 2

    install_dir = dest_root / tag
    if is_inside_git_work_tree(install_dir) and not install_dir.exists():
        print(
            f"Refusing to install NVIDIA Streamline SDK inside a git work tree: '{install_dir}' (plan §19).",
            file=sys.stderr,
        )
        return 2

    dest_root.mkdir(parents=True, exist_ok=True)
    if not install_dir.exists():
        url = STREAMLINE_RELEASE_URL_TEMPLATE.format(tag=tag)
        print(f"Downloading NVIDIA Streamline SDK {tag} from {url} ...")
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp_dir = Path(tmp_str)
            zip_path = tmp_dir / f"streamline-sdk-{tag}.zip"
            urllib.request.urlretrieve(url, zip_path)
            extract_dir = tmp_dir / "extracted"
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(extract_dir)
            # Flatten a single top-level wrapper folder if present.
            children = list(extract_dir.iterdir())
            src_root = children[0] if len(children) == 1 and children[0].is_dir() and not (extract_dir / "include").is_dir() else extract_dir
            shutil.move(str(src_root), str(install_dir))
    else:
        print(f"Using existing Streamline SDK at {install_dir}; verifying rel SHA-256 pins ...")

    if not verify_pins(install_dir, PINNED_STREAMLINE_SHA256.get(tag), tag, "Streamline"):
        return 1

    print(f"NVIDIA Streamline SDK {tag} ready at: {install_dir}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tag",
        default=PINNED_TAG,
        help=f"NVIDIA/DLSS git tag to clone (default: {PINNED_TAG}).",
    )
    parser.add_argument(
        "--dest",
        default=str(Path.home() / ".godotnext" / "dlss-sdk"),
        help="Destination parent directory outside any git work tree (default: ~/.godotnext/dlss-sdk).",
    )
    parser.add_argument(
        "--streamline",
        action="store_true",
        help=f"Install the NVIDIA Streamline SDK (pinned tag {STREAMLINE_PINNED_TAG}) instead of or in addition to DLSS.",
    )
    parser.add_argument(
        "--streamline-tag",
        default=STREAMLINE_PINNED_TAG,
        help=f"NVIDIA-RTX/Streamline release tag (default: {STREAMLINE_PINNED_TAG}).",
    )
    parser.add_argument(
        "--streamline-dest",
        default=str(Path.home() / ".godotnext" / "streamline-sdk"),
        help="Destination parent directory for Streamline SDK outside any git work tree (default: ~/.godotnext/streamline-sdk).",
    )
    parser.add_argument(
        "--accept-nvidia-license",
        action="store_true",
        help="Required confirmation that you have read and accept NVIDIA's RTX SDKs License (https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt).",
    )
    args = parser.parse_args(argv)

    if not args.accept_nvidia_license:
        print(
            "Refusing to download NVIDIA SDKs without --accept-nvidia-license.\n"
            "Review https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt and docs/godotnext/dlss-license.md first.",
            file=sys.stderr,
        )
        return 2

    if args.streamline:
        sl_dest = Path(args.streamline_dest).expanduser().resolve(strict=False)
        return install_streamline(sl_dest, args.streamline_tag)

    dest_root = Path(args.dest).expanduser().resolve(strict=False)
    if is_inside_git_work_tree(dest_root):
        print(
            f"Refusing to install NVIDIA DLSS SDK inside a git work tree: '{dest_root}' (plan §19).",
            file=sys.stderr,
        )
        return 2

    checkout_dir = dest_root / args.tag
    if is_inside_git_work_tree(checkout_dir) and not checkout_dir.exists():
        print(
            f"Refusing to install NVIDIA DLSS SDK inside a git work tree: '{checkout_dir}' (plan §19).",
            file=sys.stderr,
        )
        return 2

    dest_root.mkdir(parents=True, exist_ok=True)
    if not checkout_dir.exists():
        print(f"Cloning NVIDIA/DLSS tag {args.tag} into {checkout_dir} ...")
        res = subprocess.run(
            ["git", "clone", "--depth", "1", "--branch", args.tag, DLSS_REPO_URL, str(checkout_dir)],
            check=False,
        )
        if res.returncode != 0:
            return res.returncode
    else:
        print(f"Using existing checkout at {checkout_dir}; verifying rel SHA-256 pins ...")

    if not verify_pins(checkout_dir, PINNED_REL_SHA256.get(args.tag), args.tag, "DLSS"):
        return 1

    print(f"NVIDIA DLSS SDK {args.tag} ready at: {checkout_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
