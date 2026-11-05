#!/usr/bin/env python3
"""Generate the analytic IES profiles used by the PV content scenes (#213).

Every profile is computed from a closed-form distribution, so no
manufacturer photometric file is committed. Run from the repository root:

    python3 godotnext/testbed/tools/ies_gen.py --out godotnext/testbed/assets/ies
    python3 godotnext/testbed/tools/ies_gen.py --check

`--check` parses the committed files back and asserts the candela tables
match the analytic functions, so a hand edit is caught. The format is
LM-63-2002 type C, one horizontal plane and a 0..180 degree vertical sweep.
"""

from __future__ import annotations

import argparse
import math
import sys
from collections.abc import Callable
from pathlib import Path
from typing import TypedDict

VERTICAL_ANGLES = [float(angle) for angle in range(0, 181, 5)]
LUMENS = 1000.0
WATTS = 10.0


def cosine_lobe(angle_deg: float) -> float:
    """Lambertian: I(theta) = I0 * cos(theta), zeroed past 90 degrees."""
    angle = math.radians(angle_deg)
    return max(math.cos(angle), 0.0) if angle_deg <= 90.0 else 0.0


def batwing(angle_deg: float) -> float:
    """A rotationally symmetric batwing peaking near 35 degrees."""
    math.radians(angle_deg)
    lobe = math.exp(-((angle_deg - 35.0) ** 2) / (2.0 * 18.0**2))
    return max(lobe, 0.0) if angle_deg <= 95.0 else 0.0


def narrow_spot(angle_deg: float) -> float:
    """A tight cosine^12 spot with a hard cut-off at 30 degrees."""
    if angle_deg > 30.0:
        return 0.0
    return math.cos(math.radians(angle_deg)) ** 12


def street_asymmetric(angle_deg: float) -> float:
    """A street-light lobe: a wide forward throw and a short back throw."""
    math.radians(angle_deg)
    forward = math.exp(-((angle_deg - 20.0) ** 2) / (2.0 * 14.0**2))
    back = 0.25 * math.exp(-((angle_deg - 55.0) ** 2) / (2.0 * 20.0**2))
    return max(forward + back, 0.0) if angle_deg <= 90.0 else 0.0


Profile = Callable[[float], float]


class ParsedProfile(TypedDict):
    lumens: float
    ballast: float
    watts: float
    vertical: list[float]
    horizontal: list[float]
    values: list[float]


PROFILES: dict[str, Profile] = {
    "cosine_lobe": cosine_lobe,
    "batwing": batwing,
    "narrow_spot": narrow_spot,
    "street_asymmetric": street_asymmetric,
}


def _values(profile: Profile) -> list[float]:
    raw = [profile(angle) for angle in VERTICAL_ANGLES]
    peak = max(raw)
    if peak <= 0.0:
        raise ValueError("profile has no energy")
    return [round(value / peak * 1000.0, 3) for value in raw]


def render(name: str, profile: Profile) -> str:
    values = _values(profile)
    lines = [
        "IESNA:LM-63-2002",
        f"[TEST] GodotNext analytic {name} (#213)",
        "[MANUFAC] GodotNext",
        f"[LUMCAT] {name}",
        "[LUMINAIRE] Procedural, self-authored",
        "TILT=NONE",
        f"1 {LUMENS:g} 1 {len(VERTICAL_ANGLES)} 1 1 2 0 0 0",
        f"1.0 0.0 {WATTS:g}",
    ]
    lines.append(" ".join(f"{angle:.1f}" for angle in VERTICAL_ANGLES))
    lines.append("0.0")
    lines.append(" ".join(f"{value:.3f}" for value in values))
    return "\n".join(lines) + "\n"


def parse(text: str) -> ParsedProfile:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if lines[0] != "IESNA:LM-63-2002":
        raise ValueError("not an IESNA:LM-63-2002 file")
    index = 0
    while not lines[index].startswith("TILT="):
        index += 1
    if lines[index] != "TILT=NONE":
        raise ValueError("only TILT=NONE is generated")
    header = [float(value) for value in lines[index + 1].split()]
    vertical_counts = int(header[3])
    horizontal_counts = int(header[4])
    ballast = [float(value) for value in lines[index + 2].split()]
    vertical = [float(value) for value in lines[index + 3].split()]
    horizontal = [float(value) for value in lines[index + 4].split()]
    values = [float(value) for value in lines[index + 5].split()]
    if len(vertical) != vertical_counts or len(horizontal) != horizontal_counts:
        raise ValueError("angle counts do not match the header")
    if len(values) != vertical_counts * horizontal_counts:
        raise ValueError("candela count does not match the angles")
    return {
        "lumens": header[1],
        "ballast": ballast[0],
        "watts": ballast[2],
        "vertical": vertical,
        "horizontal": horizontal,
        "values": values,
    }


def check(directory: Path) -> list[str]:
    failures = []
    for name, profile in PROFILES.items():
        path = directory / f"{name}.ies"
        if not path.exists():
            failures.append(f"{path} is missing")
            continue
        parsed = parse(path.read_text())
        expected = _values(profile)
        if parsed["vertical"] != VERTICAL_ANGLES:
            failures.append(f"{path}: vertical angles differ")
        if parsed["values"] != expected:
            failures.append(f"{path}: candela table differs from the analytic profile")
        if abs(parsed["lumens"] - LUMENS) > 1e-6:
            failures.append(f"{path}: lumens {parsed['lumens']} != {LUMENS}")
    return failures


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("godotnext/testbed/assets/ies"))
    parser.add_argument("--check", action="store_true", help="verify the committed files")
    args = parser.parse_args(argv)

    if args.check:
        failures = check(args.out)
        for failure in failures:
            print(f"ies_gen: {failure}", file=sys.stderr)
        if failures:
            return 1
        print(f"ies_gen: {len(PROFILES)} profiles verified in {args.out}")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    for name, profile in PROFILES.items():
        path = args.out / f"{name}.ies"
        path.write_text(render(name, profile), encoding="utf-8")
        print(f"ies_gen: wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
