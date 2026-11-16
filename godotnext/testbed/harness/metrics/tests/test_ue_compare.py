"""Comparison configs and the interleaved run plan (#229)."""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest
from godotnext_metrics.ue_bar import ComparisonError
from godotnext_metrics.ue_compare import (
    build_plan,
    config_label,
    godotnext_args,
    load_config,
    main,
    parse_config,
    ue_args,
)

_TESTBED = Path(__file__).resolve().parents[3]
_CONFIGS = sorted((_TESTBED / "ue_compare").glob("*.json"))
UE_EXE = "C:\\Epic\\UE_5.9\\Engine\\Binaries\\Win64\\UnrealEditor.exe"
UE_PROJECT = "X:\\Synthetic\\Synthetic.uproject"
UE_CSV_DIR = "C:\\Users\\synthetic\\AppData\\Local\\UnrealEngine\\5.9\\Saved\\Profiling\\CSV"


def synthetic_config(**overrides: object) -> dict[str, object]:
    config: dict[str, object] = {
        "schema": 1,
        "name": "synthetic_set",
        "title": "Synthetic set",
        "settings_record": "docs/godotnext/ue-bar/settings-map.md",
        "sign_off": None,
        "device": "NVIDIA GeForce RTX 4080 SUPER",
        "resolution": [1920, 1080],
        "runs": 5,
        "warmup_frames": 2,
        "frames": 6,
        "godotnext": {
            "driver": "vulkan",
            "scene": "res://scenes/synthetic/synthetic.tscn",
            "camera": "CaptureCam",
            "seed": 1,
            "override": "res://ue_compare/synthetic.override.cfg",
            "harness_args": ["--variant=static"],
            "timeout_minutes": 5,
        },
        "ue": {
            "run_mode": "editor_game",
            "build_version": None,
            "map": "/Game/Ports/Synthetic/Synthetic.umap",
            "rhi": "D3D12",
            "feature_level": "SM6",
            "warmup_frames": 2,
            "path_frames": 3,
            "fps": 60,
            "args": ["-dx12", "-sm6"],
            "exec_cmds": ["r.ScreenPercentage 100", "sg.GlobalIlluminationQuality 2"],
            "window_quiet_columns": ["Synthetic/Hitch"],
            "ok_exit_codes": [0],
            "timeout_minutes": 5,
        },
        "lines": [
            {
                "id": "total",
                "name": "Feature total",
                "gates": ["GX-1"],
                "status": "ue_bar",
                "statistic": "p95",
                "direction": "lower",
                "godotnext": {"columns": ["feature_a_ms", "feature_b_ms"]},
                "ue": {"group": "Feature", "columns": ["Synthetic/GPU/PassA", "Synthetic/GPU/PassB"]},
            }
        ],
    }
    config.update(overrides)
    return config


def _with(section: str, **values: object) -> dict[str, object]:
    config = synthetic_config()
    part = copy.deepcopy(config[section])
    assert isinstance(part, dict)
    part.update(values)
    config[section] = part
    return config


def _without(section: str, key: str) -> dict[str, object]:
    config = synthetic_config()
    part = copy.deepcopy(config[section])
    assert isinstance(part, dict)
    del part[key]
    config[section] = part
    return config


def _plan(config: dict[str, object], root: Path, **kwargs: object) -> dict[str, object]:
    options: dict[str, object] = {
        "config_label": "ue_compare/synthetic.json",
        "config_sha256": "0" * 64,
        "out_dir": root,
        "ue_exe": UE_EXE,
        "ue_project": UE_PROJECT,
        "ue_csv_dir": UE_CSV_DIR,
    }
    options.update(kwargs)
    return build_plan(parse_config(config), **options)  # type: ignore[arg-type]


@pytest.mark.parametrize("path", _CONFIGS, ids=lambda path: path.name)
def test_tracked_configs_load_and_their_overrides_exist(path: Path) -> None:
    config = load_config(path)
    assert config.name == path.stem
    if config.godotnext.override:
        assert (_TESTBED / config.godotnext.override.removeprefix("res://")).is_file()
    assert (_TESTBED.parents[1] / config.settings_record).is_file()
    # Build time: no verdict can be judged, so no tracked config carries a sign-off yet.
    assert config.sign_off is None
    # Plan v3.9.3: the installed editor in game mode.
    assert config.ue.run_mode == "editor_game"


def test_the_kitbash_config_forces_nextgi_and_the_software_tier() -> None:
    config = load_config(_TESTBED / "ue_compare" / "swrt_kitbash_high.json")
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == ("SWRT", "HIGH")
    assert "--nextgi=on" in godotnext_args(config)
    # The settings record's TIER_SOFTWARE row, forced and echoed so the report can check the run.
    cvars = config.ue.checked_cvars()
    assert cvars["r.Lumen.HardwareRayTracing"] == "0"
    assert cvars["r.Lumen.TraceMeshSDFs"] == "0"
    assert cvars["sg.GlobalIlluminationQuality"] == "2"
    override = (_TESTBED / "ue_compare" / "swrt_kitbash_high.override.cfg").read_text(encoding="utf-8")
    assert "global_illumination/nextgi/enabled=true" in override
    assert "global_illumination/nextgi/hardware_ray_tracing=1" in override
    assert "global_illumination/nextgi/quality=2" in override
    gs9 = next(line for line in config.lines if line.id == "gs9_swrt_total_high")
    assert gs9.godotnext is not None
    assert gs9.godotnext.prefix == "nextgi_"
    # The block's closing marker and line 1b (Q11) are not in the total.
    assert set(gs9.godotnext.exclude) == {"nextgi_2_ms", "nextgi_as_builds_ms"}


@pytest.mark.parametrize(
    ("name", "line_id", "gate", "lights"),
    [
        ("rt_shadow_yard", "gm3_rt_shadow_total_64", "GM-3", None),
        ("mega_lights_hall_1024", "gm2_megalights_total_1024", "GM-2", "1024"),
        ("mega_lights_hall_4096", "gm5_render_thread_4096", "GM-5", "4096"),
    ],
)
def test_the_lights_and_shadows_configs(name: str, line_id: str, gate: str, lights: str | None) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 233
    # The scenes as authored run no NextGI; the timing runs record every per-light pass.
    assert config.godotnext.nextgi is None
    assert "--nextgi=on" not in godotnext_args(config)
    assert config.godotnext.camera == "cam_main"
    override = (_TESTBED / "ue_compare" / "lights_shadows.override.cfg").read_text(encoding="utf-8")
    assert config.godotnext.override == "res://ue_compare/lights_shadows.override.cfg"
    assert "settings/profiler/max_timestamp_query_elements=65535" in override
    assert [arg for arg in godotnext_args(config) if arg.startswith("--lights")] == (
        [f"--lights={lights}"] if lights else []
    )
    line = next(line for line in config.lines if line.id == line_id)
    assert line.status == "ue_bar"
    assert line.gates == (gate,)
    # GodotNext's ray-traced shadows and MegaLights are not built yet: no GodotNext group.
    assert line.godotnext is None
    cvars = config.ue.checked_cvars()
    assert cvars["r.ScreenPercentage"] == "100"
    # Plan §21.3.3: these scenes run at the High preset. The settings record ties SMRT, the VSM pool,
    # volumetric fog and MegaLights to the Shadow scalability group, so the set pins it at High
    # and the report checks the echoed value.
    assert cvars["sg.ShadowQuality"] == "2"
    # Epic's MegaLights page: Stat GPU timings are distorted by async compute until
    # r.RDG.AsyncCompute 0. Only GM-2 is a GPU-pass line; GM-5 times the render thread.
    assert cvars.get("r.RDG.AsyncCompute") == ("0" if gate == "GM-2" else None)
    # GM-2's UE group is provisional until the lead maps M-1 and M-8 (§23.5); the report prints why.
    assert ("Provisional" in line.reason) == (gate == "GM-2")


@pytest.mark.parametrize(
    ("name", "line_id", "level", "history"),
    [
        ("upscaler_torture_tsr_high", "gt4_t1_tsr_high", "2", "100"),
        ("upscaler_torture_tsr_epic", "gt4_t2_tsr_epic", "3", "200"),
    ],
)
def test_the_upscaler_torture_configs(name: str, line_id: str, level: str, history: str) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 232
    # Plan §21.5.4: T1 and T2 are TSR at 2160p output and 0.5 scale on the RTX 4080 SUPER, p95 over
    # the 600-frame path; the UE path loops once per measured window.
    assert config.resolution == (3840, 2160)
    assert config.frames == config.ue.path_frames == 600
    line = next(line for line in config.lines if line.id == line_id)
    # A 3840x2160 window did not render at 3840x2160 on the machine's 4K desktop; fullscreen does
    # once the editor is DPI-aware there (the line's reason and the lead's decision).
    assert config.ue.window == "fullscreen"
    assert "2160p" in line.reason
    assert "-fullscreen" in ue_args(config) and "-windowed" not in ue_args(config)
    assert config.godotnext.nextgi is None
    quality = "high" if level == "2" else "epic"
    assert godotnext_args(config)[-3:] == ["--upscaler=tsr", f"--tsr-quality={quality}", "--scale=0.5"]
    cvars = config.ue.checked_cvars()
    # The TSR page: r.AntiAliasingMethod=4 is TSR, sg.AntiAliasingQuality 2 is High and 3 is Epic,
    # and Epic (only) uses a 200 % history. The history is not set: the run must show it.
    assert cvars["r.AntiAliasingMethod"] == "4"
    assert cvars["sg.AntiAliasingQuality"] == level
    assert cvars["r.ScreenPercentage"] == "50"
    assert cvars["sg.ResolutionQuality"] == "50"
    assert cvars["sg.ShadowQuality"] == "2"
    assert cvars["r.TSR.History.ScreenPercentage"] == history
    # The settings record's TSR async compute row: 2, UE's default, which the run must show (not set).
    assert cvars["r.TSR.AsyncCompute"] == "2"
    for name in ("r.TSR.History.ScreenPercentage", "r.TSR.AsyncCompute"):
        assert not any(command.startswith(name + " ") for command in config.ue.exec_cmds)
    # No capture records UE's internal resolution yet: the line says so, and a judged report refuses.
    assert "internal resolution" in line.reason and "not verified" in line.reason
    assert (line.status, line.gates, line.statistic) == ("ue_bar", ("GT-4",), "p95")
    assert line.godotnext is not None and line.godotnext.columns == ("tsr_ms",)
    assert line.ue is not None and line.ue.group == "TSR"


_LUMEN_TOTAL = ("GPU/LumenSceneLighting", "GPU/LumenScreenProbeGather", "GPU/LumenReflections")
_LUMEN_LITE = ("GPU/LumenSceneLighting", "GPU/LumenIrradianceFieldGather")


@pytest.mark.parametrize(
    ("name", "line_id", "gate", "tier", "quality", "frames", "path_frames", "variant"),
    [
        ("nextgi_perf_hwrt_high", "g3_1_nextgi_total_high", "G3-1", "HWRT", "HIGH", 600, 600, "p3"),
        ("nextgi_perf_hwrt_epic", "g3_1_nextgi_total_epic", "G3-1", "HWRT", "EPIC", 600, 600, "p3"),
        ("nextgi_perf_hwrt_medium", "gs7_lite_total_hwrt", "GS-7", "HWRT", "MEDIUM", 600, 600, "p3"),
        ("nextgi_perf_swrt_high", "gs2_swrt_total_high", "GS-2", "SWRT", "HIGH", 600, 600, "p3"),
        ("nextgi_perf_swrt_epic", "gs2_swrt_total_epic", "GS-2", "SWRT", "EPIC", 600, 600, "p3"),
        ("nextgi_perf_swrt_medium", "gs2_swrt_total_medium", "GS-2", "SWRT", "MEDIUM", 600, 600, "p3"),
        ("character_walk_hwrt_high", "g3_4_parent_nextgi_total_high", "G3-4", "HWRT", "HIGH", 240, 240, "walk"),
        ("glass_water_hwrt_high", "g3_5_parent_nextgi_total_high", "G3-5", "HWRT", "HIGH", 600, 1, "light_toggle"),
    ],
)
def test_the_nextgi_scene_configs(
    name: str, line_id: str, gate: str, tier: str, quality: str, frames: int, path_frames: int, variant: str
) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 230
    # NextGI Perf with its 4 skinned characters (G3-1, plan §2.1), the walk's 240 frames (G3-4's
    # parent), and Glass & Water's still camera (G3-5's parent).
    assert (config.frames, config.ue.path_frames) == (frames, path_frames)
    assert f"--variant={variant}" in godotnext_args(config)
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == (tier, quality)
    assert "--nextgi=on" in godotnext_args(config)
    level = {"MEDIUM": "1", "HIGH": "2", "EPIC": "3"}[quality]
    override = (_TESTBED / config.godotnext.override.removeprefix("res://")).read_text(encoding="utf-8")
    assert "global_illumination/nextgi/enabled=true" in override
    # Auto, the shipping default, takes the hardware tier where the device has it (the manifest's
    # NextGI status confirms it); Off forces the software tier.
    assert f"global_illumination/nextgi/hardware_ray_tracing={0 if tier == 'HWRT' else 1}" in override
    assert f"global_illumination/nextgi/quality={level}" in override
    assert ("global_illumination/nextgi/medium_gather=0" in override) == (quality == "MEDIUM")
    cvars = config.ue.checked_cvars()
    assert cvars["r.ScreenPercentage"] == "100"
    assert cvars["sg.GlobalIlluminationQuality"] == level
    assert cvars["sg.ReflectionQuality"] == level
    assert cvars["sg.ShadowQuality"] == "2"
    assert cvars["r.Lumen.AsyncCompute"] == "0"
    # The shared comparison project keeps Use Hardware Ray Tracing when available off, so each set
    # sets the tier at launch and the report checks the echo.
    assert cvars["r.Lumen.HardwareRayTracing"] == ("1" if tier == "HWRT" else "0")
    assert cvars.get("r.Lumen.TraceMeshSDFs") == (None if tier == "HWRT" else "0")
    # The Lumen performance guide's Medium: Irradiance Field Gather, r.Lumen.FinalGatherMethod 0.
    assert cvars.get("r.Lumen.FinalGatherMethod") == ("0" if quality == "MEDIUM" else None)
    line = next(line for line in config.lines if line.id == line_id)
    assert (line.status, line.gates, line.statistic) == ("ue_bar", (gate,), "p95")
    assert line.godotnext is not None and line.godotnext.prefix == "nextgi_"
    assert set(line.godotnext.exclude) == {"nextgi_2_ms", "nextgi_as_builds_ms"}
    # The UE columns come from the ports' own game-mode captures; both groups are provisional.
    assert line.ue is not None and line.ue.group == "Lumen total (provisional)"
    assert line.ue.columns == (_LUMEN_LITE if quality == "MEDIUM" else _LUMEN_TOTAL)
    assert "Provisional" in line.reason
    assert ("Lumen Lite" in line.reason) == (quality == "MEDIUM")
    # What the lead decides before the line is judged: NextGI Perf's far plane (the export's
    # camera_far approximation), the hardware tier set by the cvar rather than by the project
    # settings the settings record names, and which UE columns hold the translucency volume (G3-5).
    assert ("camera_far" in line.reason) == name.startswith("nextgi_perf_")
    assert ("departs from the settings record" in line.reason) == (tier == "HWRT")
    assert ("translucency volume" in line.reason) == (gate == "G3-5")


def test_the_ue_window_mode_comes_from_the_config() -> None:
    assert "-windowed" in ue_args(parse_config(synthetic_config()))
    fullscreen = ue_args(parse_config(_with("ue", window="fullscreen")))
    assert "-fullscreen" in fullscreen and "-windowed" not in fullscreen
    assert fullscreen[fullscreen.index("-fullscreen") + 1 : fullscreen.index("-fullscreen") + 3] == [
        "-ResX=1920",
        "-ResY=1080",
    ]


def test_expected_cvars_are_echoed_and_checked_but_not_set() -> None:
    config = parse_config(_with("ue", expect_cvars={"r.TSR.History.ScreenPercentage": "200"}))
    assert config.ue.checked_cvars()["r.TSR.History.ScreenPercentage"] == "200"
    commands = ue_args(config)[-1].removeprefix("-ExecCmds=").split(",")
    assert "r.TSR.History.ScreenPercentage" in commands
    assert not any(command.startswith("r.TSR.History.ScreenPercentage ") for command in commands)


def test_godotnext_timing_arguments() -> None:
    config = parse_config(synthetic_config())
    assert godotnext_args(config) == [
        "--path",
        "godotnext/testbed",
        "--resolution",
        "1920x1080",
        "--disable-vsync",
        "--gpu-profile",
        "--quit-after",
        "1058",
        "--",
        "--mode=timing",
        "--scene=res://scenes/synthetic/synthetic.tscn",
        "--camera=CaptureCam",
        "--out={out}",
        "--seed=1",
        "--warmup=2",
        "--frames=6",
        "--variant=static",
    ]
    forced = parse_config(_with("godotnext", nextgi={"tier": "SWRT", "quality": "HIGH"}))
    assert godotnext_args(forced)[-2:] == ["--nextgi=on", "--variant=static"]


def test_ue_launch_arguments_run_the_editor_in_game_mode() -> None:
    config = parse_config(synthetic_config())
    assert ue_args(config) == [
        "/Game/Ports/Synthetic/Synthetic.umap",
        "-game",
        "-dx12",
        "-sm6",
        "-windowed",
        "-ResX=1920",
        "-ResY=1080",
        "-novsync",
        "-unattended",
        "-nosplash",
        "-NoScreenMessages",
        "-benchmark",
        "-fps=60",
        "-deterministic",
        "-csvGpuStats",
        "-csvCaptureFrames=8",
        "-ExitAfterCsvProfiling",
        "-abslog={out}\\ue.log",
        # Each set console variable is named again on its own, so the log echoes the value the run used.
        "-ExecCmds=r.ScreenPercentage 100,sg.GlobalIlluminationQuality 2,r.ScreenPercentage,sg.GlobalIlluminationQuality",
    ]
    default_map = parse_config(_with("ue", map=None))
    assert ue_args(default_map)[0] == "-game"


def test_the_plan_interleaves_the_jobs(tmp_path: Path) -> None:
    plan = _plan(synthetic_config(), tmp_path, runs=2, kind="run")
    jobs = plan["jobs"]
    assert isinstance(jobs, list)
    assert [(job["role"], job["engine"], job["index"]) for job in jobs] == [
        ("warmup", "godotnext", 0),
        ("warmup", "ue", 0),
        ("timing", "godotnext", 1),
        ("timing", "ue", 1),
        ("timing", "godotnext", 2),
        ("timing", "ue", 2),
    ]
    assert [job["name"] for job in jobs] == ["00_g_w0", "01_u_w0", "02_g_t1", "03_u_t1", "04_g_t2", "05_u_t2"]
    files = plan["job_files"]
    assert isinstance(files, dict)
    godotnext = files[jobs[2]["job"]]
    assert godotnext["engine"] == "godotnext"
    assert godotnext["kind"] == "run"
    assert godotnext["driver"] == "vulkan"
    assert godotnext["override"] == "res://ue_compare/synthetic.override.cfg"
    assert godotnext["exclusive_gpu"] is True
    assert godotnext["priority"] == "BelowNormal"
    assert godotnext["out"] == str(tmp_path / "runs" / "02_g_t1")
    assert "godot" not in godotnext
    ue = files[jobs[3]["job"]]
    assert ue["engine"] == "ue"
    assert ue["exe"] == UE_EXE
    assert ue["project"] == UE_PROJECT
    assert ue["csv_dir"] == UE_CSV_DIR
    assert ue["priority"] == "BelowNormal"
    assert "-csvCaptureFrames=8" in ue["args"]
    assert ue["args"][:2] == ["/Game/Ports/Synthetic/Synthetic.umap", "-game"]
    assert ue["ok_exit_codes"] == [0]
    assert plan["runs"] == 2
    assert plan["warmup_launches"] is True
    assert plan["ue_run_mode"] == "editor_game"


def test_the_plan_runs_only_the_installed_editor_on_a_project(tmp_path: Path) -> None:
    with pytest.raises(ComparisonError, match="installed editor"):
        _plan(synthetic_config(), tmp_path, ue_exe="X:\\Builds\\Windows\\Synthetic.exe")
    with pytest.raises(ComparisonError, match="installed editor"):
        _plan(synthetic_config(), tmp_path, ue_exe="C:\\UE\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe")
    with pytest.raises(ComparisonError, match=".uproject"):
        _plan(synthetic_config(), tmp_path, ue_project="X:\\Synthetic\\Synthetic.umap")
    with pytest.raises(ComparisonError, match="ue-csv-dir"):
        _plan(synthetic_config(), tmp_path, ue_csv_dir="")


def test_the_plan_command_writes_job_files(tmp_path: Path) -> None:
    config_path = tmp_path / "synthetic.json"
    config_path.write_text(json.dumps(synthetic_config()), encoding="utf-8")
    out = tmp_path / "set"
    paths = ["--ue-exe", UE_EXE, "--ue-project", UE_PROJECT, "--ue-csv-dir", UE_CSV_DIR]
    code = main([
        "plan",
        "--config",
        str(config_path),
        "--out",
        str(out),
        *paths,
        "--godot",
        "godot.exe",
        "--no-warmup",
    ])
    assert code == 0
    plan = json.loads((out / "plan.json").read_text(encoding="utf-8"))
    assert "job_files" not in plan
    assert len(plan["jobs"]) == 10
    assert plan["job_kind"] == "timing"
    assert len(plan["config_sha256"]) == 64
    # A config outside the repository is named by its file name only: no machine path in the plan.
    assert plan["config_path"] == "synthetic.json"
    first = json.loads(Path(plan["jobs"][0]["job"]).read_text(encoding="utf-8"))
    assert first["godot"] == "godot.exe"
    assert main(["plan", "--config", str(config_path), "--out", "relative", *paths]) == 2
    assert main(["check", str(config_path)]) == 0


def test_a_tracked_config_is_named_relative_to_the_repository() -> None:
    path = _TESTBED / "ue_compare" / "swrt_kitbash_high.json"
    assert config_label(path) == "godotnext/testbed/ue_compare/swrt_kitbash_high.json"


@pytest.mark.parametrize(
    ("config", "match"),
    [
        (synthetic_config(frames=7), "whole number of camera paths"),
        (synthetic_config(schema=2), "schema"),
        (synthetic_config(resolution=[1920]), "resolution"),
        (synthetic_config(lines=[]), "at least one line"),
        (synthetic_config(name="Bad Name"), "lower-case"),
        (synthetic_config(priority="High"), "priority"),
        (_with("godotnext", driver="metal"), "vulkan or d3d12"),
        (_with("godotnext", harness_args=["--frames=10"]), "the plan sets it"),
        (_with("godotnext", harness_args=["--nextgi=on"]), "the plan sets it"),
        (_with("godotnext", override="res://x.txt"), "override"),
        (_with("godotnext", nextgi={"tier": "SOFTWARE", "quality": "HIGH"}), "godotnext.nextgi"),
        (_with("ue", config="Test"), "game mode"),
        (_with("ue", run_mode="packaged"), "run_mode"),
        (_without("ue", "rhi"), "rhi"),
        (_with("ue", args=["-ResX=1280"]), "the plan sets it"),
        (_with("ue", args=["-game"]), "the plan sets it"),
        (_with("ue", args=['-foo="bar"']), "no quotes"),
        (_with("ue", exec_cmds=["a 1,b 2"]), "one command per entry"),
        (_with("ue", map="Synthetic"), "/Game/"),
        (_with("ue", path_frames=0), "path_frames"),
        (_with("ue", ok_exit_codes=[777003]), "0 among them"),
        (_with("ue", expect_cvars={"r.ScreenPercentage": "100"}), "exec_cmds sets it"),
        (_with("ue", expect_cvars={"r.TSR.History.ScreenPercentage": "1,2"}), "expect_cvars"),
        (_with("ue", expect_cvars={"bad name": "1"}), "expect_cvars"),
        (_with("ue", window="borderless"), "ue.window"),
    ],
)
def test_bad_configs_are_refused(config: dict[str, object], match: str) -> None:
    with pytest.raises(ComparisonError, match=match):
        parse_config(config)


def test_duplicate_line_ids_are_refused() -> None:
    config = synthetic_config()
    lines = config["lines"]
    assert isinstance(lines, list)
    config["lines"] = lines + lines
    with pytest.raises(ComparisonError, match="unique"):
        parse_config(config)
