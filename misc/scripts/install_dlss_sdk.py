#!/usr/bin/env python3
"""Clone the developer-supplied NVIDIA DLSS SDK outside any git work tree (plan §19, #99).

Official GodotNext binaries stay 100% MIT and DLSS-free. Developers who wish to compile
modules/dlss with dlss_backend=ngx must accept NVIDIA's RTX SDKs License directly and
store the SDK outside any git repository.

    python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license
    python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license --tag v310.9.16 --dest ~/.godotnext/dlss-sdk
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

PINNED_TAG = "v310.9.16"
DLSS_REPO_URL = "https://github.com/NVIDIA/DLSS.git"

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


def is_inside_git_work_tree(path: Path) -> bool:
    current = path.expanduser().resolve(strict=False)
    for candidate in (current, *current.parents):
        if (candidate / ".git").exists():
            return True
    # Also ask git if an existing ancestor directory is inside a work tree.
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


def verify_pins(checkout_dir: Path, tag: str) -> bool:
    expected = PINNED_REL_SHA256.get(tag)
    if not expected:
        print(f"warning: no pinned SHA-256 table for tag '{tag}'; skipping pin verification.", file=sys.stderr)
        return True
    ok = True
    for rel_path, want_hash in expected.items():
        full = checkout_dir / rel_path
        if not full.is_file():
            print(f"error: missing expected rel runtime '{rel_path}'", file=sys.stderr)
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
        "--accept-nvidia-license",
        action="store_true",
        help="Required confirmation that you have read and accept NVIDIA's RTX SDKs License (https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt).",
    )
    args = parser.parse_args(argv)

    if not args.accept_nvidia_license:
        print(
            "Refusing to download NVIDIA DLSS SDK without --accept-nvidia-license.\n"
            "Review https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt and docs/godotnext/dlss-license.md first.",
            file=sys.stderr,
        )
        return 2

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

    if not verify_pins(checkout_dir, args.tag):
        return 1

    print(f"NVIDIA DLSS SDK {args.tag} ready at: {checkout_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
