#!/usr/bin/env python3
"""Scan compiled binaries for forbidden NVIDIA NGX / DLSS / Streamline strings (plan §19, §21.5, U6, #97, #208).

Default official binaries and DLSS mock-backend binaries must contain zero ASCII
or UTF-16LE occurrences of proprietary NGX / DLSS / Streamline runtime identifiers.

    python3 misc/scripts/check_vendor_bytes.py <binary>...

Exit code 0 means all binaries are clean; exit code 1 means at least one match or error.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

FORBIDDEN_TOKENS: tuple[str, ...] = (
    "nvngx_dlssg",
    "nvngx_dlssd",
    "sl.interposer",
    "sl.dlss_g",
    "NVSDK_NGX",
    "nvngx_dlss",
)

CHUNK_SIZE = 8 * 1024 * 1024
OVERLAP_BYTES = 64


def _build_needles() -> list[tuple[str, str, bytes]]:
    needles: list[tuple[str, str, bytes]] = []
    for token in FORBIDDEN_TOKENS:
        lowered = token.lower()
        needles.append((token, "ascii", lowered.encode("ascii")))
        needles.append((token, "utf-16-le", lowered.encode("utf-16-le")))
    return needles


def scan_binary(path: Path) -> list[tuple[str, str, int]]:
    """Return list of (token, encoding, first_byte_offset) hits in `path`."""
    needles = _build_needles()
    remaining: list[tuple[str, str, bytes]] = list(needles)
    hits: list[tuple[str, str, int]] = []

    offset = 0
    tail = b""
    with path.open("rb") as f:
        while remaining:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            window = (tail + chunk).lower()
            window_base = offset - len(tail)

            still_remaining: list[tuple[str, str, bytes]] = []
            for token, encoding, needle in remaining:
                pos = window.find(needle)
                if pos != -1:
                    hits.append((token, encoding, window_base + pos))
                else:
                    still_remaining.append((token, encoding, needle))
            remaining = still_remaining

            offset += len(chunk)
            tail = chunk[-OVERLAP_BYTES:] if len(chunk) >= OVERLAP_BYTES else (tail + chunk)[-OVERLAP_BYTES:]

    return hits


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binaries",
        nargs="+",
        type=Path,
        help="Binary file(s) to scan for forbidden vendor substrings.",
    )
    args = parser.parse_args(argv)

    had_error = False
    total_hits = 0
    for binary in args.binaries:
        if not binary.is_file():
            print(f"check_vendor_bytes: file not found: {binary}", file=sys.stderr)
            had_error = True
            continue

        hits = scan_binary(binary)
        for token, encoding, byte_offset in hits:
            print(
                f"check_vendor_bytes: forbidden vendor string '{token}' ({encoding}) found in {binary} at offset 0x{byte_offset:x}",
                file=sys.stderr,
            )
        total_hits += len(hits)

    if had_error or total_hits > 0:
        print(
            f"check_vendor_bytes: failed ({total_hits} forbidden match(es) across {len(args.binaries)} binary(s)).",
            file=sys.stderr,
        )
        return 1

    print(f"check_vendor_bytes: {len(args.binaries)} binary(s) checked, 0 forbidden vendor byte matches.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
