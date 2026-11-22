"""Comparison configs and the interleaved run plan (#229)."""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest
from godotnext_metrics.ue_bar import ComparisonError
from godotnext_metrics.ue_compare import (
    SCALABILITY_LEVEL_GROUPS,
    UE_ASYNC_OFF,
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
# Every set pins all scalability groups and turns UE's async compute off (lead decisions 2026-11-16).
PINNED = [
    "r.ScreenPercentage 100",
    "sg.ResolutionQuality 100",
    *(f"{group} 2" for group in SCALABILITY_LEVEL_GROUPS),
    *(f"{name} {value}" for name, value in UE_ASYNC_OFF.items()),
]


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
            "exec_cmds": list(PINNED),
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


def _replaced(name: str, value: str | None) -> list[str]:
    """PINNED with one console variable set to another value, or left out (None)."""
    commands = [command for command in PINNED if command.split()[0] != name]
    return commands if value is None else [*commands, f"{name} {value}"]


def nextgi_config(tier: str, tier_cvar: str | None) -> dict[str, object]:
    """A synthetic set that runs NextGI on the given tier, with UE's Lumen tier cvar (or none)."""
    config = _with("godotnext", nextgi={"tier": tier, "quality": "HIGH"})
    ue = copy.deepcopy(config["ue"])
    assert isinstance(ue, dict)
    ue["exec_cmds"] = _replaced("r.Lumen.HardwareRayTracing", tier_cvar)
    config["ue"] = ue
    return config


def own_levels_config(quality: str | None, tsr: str | None, levels: dict[str, str]) -> dict[str, object]:
    """A synthetic set at a NextGI quality (hardware tier) and/or a TSR quality, with these groups set."""
    config = nextgi_config("HWRT", "1") if quality else synthetic_config()
    godotnext = config["godotnext"]
    ue = config["ue"]
    assert isinstance(godotnext, dict) and isinstance(ue, dict)
    if quality:
        godotnext["nextgi"] = {"tier": "HWRT", "quality": quality}
    if tsr:
        godotnext["harness_args"] = ["--upscaler=tsr", f"--tsr-quality={tsr}", "--scale=0.5"]
    commands = [str(command) for command in ue["exec_cmds"]]
    for name, value in levels.items():
        commands = [*(command for command in commands if command.split()[0] != name), f"{name} {value}"]
    ue["exec_cmds"] = commands
    return config


def test_a_line_runs_only_its_own_groups_at_its_own_level() -> None:
    # Plan §23.4: every quality group at High (2) but a group a line runs at its own level: a NextGI
    # quality's GI and reflections, a TSR quality's anti-aliasing (the same names on the 0..4 scale).
    for quality, level in (("MEDIUM", "1"), ("HIGH", "2"), ("EPIC", "3")):
        levels = {"sg.GlobalIlluminationQuality": level, "sg.ReflectionQuality": level}
        cvars = parse_config(own_levels_config(quality, None, levels)).ue.checked_cvars()
        assert (cvars["sg.GlobalIlluminationQuality"], cvars["sg.ReflectionQuality"]) == (level, level)
        assert cvars["sg.ShadowQuality"] == "2"
    for tsr, level in (("low", "0"), ("high", "2"), ("epic", "3"), ("cinematic", "4")):
        cvars = parse_config(own_levels_config(None, tsr, {"sg.AntiAliasingQuality": level})).ue.checked_cvars()
        assert cvars["sg.AntiAliasingQuality"] == level


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


# The lead's column rule (2026-11-16): every pass the captures show doing the GodotNext group's work.
# Line 1a: the global distance field update on the software tier; the TLAS build and, where skinned
# characters are traced (the captures show the column there only), their BLAS updates on the hardware tier.
_SDF_1A = ("GPU/GlobalDistanceFieldUpdate",)
_TLAS_1A = ("GPU/RayTracingScene",)
_SKINNED_1A = (*_TLAS_1A, "GPU/RayTracingDynamicGeometry")
_LUMEN_UPDATE = ("GPU/LumenSceneUpdate", "GPU/UpdateLumenSceneBuffers")
_LUMEN_TOTAL = (*_LUMEN_UPDATE, "GPU/LumenSceneLighting", "GPU/LumenScreenProbeGather", "GPU/LumenReflections")
_LUMEN_LITE = (*_LUMEN_UPDATE, "GPU/LumenSceneLighting", "GPU/LumenIrradianceFieldGather")
_TRANSLUCENCY = ("GPU/TranslucentLighting", "GPU/Translucency")
# The Lumen columns of the sets that run Lumen; the sets without NextGI keep them quiet, and the GM sets
# need the scene lighting, the screen probe gather and the TLAS build active.
_LUMEN_QUIET = (*_LUMEN_UPDATE, "GPU/LumenSceneLighting", "GPU/LumenScreenProbeGather", "GPU/LumenReflections")
_GM_ACTIVE = ("GPU/LumenSceneLighting", "GPU/LumenScreenProbeGather", "GPU/RayTracingScene")
_GV_ACTIVE = ("GPU/ShadowDepths", "GPU/ShadowProjection")
_PSO_QUIET = ("PSO/PSOMisses", "PSO/PSOComputeMisses", "PSO/GraphicsPSOHitch", "PSO/ComputePSOHitch")
_MEGALIGHTS_TOTAL = (
    "GPU/MegaLights",
    "GPU/MegaLightsLightPowerDelta",
    "GPU/LightGrid",
    "GPU/VolumetricFog",
    *_TRANSLUCENCY,
)


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
    assert not gs9.godotnext.include
    # The lead's column rule (2026-11-16): line 1a's global distance field update, the Lumen scene update
    # and the Lumen total.
    assert gs9.ue is not None and (gs9.ue.group, gs9.ue.columns) == ("Lumen total", (*_SDF_1A, *_LUMEN_TOTAL))


def test_the_kitbash_detail_config_enables_mesh_sdf_tracing_on_both_sides() -> None:
    config = load_config(_TESTBED / "ue_compare" / "swrt_kitbash_detail.json")
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == ("SWRT", "HIGH")
    assert config.ue.checked_cvars()["r.Lumen.TraceMeshSDFs"] == "1"
    override = (_TESTBED / "ue_compare" / "swrt_kitbash_detail.override.cfg").read_text(encoding="utf-8")
    assert "global_illumination/nextgi/software_ray_tracing_mode=1" in override
    line = next(line for line in config.lines if line.id == "gs9_swrt_total_detail")
    assert line.status == "report_only" and line.gates == ("GS-9",)
    assert line.godotnext is not None and line.godotnext.prefix == "nextgi_"
    assert line.ue is not None and line.ue.columns is not None and "GPU/LumenSceneUpdate" in line.ue.columns


@pytest.mark.parametrize("path", _CONFIGS, ids=lambda path: path.name)
def test_every_set_pins_scalability_first_and_echoes_every_pinned_value(path: Path) -> None:
    # parse_config refuses a set that breaks the lead's decisions of 2026-11-16 (the refusals below);
    # this checks what it does not: the order of the pins, their echo, and where Lumen must (not) run.
    config = load_config(path)
    commands = [command.split() for command in config.ue.exec_cmds]
    # The groups go first, so a later scalability change cannot undo a cvar the set writes itself.
    names = [parts[0] for parts in commands]
    assert max(names.index(group) for group in SCALABILITY_LEVEL_GROUPS) < min(
        index for index, name in enumerate(names) if not name.startswith(("sg.", "r.ScreenPercentage"))
    )
    # The launch names every pinned group and async switch again on its own, so the log echoes the value.
    (exec_arg,) = [arg for arg in ue_args(config) if arg.startswith("-ExecCmds=")]
    echoed = exec_arg.removeprefix("-ExecCmds=").split(",")
    for name in (*SCALABILITY_LEVEL_GROUPS, "sg.ResolutionQuality", *UE_ASYNC_OFF):
        assert name in echoed, name
    # UE runs Lumen only where GodotNext runs NextGI: the other sets set no tier and keep the Lumen
    # columns quiet; the lights and shadows sets, which time no Lumen column, need it active.
    if config.godotnext.nextgi is None:
        assert not any(name.startswith("r.Lumen.HardwareRayTracing") for name in names)
        assert config.ue.window_quiet_columns == (*_PSO_QUIET, *_LUMEN_QUIET)
    else:
        assert config.ue.window_quiet_columns == _PSO_QUIET
    expected_active = (
        _GM_ACTIVE
        if config.raw["port_issue"] == 233
        else _GV_ACTIVE
        if config.name.startswith("dense_city_gv3")
        else ()
    )
    assert config.ue.window_active_columns == expected_active


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
    # The GM sets run GI at High on both sides (lead decision 2026-11-16): NextGI forced on at High
    # on the hardware tier (Auto takes it where the device has it), Lumen High on the hardware tier.
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == ("HWRT", "HIGH")
    assert "--nextgi=on" in godotnext_args(config)
    assert config.godotnext.camera == "cam_main"
    override = (_TESTBED / "ue_compare" / "lights_shadows.override.cfg").read_text(encoding="utf-8")
    assert config.godotnext.override == "res://ue_compare/lights_shadows.override.cfg"
    assert "settings/profiler/max_timestamp_query_elements=65535" in override
    for setting in ("enabled=true", "hardware_ray_tracing=0", "quality=2"):
        assert f"global_illumination/nextgi/{setting}" in override
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
    # Plan §21.3.3: these scenes run at the High preset: every scalability group at High, Lumen High.
    assert cvars["sg.ShadowQuality"] == "2"
    assert cvars["sg.GlobalIlluminationQuality"] == cvars["sg.ReflectionQuality"] == "2"
    assert cvars["r.Lumen.HardwareRayTracing"] == "1"
    # Epic's MegaLights page: Stat GPU timings are distorted by async compute until
    # r.RDG.AsyncCompute 0; every set turns it off.
    assert cvars["r.RDG.AsyncCompute"] == "0"
    # No GM line times a Lumen column, so the report needs Lumen (and the TLAS it traces) active instead.
    assert config.ue.window_active_columns == _GM_ACTIVE
    # GM-2's UE group: every pass doing the work of M-1 to M-6 plus M-8 (the lead's column rule).
    if gate == "GM-2":
        assert line.ue is not None and (line.ue.group, line.ue.columns) == ("MegaLights total", _MEGALIGHTS_TOTAL)
        assert "GPU/Fog" in line.reason
    assert "Provisional" not in line.reason


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
    # A 3840x2160 request rendered smaller on the machine's 125 % desktop until the runner made each UE
    # launch DPI-aware (lead decision 2026-11-16); fullscreen recorded 3840x2160 then.
    assert config.ue.window == "fullscreen"
    assert "DPI-aware" in line.reason and "recorded 3840x2160" in line.reason
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
    assert not any(command.startswith("r.TSR.History.ScreenPercentage ") for command in config.ue.exec_cmds)
    # Async compute off in timing sets (lead decision 2026-11-16), TSR's own switch included.
    assert cvars["r.TSR.AsyncCompute"] == "0"
    assert dict(config.ue.expect_cvars) == {"r.TSR.History.ScreenPercentage": history}
    # UE's internal resolution from the GPU profiler's TSR draw event: one ProfileGPU at start-up,
    # without the profiler window, after every cvar the set writes.
    assert config.ue.exec_cmds[-2:] == ("r.ProfileGPU.ShowUI 0", "ProfileGPU")
    assert "internal resolution" in line.reason and "ProfileGPU" in line.reason
    assert (line.status, line.gates, line.statistic) == ("ue_bar", ("GT-4",), "p95")
    assert line.godotnext is not None and line.godotnext.columns == ("tsr_ms",)
    assert line.ue is not None and line.ue.group == "TSR"


# Each NextGI total's UE columns, as the ports' own captures on the RTX 4080 SUPER machine show them
# (the walk's and NextGI Perf's High and Epic captures have GPU/RayTracingDynamicGeometry; its Medium
# capture, Glass & Water's and the GM halls' do not).
_NEXTGI_COLUMNS = {
    "nextgi_perf_hwrt_high": ("Lumen total", (*_SKINNED_1A, *_LUMEN_TOTAL)),
    "nextgi_perf_hwrt_epic": ("Lumen total", (*_SKINNED_1A, *_LUMEN_TOTAL)),
    "nextgi_perf_hwrt_medium": ("Lumen total", (*_TLAS_1A, *_LUMEN_LITE)),
    "nextgi_perf_swrt_high": ("Lumen total", (*_SDF_1A, *_LUMEN_TOTAL)),
    "nextgi_perf_swrt_epic": ("Lumen total", (*_SDF_1A, *_LUMEN_TOTAL)),
    "nextgi_perf_swrt_medium": ("Lumen total", (*_SDF_1A, *_LUMEN_LITE)),
    "character_walk_hwrt_high": ("Lumen total", (*_SKINNED_1A, *_LUMEN_TOTAL)),
    "glass_water_hwrt_high": ("Lumen total and translucency", (*_TLAS_1A, *_LUMEN_TOTAL, *_TRANSLUCENCY)),
}


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
    # sets the tier at launch (the lead accepted this on 2026-11-16) and the report checks the echo.
    assert cvars["r.Lumen.HardwareRayTracing"] == ("1" if tier == "HWRT" else "0")
    assert cvars.get("r.Lumen.TraceMeshSDFs") == (None if tier == "HWRT" else "0")
    # The Lumen performance guide's Medium: Irradiance Field Gather, r.Lumen.FinalGatherMethod 0.
    assert cvars.get("r.Lumen.FinalGatherMethod") == ("0" if quality == "MEDIUM" else None)
    line = next(line for line in config.lines if line.id == line_id)
    assert (line.status, line.gates, line.statistic) == ("ue_bar", (gate,), "p95")
    assert line.godotnext is not None and line.godotnext.prefix == "nextgi_"
    assert set(line.godotnext.exclude) == {"nextgi_2_ms", "nextgi_as_builds_ms"}
    # On the hardware tier line 1a's "Skinned refit" timestamp (no nextgi_ prefix) holds the TLAS build on
    # frames with skinned work, so the block takes its column too.
    assert line.godotnext.include == (("skinned_refit_ms",) if tier == "HWRT" else ())
    # The UE columns come from the ports' own game-mode captures, by the lead's column rule.
    assert line.ue is not None and (line.ue.group, line.ue.columns) == _NEXTGI_COLUMNS[name]
    assert "Provisional" not in line.reason and "GPU/LumenSceneUpdate" in line.reason
    # The reason cites the plan's rule and names the busy columns it leaves out, and does not claim the
    # settings record lists the columns yet.
    assert "plan §23.5 states it" in line.reason and "settings record and plan §23.5 carry it" not in line.reason
    assert "GPU/DistanceFields" in line.reason
    assert ("GPU/NaniteRayTracing" in line.reason) == (tier == "HWRT")
    assert ("skinned_refit_ms" in line.reason) == (tier == "HWRT")
    assert ("other UE features" in line.reason) == (tier == "SWRT")
    assert ("refit the UE side does not show" in line.reason) == (name == "nextgi_perf_hwrt_medium")
    assert ("Lumen Lite" in line.reason) == (quality == "MEDIUM")
    # NextGI Perf's camera now reaches the courtyard's far end (no camera_far approximation), and the
    # tier set per launch is the lead's accepted form.
    assert "camera_far" not in line.reason
    assert ("accepted on 2026-11-16" in line.reason) == (tier == "HWRT")
    assert ("translucency volume" in line.reason) == (gate == "G3-5")


_NANITE_TOTAL = ("GPU/NaniteStreaming", "GPU/NaniteVisBuffer", "GPU/NaniteBasePass", "GPU/NaniteReadback")


@pytest.mark.parametrize(
    ("name", "line_id", "gate", "path_frames"),
    [
        ("foliage_field", "gn7_nextgeo_total", "GN-7", 1),
        ("displacement_cliff", "gn6_parent_nextgeo_total", "GN-6", 1),
        ("character_crowd", "gn5_parent_nextgeo_total", "GN-5", 600),
    ],
)
def test_the_pn_scene_configs(name: str, line_id: str, gate: str, path_frames: int) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 234
    # The PN scenes are generated (generators/build_pn.gd) into the gitignored generated folder and
    # timed with their static cam_main; their Environments leave NextGI off.
    assert config.godotnext.scene == f"res://generated/pn_content/{name}.tscn"
    assert config.godotnext.camera == "cam_main"
    assert config.godotnext.nextgi is None and not config.godotnext.override
    assert "--nextgi=on" not in godotnext_args(config)
    # The crowd's Level Sequence holds the walkers' 600-row clips (a whole number of walk cycles, so
    # its loop wrap continues the cycle); the field and the cliff are still.
    assert (config.frames, config.ue.path_frames) == (600, path_frames)
    cvars = config.ue.checked_cvars()
    assert cvars["r.ScreenPercentage"] == "100"
    assert cvars["sg.ResolutionQuality"] == "100"
    assert cvars["sg.ShadowQuality"] == "2"
    # The maps turn Lumen off to match: the Lumen columns must stay quiet in the window.
    assert set(_LUMEN_QUIET) <= set(config.ue.window_quiet_columns)
    line = next(line for line in config.lines if line.id == line_id)
    assert (line.status, line.gates, line.statistic) == ("ue_bar", (gate,), "p95")
    # No NextGeo virtual geometry in the generated content and the PN features not built: no
    # GodotNext group, and the reason says why.
    assert line.godotnext is None
    assert "no NextGeo virtual geometry" in line.reason
    # The columns that appear only with Nanite on, from the port's own captures; provisional, and the
    # shadow raster inside GPU/ShadowDepths (and, for the crowd, the skin cache) left open.
    assert line.ue is not None
    assert (line.ue.group, line.ue.columns) == ("Nanite total (provisional)", _NANITE_TOTAL)
    assert "Provisional" in line.reason and "GPU/ShadowDepths" in line.reason
    assert ("GPU/GPUSkinCache" in line.reason) == (gate == "GN-5")
    # GN-6 compares against Nanite tessellation, which the UE map does not run yet: the lead accepted
    # it as not like-for-like until #163 (2026-11-16), and forcing Nanite on the port meshes.
    assert ("tessellation" in line.reason) == (gate == "GN-6")
    assert ("#163" in line.reason) == (gate == "GN-6")
    assert "waits for the lead" not in line.reason
    assert ("Nanite Foliage" in line.reason) == (gate == "GN-7")
    # The foliage row's Preserve Area is neither set nor read back on the UE side; it stays open.
    assert ("Preserve Area" in line.reason) == (gate == "GN-7")
    # GN-13 judges Dense City and Foliage Field together: this set records the Foliage Field half and
    # keeps the UE bar (§23.6); the cross-set sum and the GodotNext group are not defined yet.
    render = [line for line in config.lines if line.gates == ("GN-13",)]
    assert len(render) == (1 if name == "foliage_field" else 0)
    for line in render:
        assert line.status == "ue_bar" and line.godotnext is None and "Dense City" in line.reason
        assert line.ue is not None and line.ue.columns == ("RenderThreadTime",)


_DENSE_CITY_LINES = {
    # line id: gates, status, UE group, UE columns (from the port's own game-mode captures)
    "g5_1_whole_frame": (("G5-1",), "ue_bar", "Whole frame, GPU", ("GPUTime",)),
    # The lead's column rule (2026-11-16): line 1a's TLAS build, the Lumen scene update and the Lumen total
    # (the city traces no skinned meshes, and its capture has no GPU/RayTracingDynamicGeometry).
    "g5_1_nextgi_total": (
        ("G5-1",),
        "ue_bar",
        "Lumen total",
        (
            "GPU/RayTracingScene",
            "GPU/LumenSceneUpdate",
            "GPU/UpdateLumenSceneBuffers",
            "GPU/LumenSceneLighting",
            "GPU/LumenScreenProbeGather",
            "GPU/LumenReflections",
        ),
    ),
    "g5_1_nextgeo_cull": (("G5-1", "GN-2"), "ue_bar", "Nanite raster and cull (provisional)", ("GPU/NaniteVisBuffer",)),
    "g5_1_nextgeo_resolve": (("G5-1", "GN-11"), "ue_bar", "Nanite shading (provisional)", ("GPU/NaniteBasePass",)),
    "g5_1_vsm_total": (
        ("G5-1",),
        "ue_bar",
        "Virtual Shadow Maps (provisional)",
        ("GPU/ShadowDepths", "GPU/ShadowProjection"),
    ),
    "gn13_render_thread_dense_city": (("GN-13",), "ue_bar", "Render thread, CPU", ("RenderThreadTime",)),
}
# Epic's 5.9 console-variable reference: the eleven quality groups, pinned at High (2) in every set.
_DENSE_CITY_SCALABILITY_GROUPS = (
    "sg.ViewDistanceQuality",
    "sg.AntiAliasingQuality",
    "sg.ShadowQuality",
    "sg.GlobalIlluminationQuality",
    "sg.ReflectionQuality",
    "sg.PostProcessQuality",
    "sg.TextureQuality",
    "sg.EffectsQuality",
    "sg.FoliageQuality",
    "sg.ShadingQuality",
    "sg.LandscapeQuality",
)


@pytest.mark.parametrize(
    ("name", "variant"),
    [
        ("dense_city_hwrt_high", "dense_city"),
        ("dense_city_sweep_10m_hwrt_high", "dense_city_sweep_10m"),
        ("dense_city_sweep_100m_hwrt_high", "dense_city_sweep_100m"),
        ("dense_city_sweep_1b_hwrt_high", "dense_city_sweep_1b"),
    ],
)
def test_the_dense_city_configs(name: str, variant: str) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 231
    # The generated city (generators/dense_city/, into the gitignored generated folder) and its sweeps,
    # timed with the still street camera cam_main and NextGI at the hardware tier, High (G5-1).
    assert config.godotnext.scene == f"res://generated/dense_city/{variant}.tscn"
    assert config.godotnext.camera == "cam_main"
    assert (config.frames, config.ue.path_frames) == (600, 1)
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == ("HWRT", "HIGH")
    assert config.godotnext.override == "res://ue_compare/nextgi_hwrt_high.override.cfg"
    assert "--nextgi=on" in godotnext_args(config)
    cvars = config.ue.checked_cvars()
    assert cvars["r.ScreenPercentage"] == "100"
    assert cvars["sg.ResolutionQuality"] == "100"
    # The lead's decisions of 2026-11-16 (plan v3.9.7 §23.4): every scalability group pinned at High,
    # UE's async compute off, and the Lumen tier set per launch.
    for group in _DENSE_CITY_SCALABILITY_GROUPS:
        assert cvars[group] == "2", group
    for name in ("r.RDG.AsyncCompute", "r.Lumen.AsyncCompute", "r.TSR.AsyncCompute"):
        assert cvars[name] == "0", name
    assert cvars["r.Lumen.HardwareRayTracing"] == "1"
    # Not set, only checked from the echo: UE renders Virtual Shadow Maps (the project's method, G5-1's
    # shadow_method = virtual) and Nanite meshes trace their fallback (or proxy) in ray tracing.
    assert dict(config.ue.expect_cvars) == {"r.Shadow.Virtual.Enable": "1", "r.RayTracing.Nanite.Mode": "0"}
    lines = {line.id: line for line in config.lines}
    if variant == "dense_city":
        assert set(lines) == set(_DENSE_CITY_LINES)
        for line_id, (gates, status, group, columns) in _DENSE_CITY_LINES.items():
            line = lines[line_id]
            assert (line.gates, line.status, line.statistic) == (gates, status, "p95")
            assert line.ue is not None and (line.ue.group, line.ue.columns) == (group, columns)
        # GodotNext's virtual shadow maps are not built: the VSM line has no GodotNext group, the
        # whole frame says it is not like for like, and neither is the render thread's group defined.
        assert lines["g5_1_vsm_total"].godotnext is None
        assert lines["gn13_render_thread_dense_city"].godotnext is None
        assert "Foliage Field" in lines["gn13_render_thread_dense_city"].reason
        nextgi = lines["g5_1_nextgi_total"].godotnext
        assert nextgi is not None and nextgi.prefix == "nextgi_"
        assert set(nextgi.exclude) == {"nextgi_2_ms", "nextgi_as_builds_ms"}
        assert nextgi.include == ("skinned_refit_ms",)
        assert "plan §23.5 states it" in lines["g5_1_nextgi_total"].reason
        cull = lines["g5_1_nextgeo_cull"].godotnext
        assert cull is not None and "nextgeo_hiz_ms" in cull.columns
        resolve = lines["g5_1_nextgeo_resolve"].godotnext
        assert resolve is not None and resolve.columns == ("nextgeo_stage_a_ms", "nextgeo_stage_b_ms")
        for line_id in ("g5_1_whole_frame", "g5_1_vsm_total"):
            assert "shadow_method" in lines[line_id].reason and "not like for like" in lines[line_id].reason
        assert "GPU/HZB" in lines["g5_1_nextgeo_cull"].reason
        assert "Generate Ray Tracing Proxies" in lines["g5_1_nextgi_total"].reason
        assert "GPU/LumenSceneUpdate" in lines["g5_1_nextgi_total"].reason
        assert "Provisional" not in lines["g5_1_nextgi_total"].reason
        # The ground plane stays off Nanite as GodotNext draws it outside NextGeo, against §23.4's
        # v3.9.7 sentence until the lead decides: the two Nanite-column lines say so.
        for line_id in ("g5_1_nextgeo_cull", "g5_1_nextgeo_resolve"):
            assert "ground plane" in lines[line_id].reason and "v3.9.7" in lines[line_id].reason
    else:
        # G5-9: the sweeps' whole frame, report-only with the UE ratio, at the G5-1 set's settings.
        (line,) = config.lines
        assert (line.gates, line.status, line.statistic) == (("G5-9",), "report_only", "p95")
        assert line.godotnext is not None and line.godotnext.all_passes
        assert line.ue is not None and line.ue.columns == ("GPUTime",)
        assert "shadow_method" in line.reason
        # Only the 1B sweep places buildings past the sun's 1,600 m shadow range, which the UE import
        # does not map.
        assert ("directional_shadow_max_distance" in line.reason) == (variant == "dense_city_sweep_1b")
    # The hardware tier comes from the launch cvar, which the lead accepted on 2026-11-16, not from the
    # project settings the record names: the lines that time Lumen (the whole frame and the Lumen total)
    # say so.
    tier_lines = {
        "g5_1_whole_frame",
        "g5_1_nextgi_total",
        "g5_9_whole_frame_10m",
        "g5_9_whole_frame_100m",
        "g5_9_whole_frame_1b",
    }
    for line in config.lines:
        assert ("accepted on 2026-11-16" in line.reason) == (line.id in tier_lines)
        assert "departs from the settings record" not in line.reason


@pytest.mark.parametrize(
    ("name", "source", "camera", "frames", "map_name"),
    [
        ("dense_city_flight_hwrt_high", "dense_city_flight", "FlightCam", 3600, "DenseCityFlight"),
        ("dense_city_gv3_vsm", "dense_city_gv3", "cam_main", 600, "DenseCityGV3"),
        ("dense_city_gv3_storm_vsm", "dense_city_gv3", "cam_main", 600, "DenseCityGV3Storm"),
    ],
)
def test_dense_city_flight_and_gv3_configs(name: str, source: str, camera: str, frames: int, map_name: str) -> None:
    config = load_config(_TESTBED / "ue_compare" / f"{name}.json")
    assert config.raw["port_issue"] == 231
    assert config.godotnext.scene == f"res://generated/dense_city/{source}.tscn"
    assert config.godotnext.camera == camera
    assert (config.frames, config.ue.path_frames) == (frames, frames)
    assert config.ue.warmup_frames == (1800 if name == "dense_city_flight_hwrt_high" else 300)
    assert config.ue.map == f"/Game/Ports/{map_name}/{map_name}.umap"
    assert config.godotnext.nextgi is not None
    assert (config.godotnext.nextgi.tier, config.godotnext.nextgi.quality) == ("HWRT", "HIGH")
    assert "--nextgi=on" in godotnext_args(config)
    assert ("--storm=1" in godotnext_args(config)) == (name == "dense_city_gv3_storm_vsm")
    for group in _DENSE_CITY_SCALABILITY_GROUPS:
        assert config.ue.checked_cvars()[group] == "2", group
    if name == "dense_city_flight_hwrt_high":
        lines = {line.id: line for line in config.lines}
        assert set(lines) == {"gn2_dense_city_flight", "gn11_dense_city_flight"}
        assert lines["gn2_dense_city_flight"].gates == ("GN-2",)
        assert lines["gn11_dense_city_flight"].gates == ("GN-11",)
        assert lines["gn2_dense_city_flight"].ue is not None
        assert lines["gn2_dense_city_flight"].ue.columns == ("GPU/NaniteVisBuffer",)
        assert lines["gn11_dense_city_flight"].ue is not None
        assert lines["gn11_dense_city_flight"].ue.columns == ("GPU/NaniteBasePass",)
    else:
        (line,) = config.lines
        assert line.gates == ("GV-3",) and line.status == "ue_bar"
        assert line.godotnext is None and "V1–V5" in line.reason
        assert line.ue is not None and line.ue.columns == ("GPU/ShadowDepths", "GPU/ShadowProjection")
        assert config.ue.window_active_columns == ("GPU/ShadowDepths", "GPU/ShadowProjection")
        assert config.sign_off is None


def _readme_job(name: str) -> dict[str, object]:
    """A harness job from the harness README's JSON examples, by its name."""
    text = (_TESTBED / "harness" / "README.md").read_text(encoding="utf-8")
    for block in text.split("```json\n")[1:]:
        body = block.split("\n```", 1)[0]
        if f'"name": "{name}"' in body:
            job = json.loads(body)
            assert isinstance(job, dict)
            return job
    raise AssertionError(f"no README job {name}")


def _engine_options(args: list[str]) -> list[tuple[str, str | None]]:
    """The engine flags before "--" as (flag, value) pairs, in sorted order."""
    head = args[: args.index("--")]
    pairs: list[tuple[str, str | None]] = []
    index = 0
    while index < len(head):
        value = head[index + 1] if index + 1 < len(head) and not head[index + 1].startswith("--") else None
        pairs.append((head[index], value))
        index += 1 if value is None else 2
    return sorted(pairs, key=lambda pair: (pair[0], pair[1] or ""))


def test_the_gu_plus_config(tmp_path: Path) -> None:
    config = load_config(_TESTBED / "ue_compare" / "dense_city_gu_plus_2160p.json")
    assert config.raw["port_issue"] == 236
    # The GodotNext side is #202's GU+ job (the README's gu_plus_fg_off example) exactly: every GodotNext job
    # the timing plan writes has the job's device, driver and kind, the same engine flags before "--"
    # (--fullscreen and --audio-driver Dummy among them) and the same harness arguments after it. The runner
    # adds --rendering-driver and --gpu-index to both.
    job = _readme_job("gu_plus_fg_off")
    listed = job["args"]
    assert isinstance(listed, list)
    wanted = [str(arg) for arg in listed]
    plan = build_plan(
        config,
        config_label="godotnext/testbed/ue_compare/dense_city_gu_plus_2160p.json",
        config_sha256="0" * 64,
        out_dir=tmp_path,
        ue_exe=UE_EXE,
        ue_project=UE_PROJECT,
        ue_csv_dir=UE_CSV_DIR,
    )
    files = plan["job_files"]
    assert isinstance(files, dict)
    planned = [file for file in files.values() if file["engine"] == "godotnext"]
    assert len(planned) == config.runs + 1
    for file in planned:
        assert (file["device"], file["driver"], file["kind"]) == (job["device"], job["driver"], job["kind"])
        args = [str(arg) for arg in file["args"]]
        assert _engine_options(args) == _engine_options(wanted)
        assert args[args.index("--") :] == wanted[wanted.index("--") :]
    assert config.godotnext.nextgi is None and not config.godotnext.override
    # Godot's --fullscreen window on Windows is the testbed's borderless window (#270).
    project = (_TESTBED / "project.godot").read_text(encoding="utf-8").splitlines()
    assert "window/size/borderless.windows=true" in project
    # The UE map's sequence holds the still pose for 600 + 3,000 frames, and UE drops 600 warm-up rows.
    assert config.ue.map == "/Game/Ports/DenseCityGUPlus/DenseCityGUPlus.umap"
    assert (config.ue.warmup_frames, config.ue.path_frames) == (600, 1)
    launch = ue_args(config)
    assert "-csvCaptureFrames=3600" in launch
    assert config.ue.window == "fullscreen"
    assert dict(config.ue.expect_cvars) == {
        "r.Shadow.Virtual.Enable": "1",
        "r.FullScreenMode": "1",
        "r.AntiAliasingMethod": "0",
    }
    # No NextGI on the GodotNext side, so no Lumen tier on the UE side and the Lumen columns stay quiet.
    assert "r.Lumen.HardwareRayTracing" not in config.ue.checked_cvars()
    assert config.ue.window_quiet_columns == (*_PSO_QUIET, *_LUMEN_QUIET)
    # Whether UE meets #202's GPU-bound rule here is open, so nothing names the set or a line GPU-bound.
    assert "GPU-bound" not in config.title
    lines = {line.id: line for line in config.lines}
    assert set(lines) == {"gpu_pass_sum", "ue_render_thread"}
    for line in config.lines:
        assert line.status == "report_only" and line.gates == ()
        assert "GPU-bound" not in line.name
    total = lines["gpu_pass_sum"]
    assert total.godotnext is not None and total.godotnext.all_passes
    assert total.ue is not None and total.ue.columns == ("GPUTime",)
    for text in (
        "#98",
        "PresentMon",
        "Nsight Systems",
        "95%",
        "is open",
        "--fullscreen",
        "anti-alias",
        "shadow_method",
        "GPU/DistanceFieldShadows",
        "GPU/DistanceFields",
        "GPU/GlobalDistanceFieldUpdate",
        "not like for like",
    ):
        assert text in total.reason, text
    render = lines["ue_render_thread"]
    assert render.godotnext is None
    assert render.ue is not None and render.ue.columns == ("RenderThreadTime",)


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
    forced = parse_config(nextgi_config("SWRT", "0"))
    assert godotnext_args(forced)[-2:] == ["--nextgi=on", "--variant=static"]
    # engine_args go before "--", after the plan's own engine flags.
    flagged = godotnext_args(parse_config(_with("godotnext", engine_args=["--fullscreen", "--audio-driver", "Dummy"])))
    assert flagged[flagged.index("--gpu-profile") + 1 : flagged.index("--quit-after")] == [
        "--fullscreen",
        "--audio-driver",
        "Dummy",
    ]
    assert flagged.index("--quit-after") < flagged.index("--")


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
        "-ExecCmds=" + ",".join(PINNED + [command.split()[0] for command in PINNED]),
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
        (_with("godotnext", engine_args=["--quit-after", "10"]), "the plan or the runner sets it"),
        (_with("godotnext", engine_args=["--rendering-driver", "vulkan"]), "the plan or the runner sets it"),
        (_with("godotnext", engine_args=["--resolution=1280x720"]), "the plan or the runner sets it"),
        (_with("godotnext", engine_args=["--fullscreen", "--", "--mode=capture"]), "the plan or the runner sets it"),
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
        # The lead's decisions of 2026-11-16: every scalability group pinned, async compute off, and
        # UE's Lumen tier set per launch where GodotNext runs NextGI.
        (_with("ue", exec_cmds=_replaced("sg.LandscapeQuality", None)), "sg.LandscapeQuality"),
        (_with("ue", exec_cmds=_replaced("sg.ShadowQuality", "5")), "sg.ShadowQuality"),
        (_with("ue", exec_cmds=_replaced("sg.ResolutionQuality", None)), "sg.ResolutionQuality"),
        (_with("ue", exec_cmds=_replaced("sg.ResolutionQuality", "5")), "sg.ResolutionQuality"),
        (_with("ue", exec_cmds=_replaced("r.RDG.AsyncCompute", "1")), "async compute off: r.RDG.AsyncCompute 0"),
        (_with("ue", exec_cmds=_replaced("r.Lumen.AsyncCompute", None)), "r.Lumen.AsyncCompute 0"),
        (_with("ue", exec_cmds=_replaced("r.TSR.AsyncCompute", "2")), "r.TSR.AsyncCompute 0"),
        (_with("ue", exec_cmds=PINNED[:-1], expect_cvars={"r.TSR.AsyncCompute": "0"}), "r.TSR.AsyncCompute 0"),
        (nextgi_config("HWRT", None), "r.Lumen.HardwareRayTracing 1"),
        (nextgi_config("HWRT", "0"), "r.Lumen.HardwareRayTracing 1"),
        (nextgi_config("SWRT", "1"), "r.Lumen.HardwareRayTracing 0"),
        # High (2) everywhere but a line's own groups (plan §23.4), checked by parse_config itself.
        (own_levels_config(None, None, {"sg.ShadowQuality": "3"}), r"sg.ShadowQuality 2 \(High\), not '3'"),
        (own_levels_config(None, None, {"sg.ReflectionQuality": "3"}), r"sg.ReflectionQuality 2 \(High\)"),
        (own_levels_config("EPIC", None, {}), r"sg.GlobalIlluminationQuality 3 \(the line's own level\), not '2'"),
        (own_levels_config("HIGH", None, {"sg.ReflectionQuality": "3"}), r"sg.ReflectionQuality 2 \(the line's own"),
        (own_levels_config("MEDIUM", None, {"sg.GlobalIlluminationQuality": "1"}), "sg.ReflectionQuality 1"),
        (own_levels_config(None, "epic", {}), r"sg.AntiAliasingQuality 3 \(the line's own level\), not '2'"),
        (own_levels_config(None, "high", {"sg.AntiAliasingQuality": "3"}), "sg.AntiAliasingQuality 2"),
        (own_levels_config(None, "ultra", {}), "tsr-quality=ultra"),
        (_with("ue", exec_cmds=_replaced("sg.ResolutionQuality", "50")), r"at r.ScreenPercentage \(100\)"),
        # A column cannot be both quiet and active.
        (_with("ue", window_active_columns=["Synthetic/Hitch"]), "window_active_columns"),
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
