"""AS calibration derivation (#9, G0-6; plan §2.1 lines 1a and 1b).

Reads the ``as_calibration.json`` results of ``tests/as_calibration`` (one per card)
and derives, per card:

- TLAS build p95 GPU ms at every instance count, PREFER_FAST_TRACE and
  PREFER_FAST_BUILD (median of the per-run p95 values);
- BLAS build throughput in triangles per GPU ms, per flag and size bucket;
- refit throughput through RD's UPDATE mode, and through the standalone Vulkan
  tool when its result is given;
- line 1a = TLAS p95 at 50k instances + 200k triangles / refit throughput (also
  at the largest instance count), against the report-only target of 0.14 ms;
- line 1b's triangle cap = FAST_TRACE BLAS throughput x 0.10 ms, rounded down
  to 10k triangles;
- AS memory per configuration, and the p95 spread across runs per point.

At PR time only the field check gates (``missing_fields``); the numbers are PQ's.

Usage::

    python3 -m godotnext_metrics.as_calibration <as_calibration.json>... \\
        [--refit-tool <tool.json>] [--out <dir>]
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any

LINE_1A_TARGET_MS = 0.14
LINE_1B_TARGET_MS = 0.10
LINE_1A_INSTANCES = 50000
LINE_1A_SKINNED_TRIANGLES = 200000
CAP_ROUNDING = 10000
TOOL_TOLERANCE = 0.15
FLAGS = ("fast_trace", "fast_build")


def missing_fields(result: dict[str, Any]) -> list[str]:
    """Fields the derivation needs that a benchmark result lacks."""
    missing: list[str] = []
    for key in ("adapter", "driver", "runs", "tlas_sizes", "refit_sizes"):
        if key not in result:
            missing.append(key)
    runs = result.get("runs")
    if not isinstance(runs, list) or not runs:
        return missing + (["runs[0]"] if "runs" in result else [])
    for index, run in enumerate(runs):
        prefix = f"runs[{index}]"
        if not isinstance(run, dict):
            missing.append(prefix)
            continue
        blas = run.get("blas", {})
        for flag in FLAGS:
            entry = blas.get(flag) if isinstance(blas, dict) else None
            if not isinstance(entry, dict):
                missing.append(f"{prefix}.blas.{flag}")
                continue
            for key in ("triangles_per_ms", "buckets", "memory_bytes"):
                if key not in entry:
                    missing.append(f"{prefix}.blas.{flag}.{key}")
        tlas = run.get("tlas", {})
        for count in result.get("tlas_sizes", []) or []:
            point = tlas.get(str(count)) if isinstance(tlas, dict) else None
            for flag in FLAGS:
                entry = point.get(flag) if isinstance(point, dict) else None
                if not isinstance(entry, dict):
                    missing.append(f"{prefix}.tlas.{count}.{flag}")
                    continue
                for key in ("p50_ms", "p95_ms", "cpu_p95_ms", "memory_bytes"):
                    if key not in entry:
                        missing.append(f"{prefix}.tlas.{count}.{flag}.{key}")
        refit = run.get("refit_rd", {})
        for count in result.get("refit_sizes", []) or []:
            entry = refit.get(str(count)) if isinstance(refit, dict) else None
            if not isinstance(entry, dict) or "triangles_per_ms" not in entry:
                missing.append(f"{prefix}.refit_rd.{count}")
    return missing


def _median(values: list[float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def _spread_percent(values: list[float]) -> float:
    """(max - min) / median, in percent; 0 for fewer than two runs."""
    if len(values) < 2:
        return 0.0
    middle = _median(values)
    return 0.0 if middle == 0.0 else (max(values) - min(values)) / middle * 100.0


def triangle_cap(fast_trace_triangles_per_ms: float) -> int:
    """Line 1b's per-frame triangle cap: throughput x 0.10 ms, rounded down to 10k."""
    return int(math.floor(fast_trace_triangles_per_ms * LINE_1B_TARGET_MS / CAP_ROUNDING)) * CAP_ROUNDING


def line_1a_ms(tlas_p95_ms: float, refit_triangles_per_ms: float) -> float | None:
    """TLAS build plus the 200k-triangle skinned refit budget; None without a refit figure."""
    if refit_triangles_per_ms <= 0.0:
        return None
    return tlas_p95_ms + LINE_1A_SKINNED_TRIANGLES / refit_triangles_per_ms


def derive_card(result: dict[str, Any], refit_tool: dict[str, Any] | None = None) -> dict[str, Any]:
    """The derivation for one card's benchmark result."""
    runs = [run for run in result["runs"] if isinstance(run, dict)]
    spreads: dict[str, float] = {}

    tlas: dict[str, Any] = {}
    for count in result["tlas_sizes"]:
        per_flag: dict[str, Any] = {}
        for flag in FLAGS:
            p95 = [float(run["tlas"][str(count)][flag]["p95_ms"]) for run in runs]
            p50 = [float(run["tlas"][str(count)][flag]["p50_ms"]) for run in runs]
            cpu = [float(run["tlas"][str(count)][flag]["cpu_p95_ms"]) for run in runs]
            memory = [int(run["tlas"][str(count)][flag]["memory_bytes"]) for run in runs]
            per_flag[flag] = {
                "p50_ms": _median(p50),
                "p95_ms": _median(p95),
                "cpu_p95_ms": _median(cpu),
                "memory_bytes": max(memory) if memory else 0,
            }
            spreads[f"tlas.{count}.{flag}.p95"] = _spread_percent(p95)
        tlas[str(count)] = per_flag

    blas: dict[str, Any] = {}
    for flag in FLAGS:
        throughput = [float(run["blas"][flag]["triangles_per_ms"]) for run in runs]
        buckets: dict[str, float] = {}
        for label in runs[0]["blas"][flag]["buckets"]:
            buckets[label] = _median([float(run["blas"][flag]["buckets"][label]["triangles_per_ms"]) for run in runs])
        blas[flag] = {
            "triangles_per_ms": _median(throughput),
            "buckets": buckets,
            "memory_bytes": max(int(run["blas"][flag]["memory_bytes"]) for run in runs),
        }
        spreads[f"blas.{flag}.p95_total"] = _spread_percent([
            float(run["blas"][flag].get("p95_ms_total", 0.0)) for run in runs
        ])

    refit_rd: dict[str, float] = {}
    for count in result["refit_sizes"]:
        refit_rd[str(count)] = _median([float(run["refit_rd"][str(count)]["triangles_per_ms"]) for run in runs])
        spreads[f"refit_rd.{count}.p95"] = _spread_percent([
            float(run["refit_rd"][str(count)].get("p95_ms", 0.0)) for run in runs
        ])
    # Line 1a uses the refit throughput at the skinned budget's size (200k), else the largest measured size.
    refit_sizes = sorted(int(count) for count in refit_rd)
    refit_key = (
        str(LINE_1A_SKINNED_TRIANGLES)
        if str(LINE_1A_SKINNED_TRIANGLES) in refit_rd
        else (str(refit_sizes[-1]) if refit_sizes else "")
    )
    refit_rd_tpm = refit_rd.get(refit_key, 0.0)
    refit_tool_tpm = (
        float(refit_tool["refit_triangles_per_ms"]) if refit_tool and "refit_triangles_per_ms" in refit_tool else None
    )
    refit_tpm = refit_tool_tpm if refit_tool_tpm is not None else refit_rd_tpm

    tlas_counts = sorted(int(count) for count in tlas)
    line_1a: dict[str, Any] = {}
    for count in {
        c for c in (LINE_1A_INSTANCES, tlas_counts[-1] if tlas_counts else LINE_1A_INSTANCES) if str(c) in tlas
    }:
        value = line_1a_ms(float(tlas[str(count)]["fast_trace"]["p95_ms"]), refit_tpm)
        line_1a[str(count)] = {
            "ms": value,
            "target_ms": LINE_1A_TARGET_MS,
            "within_target": None if value is None else value <= LINE_1A_TARGET_MS,
        }

    derived: dict[str, Any] = {
        "adapter": result.get("adapter"),
        "driver": result.get("driver"),
        "runs": len(runs),
        "tlas": tlas,
        "blas": blas,
        "refit_rd_triangles_per_ms": refit_rd,
        "refit_source": "tool" if refit_tool_tpm is not None else "rd",
        "line_1a": line_1a,
        "line_1b_triangle_cap": triangle_cap(float(blas["fast_trace"]["triangles_per_ms"])),
        "spread_percent": spreads,
        "max_spread_percent": max(spreads.values()) if spreads else 0.0,
    }
    if refit_tool is not None:
        derived["refit_tool"] = refit_tool
        derived["tool_cross_check"] = _tool_cross_check(derived, refit_tool)
    return derived


def _tool_cross_check(derived: dict[str, Any], tool: dict[str, Any]) -> dict[str, Any]:
    """The tool's TLAS and FAST_TRACE BLAS figures against RD's (15% rule)."""
    checks: dict[str, Any] = {}
    tlas = derived["tlas"]
    pairs: list[tuple[str, float, float]] = []
    if "tlas_100k_p95_ms" in tool and isinstance(tlas, dict) and "100000" in tlas:
        pairs.append((
            "tlas_100k_p95_ms",
            float(tool["tlas_100k_p95_ms"]),
            float(tlas["100000"]["fast_trace"]["p95_ms"]),
        ))
    if "blas_fast_trace_triangles_per_ms" in tool:
        blas = derived["blas"]
        pairs.append((
            "blas_fast_trace_triangles_per_ms",
            float(tool["blas_fast_trace_triangles_per_ms"]),
            float(blas["fast_trace"]["triangles_per_ms"]),
        ))
    for name, tool_value, rd_value in pairs:
        difference = None if not rd_value else abs(tool_value - rd_value) / rd_value
        checks[name] = {
            "tool": tool_value,
            "rd": rd_value,
            "difference": difference,
            "within_15_percent": difference is not None and difference <= TOOL_TOLERANCE,
        }
    return checks


def markdown_table(cards: list[dict[str, Any]]) -> str:
    """The Appendix A table: one column per card."""
    names = [str(card.get("adapter")) for card in cards]
    rows: list[tuple[str, list[str]]] = []

    def cell(value: object, digits: int = 3) -> str:
        if value is None:
            return "n/a"
        if isinstance(value, float):
            return f"{value:.{digits}f}"
        return str(value)

    counts = sorted({int(count) for card in cards for count in card["tlas"]})
    for count in counts:
        rows.append((
            f"TLAS build p95, {count} instances, FAST_TRACE (ms)",
            [cell(card["tlas"].get(str(count), {}).get("fast_trace", {}).get("p95_ms")) for card in cards],
        ))
    for flag in FLAGS:
        rows.append((
            f"BLAS build, {flag.upper()} (triangles/ms)",
            [cell(card["blas"][flag]["triangles_per_ms"], 0) for card in cards],
        ))
    rows.append((
        "Refit (triangles/ms)",
        [cell(max(card["refit_rd_triangles_per_ms"].values(), default=None), 0) for card in cards],
    ))
    for key in (str(LINE_1A_INSTANCES), str(counts[-1]) if counts else ""):
        if key:
            rows.append((
                f"Line 1a at {key} instances (ms; target {LINE_1A_TARGET_MS})",
                [cell(card["line_1a"].get(key, {}).get("ms")) for card in cards],
            ))
    rows.append(("Line 1b triangle cap (triangles/frame)", [cell(card["line_1b_triangle_cap"]) for card in cards]))
    rows.append(("Largest p95 spread across runs (%)", [cell(card["max_spread_percent"], 2) for card in cards]))
    lines = ["| Measure | " + " | ".join(names) + " |", "| --- | " + " | ".join("---" for _ in names) + " |"]
    lines.extend(f"| {label} | " + " | ".join(values) + " |" for label, values in rows)
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", nargs="+", type=Path, help="as_calibration.json, one per card")
    parser.add_argument("--refit-tool", type=Path, help="the standalone refit tool's JSON")
    parser.add_argument("--out", type=Path, help="write as_calibration.json and as_calibration.md here")
    args = parser.parse_args(argv)
    tool = json.loads(args.refit_tool.read_text(encoding="utf-8")) if args.refit_tool else None
    cards: list[dict[str, Any]] = []
    missing: dict[str, list[str]] = {}
    for path in args.results:
        result = json.loads(path.read_text(encoding="utf-8"))
        gaps = missing_fields(result)
        if gaps:
            missing[str(path)] = gaps
            continue
        cards.append(derive_card(result, tool))
    summary = {"cards": cards, "missing_fields": missing, "pass": not missing and bool(cards)}
    if args.out is not None:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "as_calibration.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
        if cards:
            (args.out / "as_calibration.md").write_text(markdown_table(cards), encoding="utf-8")
    print(json.dumps(summary))
    return 0 if summary["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
