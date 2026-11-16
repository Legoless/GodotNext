#!/usr/bin/env python3
"""Vendor-seam check (#210, plan §21.5 U2, GU+-1) for the test-only C GDExtension in this folder.

Build the extension against the engine it runs on first:

    <godot> --headless --dump-gdextension-interface
    mkdir -p tests/vendor_seam_test/src/deps
    mv gdextension_interface.h tests/vendor_seam_test/src/deps/
    scons --directory=./tests/vendor_seam_test

Then, from the repository root:

    GODOT4_BIN=<godot> ./tests/vendor_seam_test/run_vendor_seam_test.py
        The headless check (no GPU; the CPU job): registration, availability and unloading.
    GODOT4_BIN=<godot> ./tests/vendor_seam_test/run_vendor_seam_test.py --rendered --driver vulkan|d3d12 [--gbv]
        Instead of the headless check: the rendered check with GPU validation (add --gpu-index N to
        pick the GPU), then the same frame drawn with the GDScript samples of #192 and #203, which
        must be identical. --gbv (D3D12) turns on GPU-based validation for these runs only.
    ./tests/vendor_seam_test/run_vendor_seam_test.py --check-log <log> --mode headless|rendered|reference [--exit-code N] [--driver vulkan|d3d12] [--gbv]
        Checks the log of a run another runner made. The exit code comes from --exit-code or from
        the done.json next to the log. The driver comes from the log; --driver must match it.

Each run first imports the project (a headless editor run), which writes the
.godot/extension_list.cfg a game needs to load the extension. Only the checked runs' logs count:
Godot exits with 0, each fallback logs exactly one line, there is no other engine error, and there
are no validation messages or leaks. A clean log counts only when validation ran: on Vulkan the
Khronos validation layer is in the chain, and on D3D12 the debug layer's messages go to the log
(and GPU-based validation is on exactly when --gbv is given).
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

PROJECT_PATH = pathlib.Path(__file__).parent.resolve().joinpath("godot")
EXTENSION_PATH = "res://vendor_seam_test.gdextension"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

UPSCALER_FALLBACK = "Custom 3D temporal upscaler is unavailable"
UPSCALER_CLAMP = "Custom temporal upscaler 'SeamCopy' scale must be between"
FG_REASONS = [
    "API_UNSUPPORTED",
    "FG_GPU_UNSUPPORTED",
    "MFG_REQUIRES_RTX50",
    "NOT_NVIDIA_RTX",
    "NOT_MAIN_WINDOW",
    "PROVIDER_ALREADY_ACTIVE",
    "PROVIDER_UNAVAILABLE",
]
# Vulkan (debug utils) and D3D12 debug-layer errors and warnings print as ERROR:/WARNING: lines that
# name a "Message Id Number" (D3D12 info messages are plain lines and do not count). Through Vulkan's
# debug report, the WARNING: line names only the object and the layer's text follows on the next
# line, so "Validation Warning" and "Validation Performance Warning" count wherever they appear.
DEBUG_LAYER = re.compile(r"^(ERROR|WARNING): .*(Message Id Number|VALIDATION)")
ALWAYS_BAD = re.compile(r"Validation Error|(?i:Validation (Performance )?Warning)|Error detected in|leaked")
# Godot drops --gpu-validation silently when the Khronos layer is not installed, so a Vulkan log
# must show the layer: the loader's line (Linux and Windows) or the layer's own status line.
VULKAN_LAYER_ACTIVE = ('Inserted device layer "VK_LAYER_KHRONOS_validation"', "Khronos Validation Layer Active")
D3D12_LOGGED = "D3D12: Debug layer messages go to the log."
D3D12_GBV = "D3D12: GPU-based validation is on."
SCENE_DRIVER = re.compile(r"^VENDOR_SEAM_SCENE driver=(\S+)")

# The first import's known editor crash: the editor loads the extension during its first scan and
# crashes at exit when it quits that soon after (SIGABRT or SIGSEGV on Linux, an access violation
# on Windows). Its root cause is not known (docs/godotnext/vendor-extensions.md, "Known gaps").
IMPORT_CRASH_CODES = (-6, -11, 0xC0000005)


def log_lines(log: str) -> list[str]:
    return [ANSI.sub("", line).rstrip("\r") for line in log.splitlines()]


def engine_lines(log: str) -> list[str]:
    """The log without the scene's and the library's own lines, which never name a reason."""
    return [line for line in log_lines(log) if not line.startswith(("VENDOR_SEAM_", "GODOTNEXT_RESULT"))]


def check_log(log: str, mode: str, driver: str | None, exit_code: int | None, gbv: bool) -> list[str]:
    """Problems in the log of a run in mode "headless", "rendered" or "reference".

    driver is the driver the caller expects (None: take it from the log), exit_code Godot's exit
    code (None: unknown, which is a problem), gbv whether D3D12 GPU-based validation must be on.
    """
    problems: list[str] = []
    lines = log_lines(log)

    if exit_code is None:
        problems.append("Godot's exit code is unknown (pass --exit-code, or keep done.json next to the log)")
    elif exit_code != 0:
        problems.append(f"Godot exited with {exit_code} in the {mode} run")

    results = [line for line in lines if line.startswith("GODOTNEXT_RESULT ")]
    if len(results) != 1:
        problems.append(f"expected 1 GODOTNEXT_RESULT line, saw {len(results)}")
    else:
        result = json.loads(results[0].removeprefix("GODOTNEXT_RESULT "))
        if result.get("mode") != mode or result.get("pass") is not True:
            problems.append(f"the {mode} check did not pass: {results[0]}")

    for marker in (
        "VENDOR_SEAM_STARTUP registered=2 ",
        "VENDOR_SEAM_SHUTDOWN live_instances=0",
        "VENDOR_SEAM_DEINIT live_instances=0",
    ):
        count = sum(1 for line in lines if line.startswith(marker))
        if count != 1:
            problems.append(f"expected 1 line starting with '{marker.strip()}', saw {count}")

    engine = engine_lines(log)
    # One line per fallback the run reaches, none for the others.
    expected = {reason: 0 for reason in FG_REASONS}
    if mode == "rendered":
        # A second provider while SeamPassthrough holds the process, then the unload.
        expected["PROVIDER_ALREADY_ACTIVE"] = 1
        expected["PROVIDER_UNAVAILABLE"] = 1
    elif mode == "headless":
        # Nothing renders headless, so frame generation reports API_UNSUPPORTED.
        expected["API_UNSUPPORTED"] = 1
    for reason, count in expected.items():
        seen = sum(1 for line in engine if f"({reason})" in line)
        if seen != count:
            problems.append(f"expected {count} log line(s) naming {reason}, saw {seen}")
    # The rendered run clamps the scale once, then unloads the library.
    upscaler_lines = 1 if mode == "rendered" else 0
    for text, count in ((UPSCALER_FALLBACK, upscaler_lines), (UPSCALER_CLAMP, upscaler_lines)):
        seen = sum(1 for line in engine if text in line)
        if seen != count:
            problems.append(f"expected {count} log line(s) with '{text}', saw {seen}")

    # "ERROR:", and "SCRIPT ERROR:" for a script error, which would abort the scene's checks.
    errors = [line for line in engine if "ERROR:" in line]
    allowed = [line for line in errors if "(PROVIDER_ALREADY_ACTIVE)" in line]
    if len(errors) != len(allowed) or len(allowed) != expected["PROVIDER_ALREADY_ACTIVE"]:
        problems.append(f"unexpected engine errors: {errors}")

    validation = [line for line in engine if DEBUG_LAYER.search(line) or ALWAYS_BAD.search(line)]
    if validation:
        problems.append(f"validation messages or leaks: {validation[:10]}")

    if mode != "headless":
        drivers = [match.group(1) for line in lines if (match := SCENE_DRIVER.match(line))]
        if len(drivers) != 1:
            problems.append(f"expected 1 VENDOR_SEAM_SCENE line naming the driver, saw {len(drivers)}")
            return problems
        if driver is not None and driver != drivers[0]:
            problems.append(f"the run used the {drivers[0]} driver, not {driver}")
        driver = drivers[0]
        if driver == "vulkan" and not any(any(text in line for text in VULKAN_LAYER_ACTIVE) for line in engine):
            problems.append("the Khronos validation layer was not in the Vulkan chain (no line shows it)")
        if driver == "d3d12":
            if not any(line.startswith(D3D12_LOGGED) for line in engine):
                problems.append(f"the D3D12 debug layer's messages did not go to the log (no '{D3D12_LOGGED}' line)")
            gbv_on = any(line.startswith(D3D12_GBV) for line in engine)
            if gbv and not gbv_on:
                problems.append(f"GPU-based validation was asked for but is off (no '{D3D12_GBV}' line)")
            elif gbv_on and not gbv:
                problems.append(
                    f"GPU-based validation is on without --gbv ('{D3D12_GBV}'); is an override.cfg left in the project?"
                )
        elif gbv:
            problems.append(f"--gbv applies to D3D12 only, and this run used {driver}")
    return problems


def run(command: list[str], timeout: int) -> tuple[int, str]:
    print("+ " + " ".join(command), flush=True)
    try:
        proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        output = error.stdout.decode("utf-8", "replace") if error.stdout else ""
        return -1, output + f"\nrun_vendor_seam_test.py: timed out after {timeout} s\n"
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def is_known_import_crash(status: int, log: str) -> bool:
    """The first import's known editor crash, and nothing else: the import loaded the extension,
    logged no error and then crashed."""
    lines = log_lines(log)
    loaded = any(line.startswith("VENDOR_SEAM_STARTUP registered=2 ") for line in lines)
    errors = [line for line in lines if "ERROR:" in line]
    return status in IMPORT_CRASH_CODES and loaded and not errors


def import_project(godot: str) -> bool:
    """Imports the project, so that a game run loads the extension at startup."""
    extension_list = PROJECT_PATH.joinpath(".godot", "extension_list.cfg")
    command = [godot, "--headless", "--path", str(PROJECT_PATH), "--import"]
    status, log = run(command, 300)
    listed = extension_list.is_file() and EXTENSION_PATH in extension_list.read_text()
    if status != 0 and listed and is_known_import_crash(status, log):
        # A second import loads the extension at startup instead of during the scan.
        print(log)
        print(
            f"run_vendor_seam_test.py: known issue: the first import exited with {status} after loading the extension during its scan (the editor crash in docs/godotnext/vendor-extensions.md); importing again"
        )
        status, log = run(command, 300)
    if status != 0 or not listed:
        print(log)
        print(
            f"run_vendor_seam_test.py: the import exited with {status} or did not list the extension (build it first)",
            file=sys.stderr,
        )
        return False
    return True


def run_check(godot: str, mode: str, args: argparse.Namespace, user_args: list[str]) -> list[str]:
    command = [godot, "--path", str(PROJECT_PATH)]
    if mode == "headless":
        command.insert(1, "--headless")
    else:
        # The dummy audio driver: a machine without a sound card would log an audio error.
        command += ["--rendering-driver", args.driver, "--gpu-validation", "--audio-driver", "Dummy"]
        if args.gpu_index:
            command += ["--gpu-index", args.gpu_index]
    if user_args:
        command += ["--"] + user_args
    status, log = run(command, 120 if mode == "headless" else 300)
    print(log)
    return check_log(log, mode, None if mode == "headless" else args.driver, status, args.gbv)


def run_rendered(godot: str, args: argparse.Namespace) -> list[str]:
    with tempfile.TemporaryDirectory() as frames:
        problems = run_check(godot, "rendered", args, [f"--seam-out={frames}"])
        frame = pathlib.Path(frames).joinpath("seam_frame.png")
        if frame.is_file():
            problems += run_check(godot, "reference", args, ["--seam-reference", f"--seam-compare={frame}"])
        else:
            problems.append("the rendered run wrote no frame to compare the GDScript samples' frame with")
    return problems


def exit_code_next_to(log_path: pathlib.Path) -> int | None:
    """The exit code in the done.json the Session-1 runner writes next to its run.log."""
    done = log_path.parent.joinpath("done.json")
    if not done.is_file():
        return None
    code = json.loads(done.read_text(encoding="utf-8-sig")).get("exit_code")
    return code if isinstance(code, int) else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rendered", action="store_true", help="the rendered check (needs a GPU)")
    parser.add_argument("--driver", default=None, help="rendering driver of the rendered check (default vulkan)")
    parser.add_argument("--gpu-index", default="", help="GPU index of the rendered check")
    parser.add_argument("--gbv", action="store_true", help="D3D12 GPU-based validation is (or must be) on")
    parser.add_argument("--check-log", default="", help="only check this log")
    parser.add_argument("--exit-code", type=int, default=None, help="Godot's exit code for --check-log")
    parser.add_argument(
        "--mode",
        default="headless",
        choices=["headless", "rendered", "reference"],
        help="the mode of the run in --check-log",
    )
    args = parser.parse_args()

    if args.check_log:
        log_path = pathlib.Path(args.check_log)
        log = log_path.read_text(encoding="utf-8", errors="replace")
        exit_code = args.exit_code if args.exit_code is not None else exit_code_next_to(log_path)
        problems = check_log(log, args.mode, args.driver, exit_code, args.gbv)
    else:
        godot = os.environ.get("GODOT4_BIN", "")
        if not godot:
            print("run_vendor_seam_test.py: set GODOT4_BIN to the Godot binary", file=sys.stderr)
            return 2
        if args.driver is None:
            args.driver = "vulkan"
        if not import_project(godot):
            return 1
        if not args.rendered:
            problems = run_check(godot, "headless", args, [])
        elif args.gbv:
            # GPU-based validation is a project setting: an override.cfg for these runs only.
            override = PROJECT_PATH.joinpath("override.cfg")
            if override.exists():
                print(f"run_vendor_seam_test.py: {override} exists; not replacing it", file=sys.stderr)
                return 2
            override.write_text("[rendering]\n\nrendering_device/d3d12/gpu_based_validation=true\n")
            try:
                problems = run_rendered(godot, args)
            finally:
                override.unlink()
        else:
            problems = run_rendered(godot, args)

    for problem in problems:
        print(f"run_vendor_seam_test.py: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("run_vendor_seam_test.py: the vendor-seam check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
