"""Comparison configs and the interleaved run plan for the UE 5.9 comparison (#229).

A comparison config (``godotnext/testbed/ue_compare/<set>.json``) describes one
interleaved set: the GodotNext timing job, the UE launch, the window and the
gate lines it serves. It holds no machine paths. ``plan`` turns it into the
runner job files for ``harness/runners/win_session1.ps1``, in the §23.4 order
(one unmeasured warm-up launch each, then GodotNext, UE, GodotNext, UE ...),
and ``harness/runners/ue_compare.ps1`` runs them on the RTX 4080 SUPER machine.

    python3 -m godotnext_metrics.ue_compare check <config.json>
    python3 -m godotnext_metrics.ue_compare plan --config <config.json> --out <dir>
        --ue-exe <UnrealEditor.exe> --ue-project <project.uproject> --ue-csv-dir <dir>

The UE side runs the comparison project in the installed editor in game mode
(``UnrealEditor.exe <project> -game``, plan v3.9.3), with the command line
only: the map's Level Sequence plays the camera path, ``-benchmark -fps=<fps>
-deterministic`` fix the time step, and the CSV profiler captures ``warmup +
frames`` rows from startup and exits (``-csvCaptureFrames``,
``-ExitAfterCsvProfiling``; Epic's command-line reference). The path loops, so
any ``frames = k x path_frames`` consecutive rows hold every path frame k
times, the same poses as GodotNext's frame-indexed window. Every console
variable ``exec_cmds`` sets is echoed again by its bare name, so the report can
read the value the run used from the log. ``expect_cvars`` names console
variables the run does not set but whose value it must have (for example one
that a scalability level sets): they are echoed and checked the same way.

The lead's decisions of 2026-11-16 hold for every set, and ``parse_config``
refuses a config that breaks them. ``exec_cmds`` pins every scalability group
of Epic's 5.9 console-variable reference (``sg.ResolutionQuality`` at the
set's ``r.ScreenPercentage``, and the eleven quality groups at High, 2, so the
editor's saved scalability never decides a level), except the groups a line
runs at its own level (plan §23.4): a NextGI set's GI and reflections at its
NextGI quality, a TSR set's anti-aliasing at its ``--tsr-quality``. It turns
UE's async compute off (``r.RDG.AsyncCompute 0``, "no async compute is used",
plus the Lumen and TSR switches), so timed passes do not overlap. A set that
runs NextGI sets UE's Lumen tier for the launch with
``r.Lumen.HardwareRayTracing`` (1 for the hardware tier, 0 for the software
tier). The runner makes every UE launch DPI-aware (``win_session1.ps1``).
``window_active_columns`` names UE columns that must hold values in the
measured window (a feature the set relies on that no line times, such as
Lumen in the lights and shadows sets): the report refuses a run without them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath

from godotnext_metrics.jsonutil import as_dict, as_int, as_list
from godotnext_metrics.ue_bar import ComparisonError, LineSpec, interleave_order, parse_line

SCHEMA = 1
JOB_KINDS = ("timing", "run")
GODOTNEXT_DRIVERS = ("vulkan", "d3d12")
# The README's timing example caps a 120 + 600 frame run at --quit-after 1770.
QUIT_AFTER_MARGIN = 1050
# Plan v3.9.3: the installed editor in game mode is the only UE run mode.
UE_RUN_MODES = ("editor_game",)
# Epic's command-line reference: -Windowed "Use windowed mode", -fullscreen "Use fullscreen mode".
# On the RTX 4080 SUPER machine (a 3840x2160 desktop at 125 % scaling) a 3840x2160 request recorded
# 2560x1440 windowed and 3072x1728 fullscreen while the editor in game mode was not DPI-aware there;
# the runner now makes each UE launch DPI-aware (#232, lead decision 2026-11-16). 2160p sets ask for
# fullscreen, and the report refuses a capture whose recorded resolution is not the set's.
UE_WINDOW_MODES = ("windowed", "fullscreen")
# Epic's 5.9 console-variable reference: the quality groups take "0:low, 1:med, 2:high, 3:epic,
# 4:cinematic, default: 3", and sg.ResolutionQuality takes "10..100, default: 100". Every set pins
# all of them (lead decision 2026-11-16: every group at High unless the line sets its own level).
SCALABILITY_LEVEL_GROUPS = (
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
SCALABILITY_RESOLUTION = "sg.ResolutionQuality"
SCALABILITY_HIGH = "2"
# Groups a line runs at its own level (plan §23.4): a NextGI quality's GI and reflections (the settings
# record's Medium, High and Epic rows: 1, 2 and 3), and TSR's anti-aliasing at GodotNext's --tsr-quality,
# the same names on the reference's 0..4 scale (the settings record's TSR rows: sg.AntiAliasingQuality 2
# for TSR High, 3 for TSR Epic).
NEXTGI_LEVEL_GROUPS = ("sg.GlobalIlluminationQuality", "sg.ReflectionQuality")
NEXTGI_LEVELS = {"MEDIUM": "1", "HIGH": "2", "EPIC": "3"}
TSR_LEVEL_GROUP = "sg.AntiAliasingQuality"
TSR_LEVELS = {"low": "0", "medium": "1", "high": "2", "epic": "3", "cinematic": "4"}
# UE's async compute is off in every timing set (lead decision 2026-11-16). The reference:
# r.RDG.AsyncCompute "0:disabled, no async compute is used"; r.Lumen.AsyncCompute "Whether Lumen should
# use async compute if supported."; r.TSR.AsyncCompute "0: Disabled".
UE_ASYNC_OFF = {"r.RDG.AsyncCompute": "0", "r.Lumen.AsyncCompute": "0", "r.TSR.AsyncCompute": "0"}
# The Lumen tier per launch for a set that runs NextGI (lead decision 2026-11-16).
UE_LUMEN_TIER = "r.Lumen.HardwareRayTracing"
UE_EDITOR_EXE = "UnrealEditor.exe"
NEXTGI_TIERS = ("SWRT", "HWRT")
NEXTGI_QUALITIES = ("MEDIUM", "HIGH", "EPIC")
PRIORITIES = ("BelowNormal", "Normal")
# The repository root, for config paths in the plan and the report (never a machine path).
REPO_ROOT = Path(__file__).resolve().parents[5]

_RES_PATH = re.compile(r"^res://[A-Za-z0-9_./-]+$")
_CVAR = re.compile(r"^[A-Za-z][A-Za-z0-9_.]*$")
_HARNESS_OWNED = ("--mode", "--scene", "--camera", "--out", "--seed", "--warmup", "--frames", "--nextgi")
_UE_OWNED = (
    "-game",
    "-windowed",
    "-fullscreen",
    "-resx",
    "-resy",
    "-novsync",
    "-unattended",
    "-nosplash",
    "-noscreenmessages",
    "-benchmark",
    "-fps",
    "-deterministic",
    "-csvgpustats",
    "-csvcaptureframes",
    "-exitaftercsvprofiling",
    "-csvmetadata",
    "-abslog",
    "-execcmds",
)


@dataclass(frozen=True)
class NextGIExpectation:
    """NextGI forced on in the scene's Environment, and the tier and quality the run must show."""

    tier: str
    quality: str

    def to_json(self) -> dict[str, object]:
        return {"tier": self.tier, "quality": self.quality}


@dataclass(frozen=True)
class GodotNextJob:
    driver: str
    scene: str
    camera: str
    seed: int
    override: str
    harness_args: tuple[str, ...]
    nextgi: NextGIExpectation | None
    timeout_minutes: float


@dataclass(frozen=True)
class UeJob:
    run_mode: str
    build_version: str | None
    map: str
    rhi: str
    feature_level: str
    warmup_frames: int
    path_frames: int
    fps: int
    exec_cmds: tuple[str, ...]
    args: tuple[str, ...]
    window_quiet_columns: tuple[str, ...]
    ok_exit_codes: tuple[int, ...]
    timeout_minutes: float
    expect_cvars: tuple[tuple[str, str], ...] = ()
    window: str = "windowed"
    window_active_columns: tuple[str, ...] = ()

    def checked_cvars(self) -> dict[str, str]:
        """Console variables ``exec_cmds`` sets and those ``expect_cvars`` names, with their
        values: the launch echoes each one and the report checks it."""
        found = _set_cvars(self.exec_cmds)
        found.update(dict(self.expect_cvars))
        return found


def _set_cvars(exec_cmds: tuple[str, ...]) -> dict[str, str]:
    """Console variables the ``exec_cmds`` entries set, with their values."""
    found: dict[str, str] = {}
    for command in exec_cmds:
        parts = command.split()
        if len(parts) == 2 and _CVAR.match(parts[0]):
            found[parts[0]] = parts[1]
    return found


@dataclass(frozen=True)
class ComparisonConfig:
    name: str
    title: str
    device: str
    resolution: tuple[int, int]
    runs: int
    warmup_frames: int
    frames: int
    priority: str
    settings_record: str
    sign_off: str | None
    godotnext: GodotNextJob
    ue: UeJob
    lines: tuple[LineSpec, ...]
    raw: dict[str, object]


def load_config(path: Path) -> ComparisonConfig:
    raw = json.loads(path.read_text(encoding="utf-8"))
    return parse_config(raw)


def parse_config(value: object) -> ComparisonConfig:
    data = as_dict(value)
    if data.get("schema") != SCHEMA:
        raise ComparisonError(f"schema must be {SCHEMA}")
    resolution = [as_int(item) for item in as_list(data.get("resolution"))]
    if len(resolution) != 2 or min(resolution) <= 0:
        raise ComparisonError("resolution is [width, height]")
    frames = _positive(data, "frames")
    warmup = _non_negative(data, "warmup_frames")
    runs = _positive(data, "runs")
    sign_off = data.get("sign_off")
    if sign_off is not None and not isinstance(sign_off, str):
        raise ComparisonError("sign_off is the lead's sign-off reference, or null")
    priority = str(data.get("priority", "BelowNormal"))
    if priority not in PRIORITIES:
        raise ComparisonError(f"priority must be one of {', '.join(PRIORITIES)}, not {priority!r}")
    godotnext = _godotnext_job(as_dict(data.get("godotnext")))
    ue = _ue_job(as_dict(data.get("ue")), warmup)
    if frames % ue.path_frames:
        raise ComparisonError(f"frames ({frames}) must be a whole number of camera paths ({ue.path_frames} frames)")
    _check_levels(godotnext, _set_cvars(ue.exec_cmds))
    if godotnext.nextgi is not None:
        tier = _set_cvars(ue.exec_cmds).get(UE_LUMEN_TIER)
        wanted = "1" if godotnext.nextgi.tier == "HWRT" else "0"
        if tier != wanted:
            raise ComparisonError(
                f"the set runs NextGI on the {godotnext.nextgi.tier} tier, so ue.exec_cmds sets {UE_LUMEN_TIER} {wanted}"
            )
    lines = tuple(parse_line(item) for item in as_list(data.get("lines")))
    if not lines:
        raise ComparisonError("a comparison serves at least one line")
    ids = [line.id for line in lines]
    if len(set(ids)) != len(ids):
        raise ComparisonError("line ids must be unique")
    return ComparisonConfig(
        name=_name(data, "name"),
        title=_string(data, "title"),
        device=_string(data, "device"),
        resolution=(resolution[0], resolution[1]),
        runs=runs,
        warmup_frames=warmup,
        frames=frames,
        priority=priority,
        settings_record=_string(data, "settings_record"),
        sign_off=sign_off,
        godotnext=godotnext,
        ue=ue,
        lines=lines,
        raw=data,
    )


def godotnext_args(config: ComparisonConfig) -> list[str]:
    """Godot arguments for a timing run (the runner adds --rendering-driver and --gpu-index)."""
    job = config.godotnext
    width, height = config.resolution
    quit_after = config.warmup_frames + config.frames + QUIT_AFTER_MARGIN
    args = [
        "--path",
        "godotnext/testbed",
        "--resolution",
        f"{width}x{height}",
        "--disable-vsync",
        "--gpu-profile",
        "--quit-after",
        str(quit_after),
        "--",
        "--mode=timing",
        f"--scene={job.scene}",
        f"--camera={job.camera}",
        "--out={out}",
        f"--seed={job.seed}",
        f"--warmup={config.warmup_frames}",
        f"--frames={config.frames}",
    ]
    if job.nextgi is not None:
        args.append("--nextgi=on")
    return args + list(job.harness_args)


def ue_args(config: ComparisonConfig, out_marker: str = "{out}") -> list[str]:
    """The editor's command line after the project (flags from Epic's command-line reference)."""
    ue = config.ue
    width, height = config.resolution
    args: list[str] = []
    if ue.map:
        args.append(ue.map)
    args.append("-game")
    args += list(ue.args)
    args += [
        f"-{ue.window}",
        f"-ResX={width}",
        f"-ResY={height}",
        "-novsync",
        "-unattended",
        "-nosplash",
        "-NoScreenMessages",
        "-benchmark",
        f"-fps={ue.fps}",
        "-deterministic",
        "-csvGpuStats",
        f"-csvCaptureFrames={ue.warmup_frames + config.frames}",
        "-ExitAfterCsvProfiling",
        f"-abslog={out_marker}\\ue.log",
    ]
    echoes = list(ue.checked_cvars())
    commands = list(ue.exec_cmds) + echoes
    if commands:
        args.append("-ExecCmds=" + ",".join(commands))
    return args


def build_plan(
    config: ComparisonConfig,
    *,
    config_label: str,
    config_sha256: str,
    out_dir: Path,
    ue_exe: str,
    ue_project: str,
    ue_csv_dir: str,
    godot: str | None = None,
    runs: int | None = None,
    kind: str = "timing",
    warmup: bool = True,
) -> dict[str, object]:
    """The job files and their order. Paths are the RTX 4080 SUPER machine's, passed in."""
    if kind not in JOB_KINDS:
        raise ComparisonError(f"kind must be timing or run, not {kind!r}")
    exe = PureWindowsPath(ue_exe)
    if exe.name.lower() != UE_EDITOR_EXE.lower() or [part.lower() for part in exe.parts[-4:-1]] != [
        "engine",
        "binaries",
        "win64",
    ]:
        raise ComparisonError(f"the UE side runs the installed editor: ...\\Engine\\Binaries\\Win64\\{UE_EDITOR_EXE}")
    if PureWindowsPath(ue_project).suffix.lower() != ".uproject":
        raise ComparisonError("--ue-project is the comparison project's .uproject file")
    if not ue_csv_dir:
        raise ComparisonError("--ue-csv-dir is the folder the editor writes CSV profiles to")
    count = config.runs if runs is None else runs
    jobs: list[dict[str, object]] = []
    files: dict[str, dict[str, object]] = {}
    for position, (role, engine, index) in enumerate(interleave_order(count, warmup)):
        name = f"{position:02d}_{'g' if engine == 'godotnext' else 'u'}_{'w' if role == 'warmup' else 't'}{index}"
        run_out = str(out_dir / "runs" / name)
        job_path = str(out_dir / "jobs" / f"{name}.json")
        if engine == "godotnext":
            job = _godotnext_job_file(config, name, kind, run_out, godot)
        else:
            job = _ue_job_file(config, name, kind, run_out, ue_exe, ue_project, ue_csv_dir)
        files[job_path] = job
        jobs.append({"name": name, "role": role, "engine": engine, "index": index, "job": job_path, "out": run_out})
    return {
        "schema": SCHEMA,
        "kind": "ue_compare_plan",
        "config_path": config_label,
        "config_sha256": config_sha256,
        "config": config.raw,
        "runs": count,
        "job_kind": kind,
        "warmup_launches": warmup,
        "ue_run_mode": config.ue.run_mode,
        "ue_exe": ue_exe,
        "ue_project": ue_project,
        "ue_csv_dir": ue_csv_dir,
        "jobs": jobs,
        "job_files": files,
    }


def write_plan(plan: dict[str, object], out_dir: Path) -> Path:
    files = as_dict(plan.pop("job_files"))
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "jobs").mkdir(exist_ok=True)
    for path, job in files.items():
        Path(path).write_text(json.dumps(job, indent=2) + "\n", encoding="utf-8")
    plan_path = out_dir / "plan.json"
    plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    return plan_path


def config_label(path: Path, root: Path = REPO_ROOT) -> str:
    """The config's path relative to the repository root, or its file name: no machine path."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.ue_compare")
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="validate comparison configs")
    check.add_argument("configs", nargs="+")
    plan = sub.add_parser("plan", help="write the interleaved job files")
    plan.add_argument("--config", required=True)
    plan.add_argument("--out", required=True, help="output directory (absolute)")
    plan.add_argument("--ue-exe", required=True, help="the installed editor's UnrealEditor.exe")
    plan.add_argument("--ue-project", required=True, help="the comparison project's .uproject")
    plan.add_argument("--ue-csv-dir", required=True, help="the folder the editor writes CSV profiles to")
    plan.add_argument("--godot", help="Godot binary for the runner (default: the runner's)")
    plan.add_argument("--runs", type=int, help="runs per engine (default: the config's)")
    plan.add_argument("--kind", choices=JOB_KINDS, default="timing", help="timing locks clocks; run is a dry run")
    plan.add_argument("--no-warmup", action="store_true", help="skip the unmeasured warm-up launches")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        code = exc.code
        return int(code) if isinstance(code, int) else 2
    try:
        if args.command == "check":
            for name in args.configs:
                config = load_config(Path(name))
                print(f"{name}: {config.name}, {len(config.lines)} lines, {config.runs} runs per engine")
            return 0
        config_path = Path(args.config)
        text = config_path.read_bytes()
        config = parse_config(json.loads(text.decode("utf-8")))
        out_dir = Path(args.out)
        if not out_dir.is_absolute():
            raise ComparisonError("--out must be an absolute directory")
        built = build_plan(
            config,
            config_label=config_label(config_path),
            config_sha256=hashlib.sha256(text).hexdigest(),
            out_dir=out_dir,
            ue_exe=args.ue_exe,
            ue_project=args.ue_project,
            ue_csv_dir=args.ue_csv_dir,
            godot=args.godot,
            runs=args.runs,
            kind=args.kind,
            warmup=not args.no_warmup,
        )
        print(write_plan(built, out_dir))
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"ue_compare: {exc}\n")
        return 2
    return 0


def _godotnext_job_file(
    config: ComparisonConfig, name: str, kind: str, out: str, godot: str | None
) -> dict[str, object]:
    job: dict[str, object] = {
        "name": name,
        "engine": "godotnext",
        "device": config.device,
        "driver": config.godotnext.driver,
        "kind": kind,
        "args": godotnext_args(config),
        "out": out,
        "timeout_minutes": config.godotnext.timeout_minutes,
        "exclusive_gpu": True,
        "priority": config.priority,
    }
    if config.godotnext.override:
        job["override"] = config.godotnext.override
    if godot:
        job["godot"] = godot
    return job


def _ue_job_file(
    config: ComparisonConfig, name: str, kind: str, out: str, exe: str, project: str, csv_dir: str
) -> dict[str, object]:
    return {
        "name": name,
        "engine": "ue",
        "device": config.device,
        "kind": kind,
        "exe": exe,
        "project": project,
        "csv_dir": csv_dir,
        "args": ue_args(config),
        "out": out,
        "timeout_minutes": config.ue.timeout_minutes,
        "exclusive_gpu": True,
        "priority": config.priority,
        "ok_exit_codes": list(config.ue.ok_exit_codes),
    }


def _godotnext_job(data: dict[str, object]) -> GodotNextJob:
    driver = _string(data, "driver")
    if driver not in GODOTNEXT_DRIVERS:
        raise ComparisonError(f"godotnext.driver must be vulkan or d3d12, not {driver!r}")
    scene = _string(data, "scene")
    if not _RES_PATH.match(scene):
        raise ComparisonError("godotnext.scene is a res:// path")
    override = str(data.get("override") or "")
    if override and not (_RES_PATH.match(override) and override.endswith(".cfg")):
        raise ComparisonError("godotnext.override is a res:// path to an override .cfg")
    harness_args = tuple(str(item) for item in as_list(data.get("harness_args", [])))
    for arg in harness_args:
        if arg.split("=", 1)[0] in _HARNESS_OWNED:
            raise ComparisonError(f"godotnext.harness_args sets {arg.split('=', 1)[0]}; the plan sets it")
    nextgi = None
    if data.get("nextgi") is not None:
        expected = as_dict(data.get("nextgi"))
        tier = str(expected.get("tier"))
        quality = str(expected.get("quality"))
        if tier not in NEXTGI_TIERS or quality not in NEXTGI_QUALITIES:
            raise ComparisonError(
                f"godotnext.nextgi is {{tier: {'/'.join(NEXTGI_TIERS)}, quality: {'/'.join(NEXTGI_QUALITIES)}}}"
            )
        nextgi = NextGIExpectation(tier=tier, quality=quality)
    return GodotNextJob(
        driver=driver,
        scene=scene,
        camera=_string(data, "camera"),
        seed=as_int(data.get("seed", 1)),
        override=override,
        harness_args=harness_args,
        nextgi=nextgi,
        timeout_minutes=_minutes(data),
    )


def _ue_job(data: dict[str, object], default_warmup: int) -> UeJob:
    if "config" in data:
        raise ComparisonError("ue.config is gone: the UE side runs the editor in game mode (ue.run_mode, v3.9.3)")
    run_mode = _string(data, "run_mode")
    if run_mode not in UE_RUN_MODES:
        raise ComparisonError(f"ue.run_mode must be one of {', '.join(UE_RUN_MODES)} (plan v3.9.3), not {run_mode!r}")
    build_version = data.get("build_version")
    if build_version is not None and not isinstance(build_version, str):
        raise ComparisonError("ue.build_version is the pinned build version string, or null")
    map_name = str(data.get("map") or "")
    if map_name and not map_name.startswith("/Game/"):
        raise ComparisonError("ue.map is a /Game/... map path")
    args = tuple(str(item) for item in as_list(data.get("args", [])))
    for arg in args:
        flag = arg.split("=", 1)[0].lower()
        if flag in _UE_OWNED:
            raise ComparisonError(f"ue.args sets {arg.split('=', 1)[0]}; the plan sets it")
        if '"' in arg:
            raise ComparisonError(f"ue.args {arg!r}: no quotes; the runner quotes values")
    exec_cmds = tuple(str(item) for item in as_list(data.get("exec_cmds", [])))
    for command in exec_cmds:
        if "," in command or '"' in command:
            raise ComparisonError(f"ue.exec_cmds {command!r}: one command per entry, no commas or quotes")
    quiet = tuple(str(item) for item in as_list(data.get("window_quiet_columns", [])))
    active = tuple(str(item) for item in as_list(data.get("window_active_columns", [])))
    for column in active:
        if not column or column in quiet:
            raise ComparisonError(f"ue.window_active_columns {column!r}: a column name not in window_quiet_columns")
    expected = as_dict(data.get("expect_cvars") or {})
    for name, value in expected.items():
        text = str(value)
        if not _CVAR.match(name) or not text or any(ch in text for ch in ' ,"'):
            raise ComparisonError(
                f"ue.expect_cvars {name!r}: a console variable name and one value without spaces, commas or quotes"
            )
    set_by_commands = _set_cvars(exec_cmds)
    _check_pinned(set_by_commands)
    for name in expected:
        if name in set_by_commands:
            raise ComparisonError(f"ue.expect_cvars {name!r}: exec_cmds sets it; expect only what the run does not set")
    ok_exit_codes = tuple(as_int(code) for code in as_list(data.get("ok_exit_codes", [0])))
    if 0 not in ok_exit_codes:
        raise ComparisonError("ue.ok_exit_codes lists the exit codes of a finished run, 0 among them")
    window = str(data.get("window", "windowed"))
    if window not in UE_WINDOW_MODES:
        raise ComparisonError(f"ue.window must be one of {', '.join(UE_WINDOW_MODES)}, not {window!r}")
    warmup = data.get("warmup_frames", default_warmup)
    return UeJob(
        run_mode=run_mode,
        build_version=build_version,
        map=map_name,
        rhi=_string(data, "rhi"),
        feature_level=_string(data, "feature_level"),
        warmup_frames=_non_negative({"warmup_frames": warmup}, "warmup_frames"),
        path_frames=_positive(data, "path_frames"),
        fps=_positive(data, "fps"),
        exec_cmds=exec_cmds,
        args=args,
        window_quiet_columns=quiet,
        ok_exit_codes=ok_exit_codes,
        timeout_minutes=_minutes(data),
        expect_cvars=tuple((str(name), str(value)) for name, value in expected.items()),
        window=window,
        window_active_columns=active,
    )


def _check_pinned(cvars: dict[str, str]) -> None:
    """Every scalability group pinned and UE's async compute off (lead decisions 2026-11-16)."""
    for name in SCALABILITY_LEVEL_GROUPS:
        if cvars.get(name) not in ("0", "1", "2", "3", "4"):
            raise ComparisonError(f"ue.exec_cmds pins every scalability group: {name} 0..4 (High is 2)")
    resolution = cvars.get(SCALABILITY_RESOLUTION, "")
    if not resolution.isdigit() or not 10 <= int(resolution) <= 100:
        raise ComparisonError(f"ue.exec_cmds pins {SCALABILITY_RESOLUTION} (10..100)")
    percentage = cvars.get("r.ScreenPercentage")
    if percentage is not None and percentage != resolution:
        raise ComparisonError(f"ue.exec_cmds pins {SCALABILITY_RESOLUTION} at r.ScreenPercentage ({percentage})")
    for name, value in UE_ASYNC_OFF.items():
        if cvars.get(name) != value:
            raise ComparisonError(f"ue.exec_cmds turns UE's async compute off: {name} {value}")


def _check_levels(godotnext: GodotNextJob, cvars: dict[str, str]) -> None:
    """Every quality group at High (2) but the groups a line runs at its own level (plan §23.4)."""
    own: dict[str, str] = {}
    if godotnext.nextgi is not None:
        own.update(dict.fromkeys(NEXTGI_LEVEL_GROUPS, NEXTGI_LEVELS[godotnext.nextgi.quality]))
    for arg in godotnext.harness_args:
        if arg.startswith("--tsr-quality="):
            quality = arg.split("=", 1)[1]
            if quality not in TSR_LEVELS:
                raise ComparisonError(f"godotnext.harness_args {arg}: one of {', '.join(TSR_LEVELS)}")
            own[TSR_LEVEL_GROUP] = TSR_LEVELS[quality]
    for name in SCALABILITY_LEVEL_GROUPS:
        wanted = own.get(name, SCALABILITY_HIGH)
        if cvars.get(name) != wanted:
            why = "the line's own level" if name in own else "High"
            raise ComparisonError(f"ue.exec_cmds sets {name} {wanted} ({why}), not {cvars.get(name)!r}")


def _minutes(data: dict[str, object]) -> float:
    value = data.get("timeout_minutes", 30)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        raise ComparisonError("timeout_minutes is a positive number")
    return float(value)


def _string(data: dict[str, object], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise ComparisonError(f"{key} must be a non-empty string")
    return value


def _name(data: dict[str, object], key: str) -> str:
    value = _string(data, key)
    if not re.match(r"^[a-z0-9_]{1,48}$", value):
        raise ComparisonError(f"{key} is 1-48 lower-case letters, digits and _")
    return value


def _positive(data: dict[str, object], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ComparisonError(f"{key} must be a positive integer")
    return value


def _non_negative(data: dict[str, object], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ComparisonError(f"{key} must be a non-negative integer")
    return value


if __name__ == "__main__":
    sys.exit(main())
