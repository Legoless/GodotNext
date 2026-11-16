"""Report for one interleaved GodotNext/UE set (#229, plan §23.3 and §23.4).

    python3 -m godotnext_metrics.ue_report --compare <dir> [--out <dir>] [--judge]

``<dir>`` is what ``harness/runners/ue_compare.ps1`` wrote: ``plan.json``
(``python3 -m godotnext_metrics.ue_compare plan``), ``results.json`` (each
job's exit code) and ``runs/<job>/``. A GodotNext run holds the harness's
``timing.csv`` and ``manifest.json``. A UE run holds the runner's
``runner_job.json``, ``ue_profile.csv`` (the one CSV profile the editor wrote
for that launch, found by its ``godotnextrun`` metadata token) and ``ue.log``.

Every UE fact the report checks comes from what the run left, not from the
command line: the GPU, the RHI, the feature level, the resolution, VSync and
the build from the capture's metadata; the console variables from their echo
in the log; the run mode from the executable the runner launched and the
capture's command line; the clean exit from the runner's record. A GodotNext
run that the config expects to run NextGI must record the forced NextGI with
the configured tier and quality in its manifest, and a line on a NextGI
column group must find those columns with values in ``timing.csv``.

UE's internal resolution is not observed: no column or metadata key of a
game-mode capture records the size UE renders at before it upscales, and the
log echoes ``r.ScreenPercentage`` once, at start-up (#232). A set whose
GodotNext runs record a ``scaling_3d_scale`` other than 1.0 is therefore noted
in a dry run and refused when judged, until an observed source exists.

Without ``--judge`` the report is a build-time dry run: numbers and ratios,
no verdict, not a measurement (§22). Fewer than 5 runs per engine are allowed
then. ``--judge`` is for PQ: it needs 5 runs per engine, every column named,
the lead's sign-off reference, timing jobs with the warm-up launches, a pinned
UE build and a GodotNext release export (§23.4).

The UE captures stay in the private UE comparison storage on the RTX 4080
SUPER machine, so the report runs there; ``--out`` defaults to nothing (JSON on
stdout). Gate reports under ``docs/godotnext/gates/`` are written in PQ, from
judged reports, as data.

Exit 0 when a report was produced, 2 when the set is refused or an input is
missing. Both print JSON.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import cast

from godotnext_metrics.jsonutil import as_dict, as_int, as_list
from godotnext_metrics.result import json_ready
from godotnext_metrics.timing_metrics import read_timing_csv
from godotnext_metrics.ue_bar import (
    UE_BAR,
    ComparisonError,
    LineSpec,
    RunRecord,
    check_set,
    evaluate_line,
    run_statistic,
)
from godotnext_metrics.ue_compare import UE_EDITOR_EXE, ComparisonConfig, parse_config
from godotnext_metrics.ue_csv import (
    CsvProfile,
    CsvProfileError,
    group_ms,
    read_csv_profile,
    read_log_cvars,
    window_activity,
)

UE_PROFILE = "ue_profile.csv"
UE_LOG = "ue.log"
SCHEMA = 1
RUN_MODE_NOTE = (
    "UE ran the comparison project in the installed editor in game mode (plan v3.9.3). The editor's"
    " overhead and uncooked content favor GodotNext, most on the render-thread lines (GM-5, GN-13)."
)
# The capture metadata keys the report reads (lower-case, as the profiler writes them).
_META_TOKEN = "godotnextrun"
# No observed UE source records the internal resolution yet: a names-only inventory of every column
# and metadata key of an Upscaler Torture game-mode capture found none (#232).
INTERNAL_SIZE_UNVERIFIED = (
    "the GodotNext runs render at scaling_3d_scale {scale:g} of the output, and no UE capture column or"
    " metadata records the internal resolution UE rendered at (the log echoes r.ScreenPercentage once, at"
    " start-up), so the report cannot show that both engines upscaled from the same internal size"
)


def build_report(compare_dir: Path, *, judge: bool) -> dict[str, object]:
    plan = _json(compare_dir / "plan.json")
    config = parse_config(plan.get("config"))
    if judge and not config.sign_off:
        raise ComparisonError("--judge needs the lead's sign-off reference in the comparison config (§23.4)")
    results = _results(_json_any(compare_dir / "results.json"))
    jobs = [as_dict(item) for item in as_list(plan.get("jobs"))]
    records: list[RunRecord] = []
    godotnext_runs: list[Path] = []
    ue_runs: list[tuple[Path, CsvProfile]] = []
    platforms: set[str] = set()
    notes: list[str] = []
    ue_runs_facts: list[dict[str, object]] = []
    godotnext_facts: list[dict[str, object]] = []
    scales: set[float] = set()
    for job in jobs:
        name = str(job.get("name"))
        if name not in results:
            raise ComparisonError(f"run {name} has no result; the set did not finish")
        exit_code = as_int(results[name].get("exit_code"))
        engine = str(job.get("engine"))
        role = str(job.get("role"))
        index = as_int(job.get("index"))
        run_dir = compare_dir / "runs" / name
        if exit_code != 0 or role != "timing":
            records.append(RunRecord(engine, role, index, exit_code, None, False))
            continue
        if engine == "godotnext":
            manifest = _json(run_dir / "manifest.json")
            platforms.add(_platform(manifest))
            scales.add(_scaling_scale(manifest, name))
            godotnext_facts.append({"run": name, **_check_nextgi(config, manifest, name)})
            records.append(_godotnext_record(manifest, index))
            godotnext_runs.append(run_dir)
        elif engine == "ue":
            profile = read_csv_profile(run_dir / UE_PROFILE)
            needed = config.ue.warmup_frames + config.frames
            if profile.frames < needed:
                raise ComparisonError(
                    f"run {name}: the UE capture has {profile.frames} frames; the window needs {needed}"
                )
            runner = _json(run_dir / "runner_job.json")
            log = run_dir / UE_LOG
            log_text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
            facts = _check_ue_run(config, name, runner, profile, log_text, judge=judge, notes=notes)
            ue_runs_facts.append(facts)
            records.append(_ue_record(runner, profile.metadata, facts, index))
            ue_runs.append((run_dir, profile))
        else:
            raise ComparisonError(f"run {name}: unknown engine {engine!r}")
    facts = check_set(
        records,
        judge=judge,
        ue_run_mode=config.ue.run_mode,
        ue_build_version=config.ue.build_version,
        job_kind=str(plan.get("job_kind") or "timing"),
    )
    if len(platforms) != 1:
        raise ComparisonError(f"the GodotNext runs ran on different platforms: {sorted(platforms)}")
    platform = next(iter(platforms))
    if len(scales) != 1:
        raise ComparisonError(f"the GodotNext runs differ in scaling_3d_scale: {sorted(scales)}")
    scale = next(iter(scales))
    if scale != 1.0:
        message = INTERNAL_SIZE_UNVERIFIED.format(scale=scale)
        if judge:
            raise ComparisonError(f"UE's internal resolution is not verified: {message}")
        notes.append(f"UE's internal resolution is not verified: {message}; a judged report refuses the set.")
    lines = []
    for spec in config.lines:
        godotnext_values, godotnext_missing = _godotnext_statistics(spec, config, godotnext_runs)
        ue_values, ue_missing = _ue_statistics(spec, config, ue_runs)
        lines.append(
            evaluate_line(
                spec,
                godotnext_values,
                ue_values,
                judge=judge,
                platform=platform,
                godotnext_missing=godotnext_missing,
                ue_missing=ue_missing,
            )
        )
    facts["notes"] = [RUN_MODE_NOTE, *as_list(facts.get("notes", [])), *_unique(notes)]
    facts["godotnext_scaling_3d_scale"] = scale
    facts["ue_runs"] = ue_runs_facts
    facts["godotnext_runs"] = godotnext_facts
    first_profile = ue_runs[0][1] if ue_runs else None
    return {
        "schema": SCHEMA,
        "kind": "ue_compare_report",
        "mode": "judged" if judge else "dry_run",
        "note": "Judged against the UE bar (PQ)."
        if judge
        else "Functional dry run: not a measurement, no verdict (§22).",
        "comparison": config.name,
        "title": config.title,
        "config_path": plan.get("config_path"),
        "config_sha256": plan.get("config_sha256"),
        "settings_record": config.settings_record,
        "sign_off": config.sign_off,
        "device": config.device,
        "platform": platform,
        "ue_bar": UE_BAR,
        "window": {
            "godotnext_warmup": config.warmup_frames,
            "ue_warmup": config.ue.warmup_frames,
            "frames": config.frames,
            "path_frames": config.ue.path_frames,
        },
        "job_kind": plan.get("job_kind"),
        "set": facts,
        "lines": lines,
        "ue_columns_seen": [] if first_profile is None else list(first_profile.headers),
    }


def render_markdown(report: dict[str, object]) -> str:
    lines_out = [
        f"# UE comparison: {report['title']}",
        "",
        str(report["note"]),
        "",
        f"- Comparison config: `{report['config_path']}` (sha256 {str(report['config_sha256'])[:12]})",
        f"- Settings record: `{report['settings_record']}`; lead sign-off: {report['sign_off'] or 'none yet'}",
        f"- Device: {report['device']} ({report['platform']})",
    ]
    facts = as_dict(report["set"])
    window = as_dict(report["window"])
    resolution = as_list(facts["resolution"])
    godotnext_build = "release export" if facts.get("godotnext_release_export") else "editor binary"
    lines_out += [
        f"- Runs per engine: {facts['runs_per_engine']}, interleaved; warm-up launches: {facts['warmup_launches']};"
        f" jobs: {facts['job_kind']}",
        f"- Resolution {resolution[0]}x{resolution[1]}; clocks locked: {facts['clocks_locked']}",
        f"- Window: GodotNext warm-up {window['godotnext_warmup']}, UE warm-up {window['ue_warmup']},"
        f" {window['frames']} measured frames",
        f"- GodotNext build {facts['godotnext_commit']} ({godotnext_build});"
        f" UE {facts['ue_build_version']}, run mode {facts['ue_run_mode']}",
        *[f"- {note}" for note in as_list(facts.get("notes", []))],
        "",
        "| Line | Gates | Status | Statistic | GodotNext (median; runs) | UE (median; runs) | Ratio | Verdict |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for item in as_list(report["lines"]):
        line = as_dict(item)
        godotnext = as_dict(line["godotnext"])
        ue = as_dict(line["ue"])
        lines_out.append(
            "| "
            + " | ".join([
                str(line["name"]),
                ", ".join(str(gate) for gate in as_list(line["gates"])) or "-",
                str(line["status"]),
                str(line["statistic"]),
                _side(godotnext),
                _side(ue),
                "-" if line["ratio"] is None else f"{float(cast(float, line['ratio'])):.3f}",
                str(line["verdict"] or "-"),
            ])
            + " |"
        )
    notes = [
        f"- {line['name']}: {note}"
        for line in (as_dict(item) for item in as_list(report["lines"]))
        for note in [str(line["reason"])] + [str(n) for n in as_list(line["notes"])]
        if note
    ]
    if notes:
        lines_out += ["", "Notes:", "", *notes]
    seen = as_list(report["ue_columns_seen"])
    lines_out += ["", f"The first UE capture has {len(seen)} columns; the JSON report lists them."]
    return "\n".join(lines_out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.ue_report")
    parser.add_argument("--compare", required=True, help="the directory ue_compare.ps1 wrote")
    parser.add_argument("--out", help="directory for ue_compare_report.json and .md")
    parser.add_argument("--judge", action="store_true", help="PQ: judge the UE-bar lines")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        code = exc.code
        return int(code) if isinstance(code, int) else 2
    try:
        report = build_report(Path(args.compare), judge=args.judge)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        _emit({"kind": "ue_compare_report", "error": str(exc)})
        return 2
    _emit(report)
    if args.out:
        out_dir = Path(args.out)
        out_dir.mkdir(parents=True, exist_ok=True)
        text = json.dumps(json_ready(report), indent=2, sort_keys=True, allow_nan=False) + "\n"
        (out_dir / "ue_compare_report.json").write_text(text, encoding="utf-8")
        (out_dir / "ue_compare_report.md").write_text(render_markdown(report), encoding="utf-8")
    return 0


def _godotnext_statistics(
    spec: LineSpec, config: ComparisonConfig, runs: list[Path]
) -> tuple[list[float] | None, list[int] | None]:
    if spec.godotnext is None:
        return None, None
    values: list[float] = []
    missing: list[int] = []
    for run_dir in runs:
        columns = read_timing_csv(run_dir / "timing.csv")
        names = spec.godotnext.select(list(columns))
        if not names:
            raise ComparisonError(
                f"{run_dir.name}: line {spec.id}: timing.csv has no column {spec.godotnext.to_json()}"
            )
        for name in names:
            if name not in columns:
                raise ComparisonError(f"{run_dir.name}: timing.csv has no column {name!r}")
            if spec.godotnext.columns and all(value is None for value in columns[name]):
                raise ComparisonError(f"{run_dir.name}: column {name!r} has no value")
        length = len(columns.get("frame", []))
        # A frame whose selected cells are all blank is a frame the harness skipped (a repeated
        # timestamp frame): it has no value for the line, not 0 ms.
        totals: list[float | None] = []
        for row in range(length):
            cells = [columns[name][row] for name in names]
            totals.append(None if all(cell is None for cell in cells) else sum(cell or 0.0 for cell in cells))
        if all(total is None or total == 0.0 for total in totals):
            raise ComparisonError(f"{run_dir.name}: line {spec.id}: the selected columns {names} have no value")
        try:
            statistic, dropped = run_statistic(totals, spec.statistic, config.frames)
        except ComparisonError as exc:
            raise ComparisonError(f"{run_dir.name}: line {spec.id}: {exc}") from None
        values.append(statistic)
        missing.append(dropped)
    return values, missing


def _ue_statistics(
    spec: LineSpec, config: ComparisonConfig, runs: list[tuple[Path, CsvProfile]]
) -> tuple[list[float] | None, list[int] | None]:
    if spec.ue is None or spec.ue.columns is None:
        return None, None
    values: list[float] = []
    missing: list[int] = []
    for run_dir, profile in runs:
        try:
            frames = group_ms(profile, spec.ue.columns, config.ue.warmup_frames, config.frames)
            statistic, dropped = run_statistic(frames, spec.statistic, config.frames)
        except (CsvProfileError, ComparisonError) as exc:
            raise ComparisonError(f"{run_dir.name}: line {spec.id}: {exc}") from None
        values.append(statistic)
        missing.append(dropped)
    return values, missing


def _check_nextgi(config: ComparisonConfig, manifest: dict[str, object], name: str) -> dict[str, object]:
    """A run the config expects to run NextGI records it forced on, at the configured tier and quality."""
    expected = config.godotnext.nextgi
    if expected is None:
        return {}
    record = manifest.get("nextgi")
    if not isinstance(record, dict) or record.get("forced") is not True or as_int(record.get("environments", 0)) < 1:
        raise ComparisonError(f"GodotNext run {name} did not force NextGI on in the scene's Environment (--nextgi=on)")
    status = as_dict(record.get("status") or {})
    tier = str(status.get("tier", ""))
    quality = str(status.get("quality", ""))
    if tier != expected.tier or quality != expected.quality:
        raise ComparisonError(
            f"GodotNext run {name} ran NextGI at tier {tier or '?'} and quality {quality or '?'};"
            f" the comparison config expects {expected.tier} at {expected.quality}"
        )
    return {"nextgi_tier": tier, "nextgi_quality": quality, "nextgi_reason": str(status.get("reason", ""))}


def _check_ue_run(
    config: ComparisonConfig,
    name: str,
    runner: dict[str, object],
    profile: CsvProfile,
    log_text: str,
    *,
    judge: bool,
    notes: list[str],
) -> dict[str, object]:
    """The UE run's observed facts, refused when they break the config (names and values only)."""
    metadata = profile.metadata
    ue = config.ue
    token = str(as_dict(runner.get("csv") or {}).get("token") or "")
    if not token or metadata.get(_META_TOKEN) != token:
        raise ComparisonError(f"run {name}: the capture does not carry this launch's csvMetadata token")
    exit_record = as_dict(runner.get("exit") or {})
    if exit_record.get("csv_exit") is not True or exit_record.get("fatal") is not False:
        raise ComparisonError(
            f"run {name}: the UE run did not end cleanly through -ExitAfterCsvProfiling"
            f" (csv_exit {exit_record.get('csv_exit')}, fatal {exit_record.get('fatal')})"
        )
    # The runner counts the log lines that report a material without its usage flag or the default
    # material in its place (content that did not draw as imported); only 0 counts.
    fallback = exit_record.get("material_fallback_lines")
    if type(fallback) is not int or fallback != 0:
        raise ComparisonError(
            f"run {name}: the UE log reports a material without its usage flag or the default material"
            f" in its place (material_fallback_lines {fallback}), so the capture is not of the imported content"
        )
    expected = {
        "gpu": config.device,
        "rhiname": ue.rhi,
        "rhifeaturelevel": ue.feature_level,
        "vsyncenabled": "0",
    }
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ComparisonError(f"run {name}: the capture records {key} {metadata.get(key)!r}, not {value!r}")
    wanted = ue.checked_cvars()
    echoed = read_log_cvars(log_text, list(wanted))
    for cvar, value in wanted.items():
        seen = echoed.get(cvar)
        if seen is None:
            raise ComparisonError(f"run {name}: the UE log has no echo of {cvar}; the report cannot check it")
        if not _same_value(seen, value):
            raise ComparisonError(f"run {name}: {cvar} was {seen!r} in the run, not {value!r}")
    activity = window_activity(profile, ue.window_quiet_columns, ue.warmup_frames, config.frames)
    busy = {header: rows for header, rows in activity.items() if rows}
    if busy:
        message = f"run {name}: stats that must be quiet after the warm-up are not: {busy}"
        if judge:
            raise ComparisonError(message)
        notes.append(message[0].upper() + message[1:] + ".")
    run_mode = _observed_run_mode(runner, metadata)
    if run_mode != ue.run_mode:
        raise ComparisonError(f"run {name}: the UE run was {run_mode}, not {ue.run_mode} (plan v3.9.3)")
    editor = as_dict(runner.get("ue_editor") or {})
    return {
        "run": name,
        "run_mode": run_mode,
        "buildversion": metadata.get("buildversion", ""),
        "editor_product_version": str(editor.get("product_version", "")),
        "changelist": _changelist(str(editor.get("product_version", "")) or metadata.get("buildversion", "")),
        "rhi": metadata.get("rhiname", ""),
        "feature_level": metadata.get("rhifeaturelevel", ""),
        "cvars": {cvar: echoed[cvar] for cvar in wanted},
        "quiet_columns_active_rows": activity,
        "exit_code": exit_record.get("code"),
        "children_alive_after": as_list(exit_record.get("children_alive_after", [])),
        "priority": str(runner.get("priority", "")),
        "gpu_after": runner.get("gpu_after"),
    }


def _observed_run_mode(runner: dict[str, object], metadata: dict[str, str]) -> str:
    """editor_game when the runner launched the installed editor and the run's command line has -game."""
    exe = str(runner.get("exe") or "")
    command = metadata.get("commandline")
    source = command if command is not None else " ".join(str(arg) for arg in as_list(runner.get("args", [])))
    if exe.lower() == UE_EDITOR_EXE.lower() and re.search(r"(^|\s)-game(\s|$)", source, re.IGNORECASE):
        return "editor_game"
    return f"unknown ({exe or 'no exe recorded'})"


def _changelist(version: str) -> str:
    match = re.search(r"CL-(\d+)", version)
    return match.group(1) if match else ""


def _same_value(seen: str, wanted: str) -> bool:
    if seen == wanted:
        return True
    try:
        return float(seen) == float(wanted)
    except ValueError:
        return False


def _scaling_scale(manifest: dict[str, object], name: str) -> float:
    value = manifest.get("scaling_3d_scale")
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ComparisonError(f"GodotNext run {name} does not record its scaling_3d_scale")
    return float(value)


def _godotnext_record(manifest: dict[str, object], index: int) -> RunRecord:
    if manifest.get("mode") != "timing":
        raise ComparisonError(f"GodotNext run {index} is not a harness timing run")
    runner = as_dict(manifest.get("runner") or {})
    clock = as_dict(runner.get("clock") or {})
    size = manifest.get("output_size")
    resolution = None
    if isinstance(size, list) and len(size) == 2:
        resolution = (as_int(size[0]), as_int(size[1]))
    editor = manifest.get("editor_binary")
    gpu_after = runner.get("gpu_after")
    return RunRecord(
        engine="godotnext",
        role="timing",
        index=index,
        exit_code=0,
        resolution=resolution,
        clocks_locked=clock.get("locked") is True,
        godotnext_commit=str(manifest.get("commit") or ""),
        godotnext_editor_binary=editor if isinstance(editor, bool) else None,
        gpu_after=gpu_after if isinstance(gpu_after, dict) else None,
    )


def _ue_record(runner: dict[str, object], metadata: dict[str, str], facts: dict[str, object], index: int) -> RunRecord:
    clock = as_dict(runner.get("clock") or {})
    resolution = None
    width = metadata.get("systemresolution.resx", "")
    height = metadata.get("systemresolution.resy", "")
    if width.isdigit() and height.isdigit():
        resolution = (int(width), int(height))
    gpu_after = runner.get("gpu_after")
    return RunRecord(
        engine="ue",
        role="timing",
        index=index,
        exit_code=0,
        resolution=resolution,
        clocks_locked=clock.get("locked") is True,
        ue_build_version=metadata.get("buildversion", ""),
        ue_run_mode=str(facts.get("run_mode", "")),
        ue_vsync=metadata.get("vsyncenabled", ""),
        gpu_after=gpu_after if isinstance(gpu_after, dict) else None,
    )


def _platform(manifest: dict[str, object]) -> str:
    """The runners' host labels: a fleet.md device name on Windows, "Apple M5 Max", "Linux VM"."""
    host = str(manifest.get("host") or "").lower()
    if not host:
        return "unknown"
    if "linux" in host:
        return "linux"
    if "apple" in host or "macos" in host:
        return "macos"
    return "windows"


def _results(value: object) -> dict[str, dict[str, object]]:
    """results.json is a list of {name, exit_code, ...}; PowerShell may write a lone result as an object."""
    items = [value] if isinstance(value, dict) else as_list(value)
    return {str(as_dict(item).get("name")): as_dict(item) for item in items}


def _side(side: dict[str, object]) -> str:
    median = side.get("median")
    if median is None:
        return "-"
    runs = ", ".join(f"{float(cast(float, value)):.3f}" for value in as_list(side.get("runs")))
    return f"{float(cast(float, median)):.3f}; {runs}"


def _unique(items: list[str]) -> list[str]:
    return list(dict.fromkeys(items))


def _json(path: Path) -> dict[str, object]:
    return as_dict(_json_any(path))


def _json_any(path: Path) -> object:
    if not path.is_file():
        raise ComparisonError(f"missing {path.name} in {path.parent}")
    return cast(object, json.loads(path.read_text(encoding="utf-8-sig")))


def _emit(report: dict[str, object]) -> None:
    json.dump(json_ready(report), sys.stdout, sort_keys=True, allow_nan=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    sys.exit(main())
