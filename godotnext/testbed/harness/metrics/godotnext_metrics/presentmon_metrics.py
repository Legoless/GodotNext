"""Displayed frame time, displayed rate, PC latency and the GPU-bound rule from PresentMon (#202).

GU+-5 and GU+-9 (plan §21.5) measure what reaches the display, which the harness's rendered-frame
timing cannot see. PresentMon is installed on the Windows test host and never vendored. The pin is
the PresentMon 2.6.0 console application (MIT, https://github.com/GameTechDev/PresentMon, tag
v2.6.0), with its default CSV columns plus ``--track_pc_latency``. These are provisional metrics
registered with #14, not Q ids (``docs/godotnext/metrics.md``, "Displayed frames and latency").

- A displayed frame is a CSV row whose ``MsBetweenDisplayChange`` is a number and, when the CSV
  has a ``DisplayedTime`` column, whose ``DisplayedTime`` is not ``NA``. The README lists that column,
  but a CSV recorded with the 2.6.0 console has none (``tests/fixtures/fg``). Without
  ``MsBetweenDisplayChange`` (a ``--v2_metrics`` CSV), ``DisplayedTime`` is the interval. The window skips the first ``warmup`` (120) displayed frames
  and takes the next ``frames`` (600). Dropped rows count for neither.
- Displayed frame time is the interval between display changes. The report gives its p50, p99 and
  p99 / p50 (GU+-5 FG 2x and GU+-9: ≤ 1.5). Percentiles are the harness's (``percentile.py``).
- Displayed rate is the window's frame count over the sum of its displayed frame times, in Hz.
- PC latency is ``MsPCLatency`` (the sl.pcl markers; Reflex Off and On runs both load Streamline):
  the p50 over the window frames that report it, with the mean and p99 beside it. A run without it
  is an input error unless ``--no-latency`` says the build has no PC-latency markers.
- GPU busy is the sum of ``MsGPUBusy`` over the sum of the application frame interval
  (``MsBetweenAppStart``; ``GPUBusy`` and ``FrameTime`` in a ``--v2_metrics`` CSV), over the rows
  from the first to the last window frame, dropped rows included, skipping rows that lack either
  value. A run counts as GPU-bound only at ≥ 0.95.
- One process and one swap chain per CSV: a CSV with more than one is an input error unless the
  swap chain is chosen for that file, by its ``SwapChainAddress`` or ``auto`` (the one with the
  most displayed frames). A ``SwapChainAddress`` is a pointer in one process, so ``compare`` takes
  ``--test-swap-chain`` and ``--base-swap-chain``, and ``spread`` one ``--swap-chain`` per CSV
  (or a single ``auto`` for all).

``compare`` takes a test run and its baseline (FG 2x vs FG off, or Reflex On vs Off). The GPU-bound
rule applies to the baseline, the configuration that makes the scene GPU-bound; the test run's
fraction is reported. ``spread`` is (max − min) / min over runs, as the harness's timing spread.
``record`` starts PresentMon on the testbed process around a launch command. The console
application documents no version option, so ``record`` pins the executable by content: it refuses
any file but the 2.6.0 release's ``PresentMon-2.6.0-x64.exe``, by the SHA-256 GitHub publishes for
that asset. A report from a CSV alone carries ``presentmon_pin``, the version the parser was written
for, not one it read. The measurements are PQ work (§22); at PR time the tools only have to run.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import shutil
import subprocess
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from godotnext_metrics.percentile import percentile

PRESENTMON_VERSION = "2.6.0"
# The v2.6.0 release asset and the SHA-256 GitHub publishes for it
# (https://github.com/GameTechDev/PresentMon/releases/tag/v2.6.0; docs/godotnext/source-log/metrics.md).
PRESENTMON_EXE = "PresentMon-2.6.0-x64.exe"
PRESENTMON_SHA256 = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"
WARMUP = 120
FRAMES = 600
GPU_BUSY_MIN = 0.95
P99_OVER_MEDIAN_MAX = 1.5
RATE_RATIO_MIN = 1.6
LATENCY_REDUCTION_MIN = 0.10
SPREAD_MAX_PERCENT = 3.0
SESSION_NAME = "GodotNextPresentMon"
AUTO_SWAP_CHAIN = "auto"

# Column names from the PresentMon console README at v2.6.0 (default metrics) and v2.3.0 (the
# --v2_metrics names). The first name present wins.
_DISPLAY_CHANGE = ("MsBetweenDisplayChange",)
_DISPLAYED_TIME = ("DisplayedTime",)
_GPU_BUSY = ("MsGPUBusy", "GPUBusy")
_FRAME_INTERVAL = ("MsBetweenAppStart", "FrameTime")
_PC_LATENCY = ("MsPCLatency",)
_GROUP = ("Application", "ProcessID", "SwapChainAddress")


class PresentMonInputError(ValueError):
    """The CSV cannot give the displayed-frame window."""


@dataclass(frozen=True)
class Frame:
    """One CSV row of the chosen swap chain."""

    displayed: bool
    display_ms: float | None
    gpu_busy_ms: float | None
    frame_interval_ms: float | None
    pc_latency_ms: float | None


def _number(cell: str | None) -> float | None:
    if cell is None:
        return None
    text = cell.strip()
    if not text or text.upper() == "NA":
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def _column(fields: Sequence[str], names: tuple[str, ...]) -> str | None:
    return next((name for name in names if name in fields), None)


def read_frames(path: Path, *, swap_chain: str | None = None) -> tuple[list[Frame], dict[str, object]]:
    """The rows of one swap chain in ``path``, in CSV order, and what was read.

    ``swap_chain`` is a ``SwapChainAddress``, ``auto`` for the process / swap chain pair with the
    most displayed frames, or None when the CSV must hold one pair only.
    """
    auto = swap_chain == AUTO_SWAP_CHAIN
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        fields = [name.strip() for name in reader.fieldnames or []]
        reader.fieldnames = fields
        display_change = _column(fields, _DISPLAY_CHANGE)
        displayed_time = _column(fields, _DISPLAYED_TIME)
        if display_change is None and displayed_time is None:
            raise PresentMonInputError(f"{path.name} has no MsBetweenDisplayChange or DisplayedTime column")
        gpu_busy = _column(fields, _GPU_BUSY)
        interval = _column(fields, _FRAME_INTERVAL)
        latency = _column(fields, _PC_LATENCY)
        group_fields = [name for name in _GROUP if name in fields]
        groups: dict[tuple[str, ...], list[Frame]] = {}
        in_file: set[tuple[str, ...]] = set()
        for row in reader:
            key = tuple(row.get(name, "").strip() for name in group_fields)
            in_file.add(key)
            if swap_chain is not None and not auto and row.get("SwapChainAddress", "").strip() != swap_chain:
                continue
            shown_for = _number(row.get(displayed_time)) if displayed_time else None
            na_time = displayed_time is not None and shown_for is None
            if display_change is not None:
                display_ms = _number(row.get(display_change))
            else:
                display_ms = shown_for
            groups.setdefault(key, []).append(
                Frame(
                    displayed=display_ms is not None and not na_time,
                    display_ms=display_ms,
                    gpu_busy_ms=_number(row.get(gpu_busy)) if gpu_busy else None,
                    frame_interval_ms=_number(row.get(interval)) if interval else None,
                    pc_latency_ms=_number(row.get(latency)) if latency else None,
                )
            )
    if not groups:
        raise PresentMonInputError(
            f"{path.name} has no rows" + (f" for swap chain {swap_chain}" if swap_chain is not None else "")
        )
    pairs = len(groups)
    if pairs > 1 and auto:
        shown = {key: sum(frame.displayed for frame in rows) for key, rows in groups.items()}
        ranked = sorted(shown, key=lambda key: shown[key], reverse=True)
        if shown[ranked[0]] == shown[ranked[1]]:
            raise PresentMonInputError(
                f"{path.name}: two process / swap chain pairs have {shown[ranked[0]]} displayed frames each; "
                "name the swap chain"
            )
        groups = {ranked[0]: groups[ranked[0]]}
    elif pairs > 1:
        listed = "; ".join(" ".join(key) for key in sorted(groups))
        raise PresentMonInputError(
            f"{path.name} has {pairs} process / swap chain pairs ({listed}); choose one by its SwapChainAddress or auto"
        )
    key, frames = next(iter(groups.items()))
    source = {
        "file": path.name,
        "process": dict(zip(group_fields, key, strict=True)),
        "columns": {
            "display": display_change or displayed_time,
            "gpu_busy": gpu_busy,
            "frame_interval": interval,
            "pc_latency": latency,
        },
        "rows": len(frames),
        "swap_chain_choice": swap_chain,
        "pairs_in_file": len(in_file),
    }
    return frames, source


def _stats(values: list[float]) -> dict[str, float]:
    return {"p50": percentile(values, 50.0), "p99": percentile(values, 99.0), "mean": float(np.mean(values))}


def summarize(
    frames: list[Frame], *, warmup: int = WARMUP, count: int = FRAMES, require_latency: bool = True
) -> dict[str, object]:
    """The displayed-frame metrics of one run over its window."""
    displayed = [index for index, frame in enumerate(frames) if frame.displayed]
    if len(displayed) < warmup + count:
        raise PresentMonInputError(
            f"need {warmup + count} displayed frames ({warmup} warm-up + {count}), found {len(displayed)}"
        )
    window = displayed[warmup : warmup + count]
    times = [float(frames[index].display_ms or 0.0) for index in window]
    shown = _stats(times)
    total_ms = float(sum(times))
    rate = count * 1000.0 / total_ms if total_ms > 0.0 else math.inf
    latencies = [value for index in window if (value := frames[index].pc_latency_ms) is not None]
    if require_latency and not latencies:
        raise PresentMonInputError("no window frame reports MsPCLatency (run with --track_pc_latency and sl.pcl)")
    span = frames[window[0] : window[-1] + 1]
    busy_pairs = [
        (frame.gpu_busy_ms, frame.frame_interval_ms)
        for frame in span
        if frame.gpu_busy_ms is not None and frame.frame_interval_ms is not None and frame.frame_interval_ms > 0.0
    ]
    if not busy_pairs:
        raise PresentMonInputError("no window row has both GPU busy and the application frame interval")
    busy_ms = float(sum(pair[0] for pair in busy_pairs))
    interval_ms = float(sum(pair[1] for pair in busy_pairs))
    gpu_busy = busy_ms / interval_ms
    latency_stats = _stats(latencies) if latencies else None
    latency: dict[str, object] | None = None
    if latency_stats is not None:
        latency = {**latency_stats, "frames": len(latencies)}
    return {
        "warmup": warmup,
        "frames": count,
        "displayed_frame_ms": shown,
        "p99_over_median": shown["p99"] / shown["p50"] if shown["p50"] > 0.0 else None,
        "displayed_rate_hz": rate if math.isfinite(rate) else None,
        "pc_latency_ms": latency_stats["p50"] if latency_stats else None,
        "pc_latency": latency,
        "gpu_busy_fraction": gpu_busy,
        "gpu_busy_rows": len(busy_pairs),
        # The 1e-9 keeps the bound itself GPU-bound despite rounding in the sums.
        "gpu_bound": bool(gpu_busy >= GPU_BUSY_MIN - 1e-9),
        "dropped_in_window": len(span) - count,
    }


def summary_report(
    path: Path,
    *,
    warmup: int = WARMUP,
    count: int = FRAMES,
    require_latency: bool = True,
    swap_chain: str | None = None,
) -> dict[str, object]:
    frames, source = read_frames(path, swap_chain=swap_chain)
    report: dict[str, object] = {"metric": "displayed frames (provisional)", "presentmon_pin": PRESENTMON_VERSION}
    report.update(summarize(frames, warmup=warmup, count=count, require_latency=require_latency))
    report["source"] = source
    # The run counts only when it is GPU-bound.
    report["pass"] = report["gpu_bound"]
    return report


def compare(test: dict[str, object], base: dict[str, object], kind: str) -> dict[str, object]:
    """GU+-5: ``fg`` (FG 2x over FG off) or ``reflex`` (Reflex On over Off)."""
    counts = bool(base["gpu_bound"])
    report: dict[str, object] = {
        "metric": f"GU+-5 {kind} (provisional)",
        "kind": kind,
        "base_gpu_bound": counts,
        "base_gpu_busy_fraction": base["gpu_busy_fraction"],
        "test_gpu_busy_fraction": test["gpu_busy_fraction"],
    }
    if kind == "fg":
        test_rate = test["displayed_rate_hz"]
        base_rate = base["displayed_rate_hz"]
        ratio = (
            float(test_rate) / float(base_rate)
            if isinstance(test_rate, float) and isinstance(base_rate, float) and base_rate > 0.0
            else None
        )
        p99_ratio = test["p99_over_median"]
        report.update({
            "displayed_rate_ratio": ratio,
            "displayed_rate_ratio_min": RATE_RATIO_MIN,
            "test_p99_over_median": p99_ratio,
            "p99_over_median_max": P99_OVER_MEDIAN_MAX,
        })
        passed = (
            counts
            and ratio is not None
            and ratio >= RATE_RATIO_MIN - 1e-9
            and isinstance(p99_ratio, float)
            and p99_ratio <= P99_OVER_MEDIAN_MAX + 1e-9
        )
    elif kind == "reflex":
        test_latency = test["pc_latency_ms"]
        base_latency = base["pc_latency_ms"]
        if not isinstance(test_latency, float) or not isinstance(base_latency, float) or base_latency <= 0.0:
            raise PresentMonInputError("Reflex comparison needs PC latency on both runs")
        reduction = 1.0 - test_latency / base_latency
        report.update({
            "test_pc_latency_ms": test_latency,
            "base_pc_latency_ms": base_latency,
            "latency_reduction": reduction,
            "latency_reduction_min": LATENCY_REDUCTION_MIN,
        })
        # The 1e-9 keeps the bound itself a pass despite rounding.
        passed = counts and reduction >= LATENCY_REDUCTION_MIN - 1e-9
    else:
        raise PresentMonInputError(f"unknown comparison {kind!r}")
    report["pass"] = bool(passed)
    return report


def _spread_percent(values: list[float]) -> float | None:
    low = min(values)
    if low <= 0.0:
        return None
    return (max(values) - low) / low * 100.0


def spread(summaries: list[dict[str, object]]) -> dict[str, object]:
    """#202 PQ: displayed p99 and PC latency spread over runs (FG off), each ≤ 3%."""
    if len(summaries) < 2:
        raise PresentMonInputError("spread needs at least two runs")
    p99s: list[float] = []
    latencies: list[float] = []
    for item in summaries:
        shown = item["displayed_frame_ms"]
        assert isinstance(shown, dict)
        p99s.append(float(shown["p99"]))
        latency = item["pc_latency_ms"]
        if not isinstance(latency, float):
            raise PresentMonInputError("spread needs PC latency on every run")
        latencies.append(latency)
    p99_spread = _spread_percent(p99s)
    latency_spread = _spread_percent(latencies)
    all_bound = all(bool(item["gpu_bound"]) for item in summaries)
    passed = (
        all_bound
        and p99_spread is not None
        and latency_spread is not None
        and p99_spread <= SPREAD_MAX_PERCENT + 1e-9
        and latency_spread <= SPREAD_MAX_PERCENT + 1e-9
    )
    return {
        "metric": "displayed p99 and PC latency spread (provisional)",
        "runs": len(summaries),
        "displayed_p99_ms": p99s,
        "pc_latency_ms": latencies,
        "displayed_p99_spread_percent": p99_spread,
        "pc_latency_spread_percent": latency_spread,
        "spread_max_percent": SPREAD_MAX_PERCENT,
        "all_gpu_bound": all_bound,
        "pass": bool(passed),
    }


def presentmon_command(
    executable: str, process_name: str, csv_path: Path, *, delay_s: int = 0, session: str = SESSION_NAME
) -> list[str]:
    """The pinned PresentMon command line for one testbed process."""
    command = [
        executable,
        "--process_name",
        process_name,
        "--output_file",
        str(csv_path),
        "--track_pc_latency",
        "--terminate_on_proc_exit",
        "--stop_existing_session",
        "--session_name",
        session,
        "--no_console_stats",
    ]
    if delay_s > 0:
        command += ["--delay", str(delay_s)]
    return command


def presentmon_identity(executable: str) -> dict[str, object]:
    """The pinned PresentMon's file name and SHA-256; any other file is an input error."""
    file = Path(shutil.which(executable) or executable)
    if not file.is_file():
        raise PresentMonInputError(f"PresentMon executable {executable!r} not found")
    digest = hashlib.sha256(file.read_bytes()).hexdigest()
    if digest != PRESENTMON_SHA256:
        raise PresentMonInputError(
            f"{file.name} is not PresentMon {PRESENTMON_VERSION} ({PRESENTMON_EXE}, SHA-256 {PRESENTMON_SHA256}); "
            f"its SHA-256 is {digest}"
        )
    return {"file": file.name, "sha256": digest, "version": PRESENTMON_VERSION}


def record(
    presentmon: list[str],
    launch: list[str],
    *,
    start_wait_s: float = 2.0,
    stop_timeout_s: float = 60.0,
) -> dict[str, object]:
    """Starts PresentMon, runs ``launch`` to the end, then waits for PresentMon to stop on its own."""
    tracer = subprocess.Popen(presentmon)
    try:
        time.sleep(start_wait_s)
        if tracer.poll() is not None:
            raise PresentMonInputError(f"PresentMon exited with {tracer.returncode} before the launch")
        launched = subprocess.run(launch, check=False)
        try:
            tracer_code = tracer.wait(timeout=stop_timeout_s)
        except subprocess.TimeoutExpired as error:
            raise PresentMonInputError(
                f"PresentMon did not stop within {stop_timeout_s:g} s of the launch ending"
            ) from error
    finally:
        if tracer.poll() is None:
            tracer.kill()
            tracer.wait()
    return {"launch_exit": launched.returncode, "presentmon_exit": tracer_code}


def _spread_swap_chains(chains: list[str] | None, files: int) -> list[str | None]:
    """``spread``'s swap chain per CSV: none, one per CSV in order, or a single ``auto`` for all."""
    if chains is None:
        return [None] * files
    if len(chains) == files:
        return list(chains)
    if chains == [AUTO_SWAP_CHAIN]:
        return [AUTO_SWAP_CHAIN] * files
    raise PresentMonInputError(f"spread takes one --swap-chain per CSV ({files}) or a single {AUTO_SWAP_CHAIN}")


def _emit(report: dict[str, object], out: Path | None) -> None:
    text = json.dumps(report, sort_keys=True, allow_nan=False)
    if out is not None:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text + "\n", encoding="utf-8")
    sys.stdout.write(text + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.presentmon_metrics")
    sub = parser.add_subparsers(dest="command", required=True)

    chain_help = (
        f"the SwapChainAddress to read when the CSV has several, or {AUTO_SWAP_CHAIN}: the most displayed frames"
    )

    def window_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--warmup", type=int, default=WARMUP)
        command.add_argument("--frames", type=int, default=FRAMES)
        command.add_argument(
            "--no-latency", action="store_true", help="the build has no PC-latency markers; report it as null"
        )

    summary = sub.add_parser("summary", help="one run")
    summary.add_argument("--csv", type=Path, required=True)
    summary.add_argument("--out", type=Path, default=None, help="also write the JSON here")
    summary.add_argument("--swap-chain", default=None, help=chain_help)
    window_options(summary)

    comparison = sub.add_parser("compare", help="GU+-5: a test run against its baseline")
    comparison.add_argument("--kind", choices=("fg", "reflex"), required=True)
    comparison.add_argument("--test", type=Path, required=True, help="FG 2x, or Reflex On")
    comparison.add_argument("--base", type=Path, required=True, help="FG off, or Reflex Off")
    comparison.add_argument("--test-swap-chain", default=None, help=chain_help + " (the test CSV)")
    comparison.add_argument("--base-swap-chain", default=None, help=chain_help + " (the baseline CSV)")
    window_options(comparison)

    spreads = sub.add_parser("spread", help="#202 PQ: runs of one configuration")
    spreads.add_argument("--csv", type=Path, nargs="+", required=True)
    spreads.add_argument(
        "--swap-chain",
        nargs="+",
        default=None,
        help=f"one per CSV, in order ({chain_help}); or {AUTO_SWAP_CHAIN} for all",
    )
    window_options(spreads)

    recorder = sub.add_parser("record", help="PresentMon around a launch command given after --")
    recorder.add_argument(
        "--presentmon", required=True, help=f"the PresentMon {PRESENTMON_VERSION} console executable, {PRESENTMON_EXE}"
    )
    recorder.add_argument("--process-name", required=True, help="the testbed process's executable name")
    recorder.add_argument("--out", type=Path, required=True, help="writes presentmon.csv and presentmon.json here")
    recorder.add_argument("--delay", type=int, default=0, help="seconds PresentMon waits before recording")
    recorder.add_argument("--start-wait", type=float, default=2.0)
    recorder.add_argument("--stop-timeout", type=float, default=60.0)
    recorder.add_argument("--swap-chain", default=None, help=chain_help)
    window_options(recorder)
    recorder.add_argument("launch", nargs=argparse.REMAINDER, help="-- <launch command>")

    args = parser.parse_args(argv)
    window = {"warmup": args.warmup, "count": args.frames, "require_latency": not args.no_latency}
    try:
        if args.command == "summary":
            _emit(summary_report(args.csv, swap_chain=args.swap_chain, **window), args.out)
        elif args.command == "compare":
            test = summary_report(args.test, swap_chain=args.test_swap_chain, **window)
            base = summary_report(args.base, swap_chain=args.base_swap_chain, **window)
            _emit(compare(test, base, args.kind), None)
        elif args.command == "spread":
            chains = _spread_swap_chains(args.swap_chain, len(args.csv))
            summaries = [
                summary_report(path, swap_chain=chain, **window) for path, chain in zip(args.csv, chains, strict=True)
            ]
            _emit(spread(summaries), None)
        else:
            launch = args.launch[1:] if args.launch[:1] == ["--"] else args.launch
            if not launch:
                raise PresentMonInputError("record needs a launch command after --")
            identity = presentmon_identity(args.presentmon)
            args.out.mkdir(parents=True, exist_ok=True)
            csv_path = args.out / "presentmon.csv"
            command = presentmon_command(args.presentmon, args.process_name, csv_path, delay_s=args.delay)
            exits = record(command, launch, start_wait_s=args.start_wait, stop_timeout_s=args.stop_timeout)
            report = summary_report(csv_path, swap_chain=args.swap_chain, **window)
            report["record"] = dict(exits, presentmon_command=command, presentmon_executable=identity)
            _emit(report, args.out / "presentmon.json")
    except (OSError, ValueError) as error:
        json.dump({"metric": "displayed frames (provisional)", "pass": False, "error": str(error)}, sys.stdout)
        sys.stdout.write("\n")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
