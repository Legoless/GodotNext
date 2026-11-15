"""GU+-7 fallback checker (#202, plan §21.5.3).

Each GU+-7 configuration that runs on hardware starts the testbed with frame generation requested
through an ``override.cfg``, on the probe scene ``harness/fg_status_probe.tscn``. The probe shows
the static-UI scene for 120 frames, then prints ``RenderingServer.get_frame_generation_status()``
as one ``GODOTNEXT_FG_STATUS {json}`` line, with the reason's enum constant name read from ClassDB.
A configuration passes when the status names the expected reason (the constant's name, or a name
ending in ``_<reason>``), every expected status field matches, and exactly one other log line names
the reason. A run with no status line, or with the method absent (before #203), fails. The status
keys read here, ``reason`` and (for MFG on Ada) ``multiplier``, the active multiplier, are this
checker's reading of #203 and #201; #203 fixes the Dictionary, and the table below follows it.

The mocked-capability cases (a pre-Ada NVIDIA GPU, ``FG_GPU_UNSUPPORTED``; a non-NVIDIA device,
``NOT_NVIDIA_RTX``) are doctests in #200 and #203 (§23.2), not runs; ``list`` names them.

``identical`` is GU+-7's last line: an all-PU+-off capture compared byte for byte, frame by frame,
with the same capture from a build without ``modules/dlss`` (both made by the harness capture mode).

``run`` launches the configurations here (an interactive session or the Linux VMs); on the RTX 4080
SUPER machine they run as runner jobs, and ``analyze`` reads their logs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

STATUS_PREFIX = "GODOTNEXT_FG_STATUS "
PROBE_SCENE = "res://harness/fg_status_probe.tscn"
PROVIDER = "rendering/frame_generation/provider"
MULTIPLIER = "rendering/frame_generation/multiplier"


class CheckInputError(ValueError):
    """A configuration, log or capture cannot be read."""


@dataclass(frozen=True)
class Configuration:
    name: str
    driver: str
    reason: str
    settings: dict[str, object]
    host: str
    expect: dict[str, object] = field(default_factory=dict)

    def override_body(self) -> str:
        """``override.cfg`` text that sets ``settings`` for one launch."""
        sections: dict[str, list[str]] = {}
        for setting, value in self.settings.items():
            section, _, key = setting.partition("/")
            sections.setdefault(section, []).append(f"{key}={json.dumps(value)}")
        return "".join(f"[{section}]\n" + "\n".join(lines) + "\n" for section, lines in sections.items())


CONFIGURATIONS: tuple[Configuration, ...] = (
    Configuration("vulkan_fg", "vulkan", "API_UNSUPPORTED", {PROVIDER: "DLSS-G"}, "RTX 4080 SUPER, RTX 5090"),
    Configuration("metal_fg", "metal", "API_UNSUPPORTED", {PROVIDER: "DLSS-G"}, "M5 Max"),
    # MFG 4x on Ada falls back to FG 2x and says why.
    Configuration(
        "mfg_on_ada",
        "d3d12",
        "MFG_REQUIRES_RTX50",
        {PROVIDER: "DLSS-G", MULTIPLIER: 4},
        "RTX 4080 SUPER",
        {"multiplier": 2},
    ),
)
MOCKED = {
    "FG_GPU_UNSUPPORTED": "a pre-Ada NVIDIA GPU with FG: mocked-capability doctest (#200, §23.2)",
    "NOT_NVIDIA_RTX": "a non-NVIDIA device with FG: mocked-capability doctest (#200, §23.2)",
}


def configuration(name: str) -> Configuration:
    for item in CONFIGURATIONS:
        if item.name == name:
            return item
    raise CheckInputError(f"unknown configuration {name!r}; known: {', '.join(c.name for c in CONFIGURATIONS)}")


def _names_reason(name: str, reason: str) -> bool:
    return name == reason or name.endswith("_" + reason)


def analyze(config: Configuration, log: str) -> dict[str, object]:
    """One configuration's verdict from its log."""
    lines = log.splitlines()
    status_lines = [line[len(STATUS_PREFIX) :] for line in lines if line.startswith(STATUS_PREFIX)]
    report: dict[str, object] = {"configuration": config.name, "driver": config.driver, "reason": config.reason}
    failures: list[str] = []
    reason_lines = [line for line in lines if not line.startswith(STATUS_PREFIX) and config.reason in line]
    report["reason_log_lines"] = len(reason_lines)
    if len(status_lines) != 1:
        failures.append(f"{len(status_lines)} {STATUS_PREFIX.strip()} lines, expected 1 (did the probe run?)")
    else:
        try:
            probe = json.loads(status_lines[0])
        except json.JSONDecodeError as error:
            raise CheckInputError(f"the status line is not JSON: {error}") from error
        report["probe"] = probe
        status = probe.get("status")
        if not probe.get("available"):
            failures.append("RenderingServer.get_frame_generation_status() is absent (#203)")
        elif not isinstance(status, dict):
            failures.append("get_frame_generation_status() did not return a Dictionary")
        else:
            name = str(probe.get("reason_name", ""))
            if not _names_reason(name, config.reason):
                failures.append(f"status reason is {name or status.get('reason')!r}, expected {config.reason}")
            for key, value in config.expect.items():
                if status.get(key) != value:
                    failures.append(f"status {key} is {status.get(key)!r}, expected {value!r}")
        if len(reason_lines) != 1:
            failures.append(f"{len(reason_lines)} log lines name {config.reason}, expected exactly 1")
    report["failures"] = failures
    report["pass"] = not failures
    return report


def run(
    godot: Path, testbed: Path, configs: list[Configuration], out_dir: Path, *, extra: list[str], timeout_s: float
) -> list[dict[str, object]]:
    """Launches each configuration on the probe scene with its override.cfg, then analyzes the log."""
    override = testbed / "override.cfg"
    results = []
    for config in configs:
        if override.exists():
            raise CheckInputError(f"{override} already exists; the checker never overwrites it")
        override.write_text(config.override_body(), encoding="utf-8")
        command = [
            str(godot),
            "--path",
            str(testbed),
            "--rendering-driver",
            config.driver,
            "--audio-driver",
            "Dummy",
            *extra,
            PROBE_SCENE,
        ]
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout_s, check=False)
        except subprocess.TimeoutExpired as error:
            raise CheckInputError(f"{config.name} did not finish within {timeout_s:g} s") from error
        finally:
            override.unlink()
        log = completed.stdout + completed.stderr
        folder = out_dir / config.name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "run.log").write_text(log, encoding="utf-8")
        result = analyze(config, log)
        result["exit_code"] = completed.returncode
        results.append(result)
    return results


def _frame_hashes(folder: Path) -> dict[str, str]:
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(folder.glob("frame*.png"))}


def identical(test: Path, reference: Path) -> dict[str, object]:
    """All-PU+-off frames against a build without modules/dlss: byte-identical, frame by frame."""
    test_hashes = _frame_hashes(test)
    ref_hashes = _frame_hashes(reference)
    if not test_hashes or not ref_hashes:
        raise CheckInputError("both capture folders need frame*.png")
    differing = sorted(name for name in test_hashes.keys() & ref_hashes.keys() if test_hashes[name] != ref_hashes[name])
    only = sorted(test_hashes.keys() ^ ref_hashes.keys())
    return {
        "metric": "GU+-7 all PU+ off, byte-identical",
        "frames": len(test_hashes),
        "reference_frames": len(ref_hashes),
        "differing": differing[:20],
        "differing_count": len(differing),
        "unpaired": only[:20],
        "pass": not differing and not only,
    }


def _emit(report: dict[str, object]) -> None:
    json.dump(report, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m godotnext_metrics.fallback_log_check")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="the configurations, their override.cfg and the mocked cases")
    launch = sub.add_parser("run", help="launch configurations here; extra Godot arguments after --")
    launch.add_argument("--godot", type=Path, required=True)
    launch.add_argument("--testbed", type=Path, required=True)
    launch.add_argument("--config", action="append", required=True, help="a configuration name; repeat")
    launch.add_argument("--out", type=Path, required=True)
    launch.add_argument("--timeout", type=float, default=300.0)
    launch.add_argument("extra", nargs=argparse.REMAINDER)
    reader = sub.add_parser("analyze", help="a configuration's log from a runner job")
    reader.add_argument("--config", required=True)
    reader.add_argument("--log", type=Path, required=True)
    same = sub.add_parser("identical", help="byte-for-byte compare of two harness captures")
    same.add_argument("--in", dest="in_dir", type=Path, required=True, help="all PU+ features off")
    same.add_argument("--ref", type=Path, required=True, help="the same capture from a build without modules/dlss")
    args = parser.parse_args(argv)
    try:
        if args.command == "list":
            _emit({
                "probe_scene": PROBE_SCENE,
                "configurations": [
                    {
                        "name": c.name,
                        "driver": c.driver,
                        "host": c.host,
                        "reason": c.reason,
                        "expect": c.expect,
                        "override_cfg": c.override_body(),
                    }
                    for c in CONFIGURATIONS
                ],
                "mocked": MOCKED,
            })
            return 0
        if args.command == "identical":
            _emit(identical(args.in_dir, args.ref))
            return 0
        if args.command == "analyze":
            results = [analyze(configuration(args.config), args.log.read_text(encoding="utf-8", errors="replace"))]
        else:
            extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
            configs = [configuration(name) for name in args.config]
            results = run(args.godot, args.testbed, configs, args.out, extra=extra, timeout_s=args.timeout)
    except (OSError, ValueError) as error:
        _emit({"metric": "GU+-7 fallbacks", "pass": False, "error": str(error)})
        return 2
    _emit({"metric": "GU+-7 fallbacks", "results": results, "pass": all(bool(r["pass"]) for r in results)})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
