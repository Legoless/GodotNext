"""The interleaved-set report (#229) on a synthetic compare tree.

Each run is written the way the runners leave it: a GodotNext run has the
harness's ``timing.csv`` and ``manifest.json``; a UE run has the runner's
``runner_job.json``, ``ue_profile.csv`` and ``ue.log``, here a hand-written
capture and log in the observed formats with invented stat names.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest
from godotnext_metrics.ue_bar import DRY_RUN_NOTE, LINUX_REASON, ComparisonError
from godotnext_metrics.ue_compare import build_plan, parse_config, write_plan
from godotnext_metrics.ue_report import RUN_MODE_NOTE, build_report, main, render_markdown
from test_ue_compare import UE_CSV_DIR, UE_EXE, UE_PROJECT, nextgi_config, synthetic_config

UE_HEADER = ["EVENTS", "Synthetic/GameThread/Work", "Synthetic/GPU/PassA", "Synthetic/GPU/PassB", "Synthetic/Hitch"]
GODOTNEXT_BASES = [1.10, 1.30, 1.20, 1.25, 1.15]
UE_BASES = [1.00, 0.95, 1.05, 1.02, 0.98]
SIGN_OFF = "synthetic sign-off reference"
BUILD = "++Synthetic+Release-0.0-CL-1"
METADATA = {
    "gpu": "NVIDIA GeForce RTX 4080 SUPER",
    "rhiname": "D3D12",
    "rhifeaturelevel": "SM6",
    "vsyncenabled": "0",
    "buildversion": BUILD,
    "systemresolution.resx": "1920",
    "systemresolution.resy": "1080",
    "config": "Development",
    "deviceprofile": "WindowsEditor",
    "commandline": '"X:/Synthetic/Synthetic.uproject" /Game/Ports/Synthetic/Synthetic.umap -game -dx12 -ExecCmds="a 1,b 2"',
}


def _lines(unnamed: bool = False, ue_columns: list[str] | None = None) -> list[dict[str, object]]:
    lines: list[dict[str, object]] = [
        {
            "id": "total",
            "name": "Feature total",
            "gates": ["GX-1"],
            "status": "ue_bar",
            "statistic": "p95",
            "direction": "lower",
            "godotnext": {"columns": ["feature_a_ms", "feature_b_ms"]},
            "ue": {"group": "Feature", "columns": ue_columns or ["Synthetic/GPU/PassA", "Synthetic/GPU/PassB"]},
        },
        {
            "id": "pass_sum",
            "name": "GPU pass sum",
            "gates": [],
            "status": "report_only",
            "reason": "Pipeline check.",
            "statistic": "p50",
            "direction": "lower",
            "godotnext": {"all_passes": True},
            "ue": {"group": "Pass A", "columns": ["Synthetic/GPU/PassA"]},
        },
        {
            "id": "godotnext_only",
            "name": "GodotNext-only mode",
            "gates": ["GX-2"],
            "status": "no_equivalent",
            "reason": "UE has no equivalent mode.",
            "statistic": "p99",
            "direction": "lower",
            "godotnext": {"columns": ["other_ms"]},
            "ue": None,
        },
    ]
    if unnamed:
        lines.append({
            "id": "unnamed",
            "name": "Feature with an unnamed UE column",
            "gates": ["GX-3"],
            "status": "ue_bar",
            "statistic": "p95",
            "direction": "lower",
            "godotnext": {"columns": ["feature_a_ms"]},
            "ue": {"group": "Unnamed", "columns": None},
        })
    return lines


def _block_line() -> dict[str, object]:
    return {
        "id": "block",
        "name": "Block total",
        "gates": ["GX-4"],
        "status": "ue_bar",
        "statistic": "p50",
        "direction": "lower",
        "godotnext": {"prefix": "nextgi_", "exclude": ["nextgi_2_ms"]},
        "ue": {"group": "Feature", "columns": ["Synthetic/GPU/PassA"]},
    }


def _timing_csv(base: float, frames: int, blank_rows: int = 0) -> str:
    rows = ["frame,wall_ms,feature_a_ms,feature_b_ms,other_ms,nextgi_ms,nextgi_trace_ms,nextgi_2_ms"]
    for frame in range(frames):
        if frame < blank_rows:
            # A repeated timestamp frame: the harness writes wall time and no pass.
            rows.append(f"{frame},16.6,,,,,,")
        else:
            rows.append(f"{frame},16.6,{base / 2:.6f},{base / 2:.6f},0.250000,0.100000,0.400000,9.000000")
    return "\n".join(rows) + "\n"


def _ue_csv(
    base: float,
    warmup: int,
    frames: int,
    metadata: dict[str, str],
    hitch_in_window: bool = False,
    idle_work_rows: int = 0,
) -> str:
    rows = [",".join(UE_HEADER)]
    for _ in range(warmup):
        rows.append(",50.0,40.0,40.0,1")
    for frame in range(frames):
        hitch = 1 if hitch_in_window and frame == 1 else 0
        # The first idle_work_rows window rows of the work column are 0 (a missed stat sample).
        work = 0 if frame < idle_work_rows else 1.0
        rows.append(f",{work},{base / 2:.6f},{base / 2:.6f},{hitch}")
    rows.append(",".join(UE_HEADER))
    rows.append(",".join(["[HasHeaderRowAtEnd],1"] + [f"[{key}],{value}" for key, value in metadata.items()]))
    return "\n".join(rows) + "\n"


def _ue_log(cvars: dict[str, str], extra: list[str] | None = None) -> str:
    lines = ["[2026.11.15-12.00.00:000][  0]LogInit: Display: synthetic start"]
    for name, value in cvars.items():
        lines.append(f"[2026.11.15-12.00.01:000][  0]LogConsoleResponse: Display: Cmd: {name} {value}")
    for name, value in cvars.items():
        # The observed echo: no log category, a LastSetBy note after the value.
        lines.append(f'[2026.11.15-12.00.01:001][  0]{name} = "{value}"      LastSetBy: Console')
    return "\n".join([*lines, *(extra or [])]) + "\n"


def _tsr_event(width: int, height: int, out_width: int = 1920, out_height: int = 1080) -> str:
    """A synthetic ProfileGPU line in the observed shape of the TSR draw event."""
    return (
        f"[2026.11.15-12.00.02:000][  2]LogSynthetic:  1.00ms  TemporalSuperResolution(synthetic)"
        f" {width}x{height} -> {out_width}x{out_height}"
    )


def make_set(
    root: Path,
    *,
    runs: int = 5,
    warmup: bool = True,
    config: dict[str, object] | None = None,
    godotnext_bases: list[float] | None = None,
    host: str = "NVIDIA GeForce RTX 4080 SUPER",
    fail: str = "",
    godotnext_frames: int | None = None,
    ue_frames: int | None = None,
    metadata: dict[str, str] | None = None,
    cvars: dict[str, str] | None = None,
    exit_record: dict[str, object] | None = None,
    exe: str = "UnrealEditor.exe",
    hitch_in_window: bool = False,
    blank_rows: int = 0,
    editor_binary: bool = True,
    nextgi: dict[str, object] | None = None,
    kind: str = "run",
    wrong_token: bool = False,
    scales: list[float | None] | None = None,
    log_extra: list[str] | None = None,
    dpi_layer: str | None = "HighDpiAware",
    idle_work_rows: int = 0,
    adapter: str | None = None,
    debug: bool | None = False,
    source_commit: str | None = "abc123",
    source_dirty: bool = False,
) -> Path:
    parsed = parse_config(config or synthetic_config(lines=_lines()))
    plan = build_plan(
        parsed,
        config_label="ue_compare/synthetic.json",
        config_sha256="0" * 64,
        out_dir=root,
        ue_exe=UE_EXE,
        ue_project=UE_PROJECT,
        ue_csv_dir=UE_CSV_DIR,
        runs=runs,
        warmup=warmup,
        kind=kind,
    )
    write_plan(plan, root)
    bases = godotnext_bases or GODOTNEXT_BASES
    results = []
    jobs = plan["jobs"]
    assert isinstance(jobs, list)
    for job in jobs:
        run_dir = root / "runs" / job["name"]
        run_dir.mkdir(parents=True)
        code = 1 if job["name"] == fail else 0
        results.append({"name": job["name"], "exit_code": code})
        if code or job["role"] != "timing":
            continue
        position = (job["index"] - 1) % len(bases)
        if job["engine"] == "godotnext":
            frames = godotnext_frames or parsed.frames
            csv_text = _timing_csv(bases[position], frames, blank_rows)
            (run_dir / "timing.csv").write_text(csv_text, encoding="utf-8")
            runner_record: dict[str, object] = {
                "clock": {"locked": False, "detail": "Clocks are not locked for this job."}
            }
            if source_commit is not None:
                runner_record["source"] = {"commit": source_commit, "dirty": source_dirty}
            manifest: dict[str, object] = {
                "schema": 1,
                "host": host,
                "adapter": {"name": parsed.device if adapter is None else adapter, "api_version": "synthetic"},
                "mode": "timing",
                "commit": "abc123",
                "output_size": [1920, 1080],
                "editor_binary": editor_binary,
                "runner": runner_record,
            }
            if debug is not None:
                manifest["debug"] = debug
            scale = (scales or [1.0])[position % len(scales or [1.0])]
            if scale is not None:
                manifest["scaling_3d_scale"] = scale
            if nextgi is not None:
                manifest["nextgi"] = nextgi
            (run_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        else:
            frames = ue_frames or parsed.frames
            token = f"token{job['name']}"
            values = dict(metadata or METADATA)
            values["godotnextrun"] = "other" if wrong_token else token
            capture = _ue_csv(
                UE_BASES[position], parsed.ue.warmup_frames, frames, values, hitch_in_window, idle_work_rows
            )
            (run_dir / "ue_profile.csv").write_text(capture, encoding="utf-8")
            log = _ue_log(cvars or parsed.ue.checked_cvars(), log_extra)
            (run_dir / "ue.log").write_text(log, encoding="utf-8")
            runner = {
                "engine": "ue",
                "exe": exe,
                "args": job_args(plan, job),
                "clock": {"locked": False},
                "priority": "BelowNormal",
                "csv": {"token": token, "collected": True},
                "exit": exit_record
                or {
                    "code": 0,
                    "csv_exit": True,
                    "log_exit": True,
                    "fatal": False,
                    "material_fallback_lines": 0,
                },
                "ue_editor": {"product_version": BUILD},
                "env": {} if dpi_layer is None else {"__COMPAT_LAYER": dpi_layer},
                "gpu_after": {"clocks_gr_mhz": 2610, "clocks_mem_mhz": 11501, "temperature_c": 45},
            }
            (run_dir / "runner_job.json").write_text(json.dumps(runner), encoding="utf-8")
    (root / "results.json").write_text(json.dumps(results), encoding="utf-8")
    return root


def job_args(plan: dict[str, object], job: dict[str, object]) -> list[object]:
    files = plan.get("job_files")
    if isinstance(files, dict) and job["job"] in files:
        return list(files[job["job"]]["args"])
    args: list[object] = json.loads(Path(str(job["job"])).read_text(encoding="utf-8"))["args"]
    return args


def _line(report: dict[str, object], line_id: str) -> dict[str, object]:
    lines = report["lines"]
    assert isinstance(lines, list)
    return next(line for line in lines if line["id"] == line_id)


def _judged_config(**overrides: object) -> dict[str, object]:
    config: dict[str, object] = synthetic_config(lines=_lines(), sign_off=SIGN_OFF)
    ue = copy.deepcopy(config["ue"])
    assert isinstance(ue, dict)
    ue["build_version"] = BUILD
    config["ue"] = ue
    config.update(overrides)
    return config


def test_dry_run_has_numbers_and_ratios_but_no_verdict(tmp_path: Path) -> None:
    report = build_report(make_set(tmp_path), judge=False)
    assert report["mode"] == "dry_run"
    assert "not a measurement" in str(report["note"])
    total = _line(report, "total")
    assert total["godotnext"]["median"] == pytest.approx(1.20)  # type: ignore[index]
    assert total["ue"]["median"] == pytest.approx(1.00)  # type: ignore[index]
    # The UE warm-up rows (80 ms) are dropped from the window.
    assert total["ue"]["runs"] == pytest.approx(UE_BASES)  # type: ignore[index]
    assert total["ratio"] == pytest.approx(1.20)
    assert total["verdict"] is None
    assert DRY_RUN_NOTE in total["notes"]  # type: ignore[operator]
    pass_sum = _line(report, "pass_sum")
    # GodotNext's pass sum adds every pass column.
    assert pass_sum["godotnext"]["median"] == pytest.approx(1.20 + 0.25 + 0.1 + 0.4 + 9.0)  # type: ignore[index]
    assert pass_sum["ue"]["median"] == pytest.approx(0.50)  # type: ignore[index]
    only = _line(report, "godotnext_only")
    assert only["godotnext"]["median"] == pytest.approx(0.25)  # type: ignore[index]
    assert only["ratio"] is None
    assert report["ue_columns_seen"] == UE_HEADER
    facts = report["set"]
    assert isinstance(facts, dict)
    assert facts["runs_per_engine"] == 5
    assert facts["ue_build_version"] == BUILD
    assert facts["ue_run_mode"] == "editor_game"
    assert facts["notes"][0] == RUN_MODE_NOTE
    # The editor binary and the unpinned build are noted, not refused, in a dry run.
    assert any("editor binary" in note for note in facts["notes"])
    ue_run = facts["ue_runs"][0]
    assert ue_run["changelist"] == "1"
    assert ue_run["config"] == "Development"
    assert ue_run["deviceprofile"] == "WindowsEditor"
    godotnext_run = facts["godotnext_runs"][0]
    assert godotnext_run["adapter"] == "NVIDIA GeForce RTX 4080 SUPER"
    assert godotnext_run["debug"] is False
    assert godotnext_run["source_commit"] == "abc123"
    assert godotnext_run["source_dirty"] is False
    assert godotnext_run["resolved_columns"]["total"] == ["feature_a_ms", "feature_b_ms"]
    assert facts["godotnext_source_commit"] == "abc123"
    assert facts["godotnext_source_dirty"] is False
    # The report lists every echoed value it checked: the screen percentage, the twelve scalability
    # groups and the three async compute switches.
    assert len(ue_run["cvars"]) == 16
    assert ue_run["cvars"]["r.ScreenPercentage"] == "100"
    assert ue_run["cvars"]["sg.LandscapeQuality"] == ue_run["cvars"]["sg.GlobalIlluminationQuality"] == "2"
    assert ue_run["cvars"]["r.RDG.AsyncCompute"] == ue_run["cvars"]["r.TSR.AsyncCompute"] == "0"
    assert ue_run["active_columns_rows"] == {}
    assert ue_run["rhi"] == "D3D12" and ue_run["feature_level"] == "SM6"
    assert report["platform"] == "windows"


def test_a_one_pair_dry_run_is_allowed(tmp_path: Path) -> None:
    report = build_report(make_set(tmp_path, runs=1, warmup=False), judge=False)
    assert _line(report, "total")["ratio"] == pytest.approx(1.10 / 1.00)


def test_judged_set_writes_verdicts(tmp_path: Path) -> None:
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    report = build_report(make_set(tmp_path / "pass", **options), judge=True)  # type: ignore[arg-type]
    assert report["mode"] == "judged"
    assert _line(report, "total")["verdict"] == "pass"
    assert _line(report, "pass_sum")["verdict"] is None
    assert _line(report, "godotnext_only")["verdict"] is None
    slow = make_set(tmp_path / "fail", godotnext_bases=[1.30, 1.40, 1.35, 1.25, 1.45], **options)  # type: ignore[arg-type]
    assert _line(build_report(slow, judge=True), "total")["verdict"] == "fail"


def test_judging_needs_the_sign_off_five_runs_timing_jobs_and_a_release_export(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="sign-off"):
        build_report(make_set(tmp_path / "unsigned"), judge=True)
    config = _judged_config()
    with pytest.raises(ComparisonError, match="5 runs per engine, not 2"):
        build_report(make_set(tmp_path / "short", runs=2, config=config, kind="timing"), judge=True)
    with pytest.raises(ComparisonError, match="timing jobs"):
        build_report(make_set(tmp_path / "run", config=config, editor_binary=False), judge=True)
    with pytest.raises(ComparisonError, match="editor binary"):
        build_report(make_set(tmp_path / "editor", config=config, kind="timing"), judge=True)


def test_godotnext_adapter_must_match_the_comparison_device(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="records adapter 'Synthetic GPU'"):
        build_report(make_set(tmp_path, adapter="Synthetic GPU"), judge=False)


def test_judging_refuses_a_debug_or_unclassified_godotnext_export(tmp_path: Path) -> None:
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    with pytest.raises(ComparisonError, match="debug export"):
        build_report(make_set(tmp_path / "debug", debug=True, **options), judge=True)  # type: ignore[arg-type]
    with pytest.raises(ComparisonError, match="does not record its debug state"):
        build_report(make_set(tmp_path / "unknown", debug=None, **options), judge=True)  # type: ignore[arg-type]


def test_godotnext_source_commit_and_dirty_state_are_recorded_and_judged(tmp_path: Path) -> None:
    missing = build_report(make_set(tmp_path / "missing", source_commit=None), judge=False)
    missing_run = missing["set"]["godotnext_runs"][0]  # type: ignore[index]
    assert missing_run["source_commit"] == "" and missing_run["source_dirty"] is None
    assert missing["set"]["godotnext_source_dirty"] is None  # type: ignore[index]
    dirty = build_report(make_set(tmp_path / "dirty", source_dirty=True), judge=False)
    assert dirty["set"]["godotnext_source_dirty"] is True  # type: ignore[index]
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    with pytest.raises(ComparisonError, match="source commit and dirty flag"):
        build_report(make_set(tmp_path / "judge_missing", source_commit=None, **options), judge=True)  # type: ignore[arg-type]
    with pytest.raises(ComparisonError, match="dirty source tree"):
        build_report(make_set(tmp_path / "judge_dirty", source_dirty=True, **options), judge=True)  # type: ignore[arg-type]
    mixed = make_set(tmp_path / "mixed", **options)  # type: ignore[arg-type]
    other = mixed / "runs" / "04_g_t2" / "manifest.json"
    manifest = json.loads(other.read_text(encoding="utf-8"))
    manifest["runner"]["source"]["commit"] = "def456"
    other.write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ComparisonError, match="do not share one source commit"):
        build_report(mixed, judge=True)


def test_an_unnamed_ue_column_is_reported_in_a_dry_run_and_refused_when_judged(tmp_path: Path) -> None:
    config = _judged_config(lines=_lines(unnamed=True))
    root = make_set(tmp_path, config=config, kind="timing", editor_binary=False)
    unnamed = _line(build_report(root, judge=False), "unnamed")
    assert unnamed["ue"]["median"] is None  # type: ignore[index]
    assert unnamed["status"] == "ue_bar"
    with pytest.raises(ComparisonError, match="UE column is not set"):
        build_report(root, judge=True)


def test_a_named_column_missing_from_the_capture_stops_the_report(tmp_path: Path) -> None:
    config = synthetic_config(lines=_lines(ue_columns=["Synthetic/GPU/PassA", "Synthetic/GPU/PassD"]))
    with pytest.raises(ComparisonError, match="no column 'Synthetic/GPU/PassD'"):
        build_report(make_set(tmp_path, config=config), judge=False)


def test_a_failed_run_stops_the_report(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="runs failed: ue timing 3"):
        build_report(make_set(tmp_path, fail="07_u_t3"), judge=False)


@pytest.mark.parametrize(
    ("key", "value", "match"),
    [
        ("gpu", "Synthetic GPU", "records gpu"),
        ("rhiname", "Vulkan", "records rhiname"),
        ("rhifeaturelevel", "SM5", "records rhifeaturelevel"),
        ("vsyncenabled", "1", "records vsyncenabled"),
    ],
)
def test_the_ue_run_is_checked_against_its_own_metadata(tmp_path: Path, key: str, value: str, match: str) -> None:
    metadata = {**METADATA, key: value}
    with pytest.raises(ComparisonError, match=match):
        build_report(make_set(tmp_path, metadata=metadata), judge=False)


def test_the_ue_resolution_comes_from_the_capture_not_the_command_line(tmp_path: Path) -> None:
    # The UE run's own check refuses a capture that records another size than the set's, before the
    # set check compares the engines.
    metadata = {**METADATA, "systemresolution.resx": "1280", "systemresolution.resy": "720"}
    with pytest.raises(ComparisonError, match="records 1280x720, not the set's 1920x1080"):
        build_report(make_set(tmp_path / "small", metadata=metadata), judge=False)
    unknown = {key: value for key, value in METADATA.items() if not key.startswith("systemresolution")}
    with pytest.raises(ComparisonError, match="does not record its resolution"):
        build_report(make_set(tmp_path / "unknown", metadata=unknown), judge=False)


def test_a_2160p_set_refuses_a_ue_run_at_another_size(tmp_path: Path) -> None:
    # A 3840x2160 request recorded 3072x1728 fullscreen while the editor was not DPI-aware (#232).
    config = synthetic_config(resolution=[3840, 2160], lines=_lines())
    scaled = {**METADATA, "systemresolution.resx": "3072", "systemresolution.resy": "1728"}
    with pytest.raises(ComparisonError, match="records 3072x1728, not the set's 3840x2160"):
        build_report(make_set(tmp_path / "scaled", config=config, metadata=scaled), judge=False)
    # At the set's size the UE runs pass their own check, and the set check still compares them with the
    # GodotNext runs' output size (1920x1080 in these synthetic manifests).
    native = {**METADATA, "systemresolution.resx": "3840", "systemresolution.resy": "2160"}
    with pytest.raises(ComparisonError, match="differ in resolution"):
        build_report(make_set(tmp_path / "native", config=config, metadata=native), judge=False)


def test_the_forced_console_variables_are_checked_in_the_log(tmp_path: Path) -> None:
    echoed = parse_config(synthetic_config(lines=_lines())).ue.checked_cvars()
    with pytest.raises(ComparisonError, match="sg.GlobalIlluminationQuality was '3' in the run, not '2'"):
        build_report(make_set(tmp_path / "other", cvars={**echoed, "sg.GlobalIlluminationQuality": "3"}), judge=False)
    # Every pinned scalability group and async compute switch is checked, not only the set's own.
    with pytest.raises(ComparisonError, match="r.TSR.AsyncCompute was '2' in the run, not '0'"):
        build_report(make_set(tmp_path / "async", cvars={**echoed, "r.TSR.AsyncCompute": "2"}), judge=False)
    # A boolean echo counts as its 0 or 1: true where 0 is wanted is another value.
    with pytest.raises(ComparisonError, match="r.RDG.AsyncCompute was 'true' in the run, not '0'"):
        build_report(make_set(tmp_path / "boolean", cvars={**echoed, "r.RDG.AsyncCompute": "true"}), judge=False)
    missing = {name: value for name, value in echoed.items() if name != "sg.LandscapeQuality"}
    with pytest.raises(ComparisonError, match="no echo of sg.LandscapeQuality"):
        build_report(make_set(tmp_path / "missing", cvars=missing), judge=False)
    # A float echo of an integer value is the same value, and so is a boolean's true/false echo.
    report = build_report(
        make_set(
            tmp_path / "float", cvars={**echoed, "r.ScreenPercentage": "100.000000", "r.RDG.AsyncCompute": "false"}
        ),
        judge=False,
    )
    assert report["mode"] == "dry_run"


def test_the_ue_launch_must_be_dpi_aware(tmp_path: Path) -> None:
    # The runner gives every UE launch the HighDpiAware compatibility layer (lead decision 2026-11-16).
    report = build_report(make_set(tmp_path / "aware"), judge=False)
    assert report["set"]["ue_runs"][0]["dpi_layer"] == "HighDpiAware"  # type: ignore[index]
    with pytest.raises(ComparisonError, match="not DPI-aware"):
        build_report(make_set(tmp_path / "unaware", dpi_layer=None), judge=False)


def test_only_a_clean_game_mode_exit_with_this_launchs_capture_counts(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="did not end cleanly"):
        build_report(
            make_set(tmp_path / "nocsv", exit_record={"code": 0, "csv_exit": False, "fatal": False}), judge=False
        )
    with pytest.raises(ComparisonError, match="did not end cleanly"):
        build_report(
            make_set(tmp_path / "fatal", exit_record={"code": 0, "csv_exit": True, "fatal": True}), judge=False
        )
    # A clean exit whose log reports a material without its usage flag (or the default material in
    # its place) is refused too, and so is a runner record without that count.
    clean: dict[str, object] = {"code": 0, "csv_exit": True, "log_exit": True, "fatal": False}
    with pytest.raises(ComparisonError, match="without its usage flag"):
        build_report(make_set(tmp_path / "fallback", exit_record={**clean, "material_fallback_lines": 2}), judge=False)
    with pytest.raises(ComparisonError, match="material_fallback_lines None"):
        build_report(make_set(tmp_path / "nocount", exit_record=clean), judge=False)
    with pytest.raises(ComparisonError, match="csvMetadata token"):
        build_report(make_set(tmp_path / "token", wrong_token=True), judge=False)
    with pytest.raises(ComparisonError, match="not editor_game"):
        build_report(make_set(tmp_path / "packaged", exe="SyntheticGame.exe"), judge=False)
    no_game = {**METADATA, "commandline": '"X:/Synthetic/Synthetic.uproject" -dx12'}
    with pytest.raises(ComparisonError, match="not editor_game"):
        build_report(make_set(tmp_path / "editor", metadata=no_game), judge=False)


def test_pso_hitches_after_the_warm_up_are_noted_in_a_dry_run_and_refused_when_judged(tmp_path: Path) -> None:
    report = build_report(make_set(tmp_path / "dry", hitch_in_window=True), judge=False)
    facts = report["set"]
    assert isinstance(facts, dict)
    assert any("must be quiet after the warm-up" in note for note in facts["notes"])
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    with pytest.raises(ComparisonError, match="must be quiet after the warm-up"):
        build_report(make_set(tmp_path / "judged", hitch_in_window=True, **options), judge=True)  # type: ignore[arg-type]


def test_a_missing_quiet_column_is_refused_even_in_a_dry_run(tmp_path: Path) -> None:
    config = synthetic_config(lines=_lines())
    ue = copy.deepcopy(config["ue"])
    assert isinstance(ue, dict)
    ue["window_quiet_columns"] = ["Synthetic/Missing"]
    config["ue"] = ue
    with pytest.raises(ComparisonError, match="no 'Synthetic/Missing' column, which must stay quiet"):
        build_report(make_set(tmp_path, config=config), judge=False)


def _active_config(columns: list[str], frames: int = 6) -> dict[str, object]:
    config: dict[str, object] = synthetic_config(lines=_lines(), frames=frames)
    ue = copy.deepcopy(config["ue"])
    assert isinstance(ue, dict)
    ue["window_quiet_columns"] = []
    ue["window_active_columns"] = columns
    config["ue"] = ue
    return config


def test_the_active_columns_must_hold_values_in_the_window(tmp_path: Path) -> None:
    # A set relies on a feature no line times (Lumen in the lights and shadows sets): the column must be
    # in the capture and hold a value on all but the missed-sample share of the window, even in a dry run.
    report = build_report(make_set(tmp_path / "on", config=_active_config(["Synthetic/GPU/PassA"])), judge=False)
    assert report["set"]["ue_runs"][0]["active_columns_rows"] == {"Synthetic/GPU/PassA": 6}  # type: ignore[index]
    with pytest.raises(ComparisonError, match="no 'Synthetic/GPU/Missing' column"):
        build_report(make_set(tmp_path / "absent", config=_active_config(["Synthetic/GPU/Missing"])), judge=False)
    with pytest.raises(ComparisonError, match="'Synthetic/Hitch' has a value on 0 of 6 window rows"):
        build_report(make_set(tmp_path / "zero", config=_active_config(["Synthetic/Hitch"])), judge=False)
    # Up to 5 % of the window may be a missed stat sample (3 of 60 rows), as for a line's columns.
    work = _active_config(["Synthetic/GameThread/Work"], frames=60)
    report = build_report(make_set(tmp_path / "missed", config=work, idle_work_rows=3), judge=False)
    assert report["set"]["ue_runs"][0]["active_columns_rows"] == {"Synthetic/GameThread/Work": 57}  # type: ignore[index]
    with pytest.raises(ComparisonError, match="a value on 56 of 60 window rows; the set needs it active"):
        build_report(make_set(tmp_path / "idle", config=work, idle_work_rows=4), judge=False)


def test_an_unverified_internal_resolution_is_noted_in_a_dry_run_and_refused_when_judged(tmp_path: Path) -> None:
    # No UE capture column records UE's internal resolution (#232): without the TSR draw event of a
    # ProfileGPU dump in the log, a set that renders below the output size cannot show both engines
    # upscaled from the same size.
    report = build_report(make_set(tmp_path / "dry", scales=[0.5]), judge=False)
    facts = report["set"]
    assert isinstance(facts, dict)
    assert facts["godotnext_scaling_3d_scale"] == 0.5
    assert any("internal resolution is not verified" in note and "0.5" in note for note in facts["notes"])
    assert _line(report, "total")["ratio"] == pytest.approx(1.20)
    native = build_report(make_set(tmp_path / "native"), judge=False)
    assert not any("internal resolution" in note for note in native["set"]["notes"])  # type: ignore[index]
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    with pytest.raises(ComparisonError, match="internal resolution is not verified"):
        build_report(make_set(tmp_path / "judged", scales=[0.5], **options), judge=True)  # type: ignore[arg-type]


def test_the_internal_resolution_comes_from_the_tsr_draw_event(tmp_path: Path) -> None:
    # The TSR draw event in the log shows the rendering size: half the output at scaling_3d_scale 0.5.
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    half = [_tsr_event(960, 540)]
    report = build_report(make_set(tmp_path / "half", scales=[0.5], log_extra=half, **options), judge=True)  # type: ignore[arg-type]
    facts = report["set"]
    assert isinstance(facts, dict)
    assert not any("internal resolution" in note for note in facts["notes"])
    assert all(run["upscale_sizes"] == [[960, 540, 1920, 1080]] for run in facts["ue_runs"])
    # Another rendering size, or another output size, refuses the set even in a dry run.
    with pytest.raises(
        ComparisonError, match="UE rendered at 1920x1080 for 1920x1080; the GodotNext runs render at 960x540"
    ):
        build_report(make_set(tmp_path / "full", scales=[0.5], log_extra=[_tsr_event(1920, 1080)]), judge=False)
    with pytest.raises(ComparisonError, match="for 3840x2160"):
        build_report(
            make_set(tmp_path / "output", scales=[0.5], log_extra=[_tsr_event(960, 540, 3840, 2160)]), judge=False
        )
    # GodotNext's manifest records the scale, not the size, and either engine may round a fractional
    # scale's size down: 1 pixel per axis is the same internal size, 2 are not (0.667 of 1920x1080 is
    # 1281x720 rounded).
    near = [_tsr_event(1280, 720)]
    report = build_report(make_set(tmp_path / "t4", scales=[0.667], log_extra=near), judge=False)
    assert report["set"]["ue_runs"][0]["upscale_sizes"] == [[1280, 720, 1920, 1080]]  # type: ignore[index]
    with pytest.raises(ComparisonError, match="UE rendered at 1279x720"):
        build_report(make_set(tmp_path / "t4_far", scales=[0.667], log_extra=[_tsr_event(1279, 720)]), judge=False)
    # At scale 1.0 a logged event must show the output size too; none is needed.
    with pytest.raises(ComparisonError, match="UE rendered at 960x540"):
        build_report(make_set(tmp_path / "native", log_extra=half), judge=False)


def test_the_godotnext_runs_record_one_scaling_3d_scale(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="differ in scaling_3d_scale"):
        build_report(make_set(tmp_path / "mixed", scales=[1.0, 0.5]), judge=False)
    with pytest.raises(ComparisonError, match="does not record its scaling_3d_scale"):
        build_report(make_set(tmp_path / "missing", scales=[None]), judge=False)


def test_skipped_godotnext_frames_are_dropped_not_zero(tmp_path: Path) -> None:
    report = build_report(make_set(tmp_path, runs=1, warmup=False, blank_rows=0), judge=False)
    assert _line(report, "total")["godotnext"]["missing_frames"] == [0]  # type: ignore[index]
    config = synthetic_config(lines=_lines(), frames=60)
    ue = config["ue"]
    assert isinstance(ue, dict)
    ue["path_frames"] = 1
    skipped = build_report(
        make_set(tmp_path / "skipped", runs=1, warmup=False, config=config, blank_rows=2), judge=False
    )
    total = _line(skipped, "total")
    assert total["godotnext"]["missing_frames"] == [2]  # type: ignore[index]
    assert total["godotnext"]["median"] == pytest.approx(1.10)  # type: ignore[index]
    with pytest.raises(ComparisonError, match="frames in the measured window have no value"):
        build_report(make_set(tmp_path / "too_many", runs=1, warmup=False, config=config, blank_rows=10), judge=False)


def test_a_prefix_line_sums_the_block_and_needs_its_columns(tmp_path: Path) -> None:
    config = synthetic_config(lines=[*_lines(), _block_line()])
    report = build_report(make_set(tmp_path, config=config), judge=False)
    block = _line(report, "block")
    # nextgi_ms + nextgi_trace_ms; the closing marker's column is excluded.
    assert block["godotnext"]["median"] == pytest.approx(0.5)  # type: ignore[index]
    run = report["set"]["godotnext_runs"][0]  # type: ignore[index]
    assert run["resolved_columns"]["block"] == ["nextgi_ms", "nextgi_trace_ms"]
    missing = copy.deepcopy(_block_line())
    missing["godotnext"] = {"prefix": "absent_"}
    with pytest.raises(ComparisonError, match="timing.csv has no column"):
        build_report(make_set(tmp_path / "missing", config=synthetic_config(lines=[missing])), judge=False)


def test_a_nextgi_set_needs_the_forced_nextgi_at_the_configured_tier_and_quality(tmp_path: Path) -> None:
    config = nextgi_config("SWRT", "0")
    config["lines"] = _lines()
    good = {"forced": True, "environments": 1, "status": {"tier": "SWRT", "quality": "HIGH", "reason": "FORCED"}}
    report = build_report(make_set(tmp_path / "good", config=config, nextgi=good), judge=False)
    facts = report["set"]
    assert isinstance(facts, dict)
    assert facts["godotnext_runs"][0]["nextgi_tier"] == "SWRT"
    with pytest.raises(ComparisonError, match="did not force NextGI"):
        build_report(make_set(tmp_path / "absent", config=config), judge=False)
    hwrt = {**good, "status": {"tier": "HWRT", "quality": "HIGH"}}
    with pytest.raises(ComparisonError, match="tier HWRT and quality HIGH"):
        build_report(make_set(tmp_path / "hwrt", config=config, nextgi=hwrt), judge=False)


def test_linux_runs_are_report_only(tmp_path: Path) -> None:
    options: dict[str, object] = {"config": _judged_config(), "kind": "timing", "editor_binary": False}
    report = build_report(make_set(tmp_path, host="Linux VM", **options), judge=True)  # type: ignore[arg-type]
    total = _line(report, "total")
    assert total["status"] == "report_only"
    assert total["reason"] == LINUX_REASON
    assert total["verdict"] is None


def test_a_short_ue_capture_is_refused_even_without_named_columns(tmp_path: Path) -> None:
    lines = [line for line in _lines(unnamed=True) if line["id"] in {"godotnext_only", "unnamed"}]
    with pytest.raises(ComparisonError, match="the UE capture has 7 frames; the window needs 8"):
        build_report(make_set(tmp_path, config=synthetic_config(lines=lines), ue_frames=5), judge=False)


def test_a_short_godotnext_window_is_refused(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="has 5 frames, not 6"):
        build_report(make_set(tmp_path, godotnext_frames=5), judge=False)


def test_the_command_writes_json_and_markdown(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = make_set(tmp_path / "set")
    out = tmp_path / "report"
    assert main(["--compare", str(root), "--out", str(out)]) == 0
    printed = json.loads(capsys.readouterr().out)
    assert printed["mode"] == "dry_run"
    saved = json.loads((out / "ue_compare_report.json").read_text(encoding="utf-8"))
    assert saved["lines"][0]["ratio"] == pytest.approx(1.20)
    markdown = (out / "ue_compare_report.md").read_text(encoding="utf-8")
    assert markdown == render_markdown(build_report(root, judge=False))
    assert "| Feature total | GX-1 | ue_bar | p95 | 1.200; " in markdown
    assert "not a measurement" in markdown
    assert "editor binary" in markdown
    assert "run mode editor_game" in markdown
    assert main(["--compare", str(root), "--judge"]) == 2
    assert "sign-off" in json.loads(capsys.readouterr().out)["error"]
    assert main(["--compare", str(tmp_path / "missing")]) == 2
