"""The UE bar (#229, plan §23.3 and §23.4).

A timing line passes when GodotNext's value is at most 1.20x UE 5.9's on the
same machine and card. A higher-is-better line passes when GodotNext's value
is at least UE's divided by 1.20. Each run gives the line's own statistic
(p50, p95 or p99, the harness percentile) over its measured window. Each engine
gives the median of its runs, and the verdict compares the two medians. The
protocol is 5 runs per engine, interleaved GodotNext, UE, GodotNext, UE.

A line's status is one of:

- ``ue_bar``: judged against the bar. A UE column the config has not named yet
  keeps the bar but cannot be judged (§23.5); a named column that is missing
  from a capture is an error.
- ``report_only``: both numbers and the ratio, with the reason, and no verdict
  (per-pass lines, lines without a gate on the scene).
- ``no_equivalent``: UE 5.9 has no equivalent feature, mode or platform path;
  the GodotNext number and the reason only.

Linux timing lines are report-only (§23.3). A build-time run is a dry run: it
computes the numbers and the ratio, writes no verdict and is not a measurement
(§22). Judging is for PQ.
"""

from __future__ import annotations

import statistics
from collections.abc import Sequence
from dataclasses import dataclass

from godotnext_metrics.jsonutil import as_dict, as_list
from godotnext_metrics.percentile import percentile

UE_BAR = 1.20
RUNS_PER_ENGINE = 5
# Same tolerance as the harness spread check: a ratio that rounds to the bar passes.
TOLERANCE = 1e-9

STATISTICS = {"p50": 50.0, "p95": 95.0, "p99": 99.0}
# A frame whose selected columns are all blank (GodotNext skips a repeated timestamp frame) or,
# on the UE side, all blank or 0 (a missing GPU-stat sample) carries no value for the line. It
# is dropped and counted; a run that misses more than this share of its window is refused.
MAX_MISSING_SHARE = 0.05
STATUSES = ("ue_bar", "report_only", "no_equivalent")
DIRECTIONS = ("lower", "higher")
ENGINES = ("godotnext", "ue")

LINUX_REASON = "Linux timing lines are report-only (§23.3)."
DRY_RUN_NOTE = "Not judged: a build-time dry run is not a measurement (§22)."
UNNAMED_UE_NOTE = "The UE column is not named yet (#229 step 2)."
UNNAMED_UE_BAR_NOTE = "The UE column is not named yet (#229 step 2); the line keeps the UE bar (§23.5)."
UNDEFINED_GODOTNEXT_NOTE = "The GodotNext column group is not defined yet."


class ComparisonError(ValueError):
    """The set cannot be compared: it breaks the protocol or misses an input."""


@dataclass(frozen=True)
class GodotNextSelection:
    """Which ``timing.csv`` columns make the line's per-frame value (summed).

    ``columns`` names them; ``all_passes`` takes every pass column; ``prefix`` takes every
    column whose name starts with it except ``exclude``, for a frame block whose inner
    timestamps vary by tier or mode (the NextGI block: ``nextgi_*``), plus the ``include``
    columns a run has: a timestamp inside the block whose name lacks the prefix (line 1a's
    ``skinned_refit_ms``) appears only on the frames that emit it.
    """

    columns: tuple[str, ...] = ()
    all_passes: bool = False
    prefix: str = ""
    exclude: tuple[str, ...] = ()
    include: tuple[str, ...] = ()

    def to_json(self) -> dict[str, object]:
        if self.all_passes:
            return {"all_passes": True}
        if self.prefix:
            data: dict[str, object] = {"prefix": self.prefix, "exclude": list(self.exclude)}
            if self.include:
                data["include"] = list(self.include)
            return data
        return {"columns": list(self.columns)}

    def select(self, available: Sequence[str]) -> list[str]:
        """The pass columns of one run's ``timing.csv`` (in its order) this selection sums."""
        passes = [name for name in available if name not in {"frame", "wall_ms"}]
        if self.all_passes:
            return passes
        if self.prefix:
            return [
                name
                for name in passes
                if (name.startswith(self.prefix) or name in self.include) and name not in self.exclude
            ]
        return list(self.columns)


@dataclass(frozen=True)
class UeSelection:
    """The settings record's stat group and, once named, its CSV columns (summed)."""

    group: str
    columns: tuple[str, ...] | None

    def to_json(self) -> dict[str, object]:
        return {"group": self.group, "columns": None if self.columns is None else list(self.columns)}


@dataclass(frozen=True)
class LineSpec:
    id: str
    name: str
    gates: tuple[str, ...]
    status: str
    statistic: str
    direction: str
    reason: str
    godotnext: GodotNextSelection | None
    ue: UeSelection | None

    def to_json(self) -> dict[str, object]:
        return {
            "id": self.id,
            "name": self.name,
            "gates": list(self.gates),
            "status": self.status,
            "statistic": self.statistic,
            "direction": self.direction,
            "reason": self.reason,
            "godotnext": None if self.godotnext is None else self.godotnext.to_json(),
            "ue": None if self.ue is None else self.ue.to_json(),
        }


def parse_line(value: object) -> LineSpec:
    data = as_dict(value)
    line_id = _text(data, "id")
    status = _text(data, "status")
    if status not in STATUSES:
        raise ComparisonError(f"line {line_id}: status must be one of {', '.join(STATUSES)}, not {status!r}")
    statistic = _text(data, "statistic")
    if statistic not in STATISTICS:
        raise ComparisonError(f"line {line_id}: statistic must be p50, p95 or p99, not {statistic!r}")
    direction = _text(data, "direction")
    if direction not in DIRECTIONS:
        raise ComparisonError(f"line {line_id}: direction must be lower or higher, not {direction!r}")
    reason = str(data.get("reason") or "")
    if status != "ue_bar" and not reason:
        raise ComparisonError(f"line {line_id}: a {status} line needs a reason")
    gates = tuple(str(gate) for gate in as_list(data.get("gates", [])))
    godotnext = _godotnext_selection(line_id, data.get("godotnext"))
    ue = _ue_selection(line_id, data.get("ue"))
    if status == "no_equivalent" and ue is not None:
        raise ComparisonError(f"line {line_id}: a no_equivalent line has no UE side")
    if status == "ue_bar" and ue is None:
        raise ComparisonError(f"line {line_id}: a ue_bar line needs its UE stat group")
    return LineSpec(
        id=line_id,
        name=_text(data, "name"),
        gates=gates,
        status=status,
        statistic=statistic,
        direction=direction,
        reason=reason,
        godotnext=godotnext,
        ue=ue,
    )


def run_statistic(values: Sequence[float | None], statistic: str, expected_frames: int) -> tuple[float, int]:
    """The line's statistic over one run's measured window, and how many frames had no value.

    ``values`` has one entry per frame of the window; None is a frame without a sample. Those
    are dropped and counted, and more than ``MAX_MISSING_SHARE`` of the window is refused.
    """
    if statistic not in STATISTICS:
        raise ComparisonError(f"unknown statistic {statistic!r}")
    if expected_frames <= 0:
        raise ComparisonError("the measured window is empty")
    if len(values) != expected_frames:
        raise ComparisonError(f"the measured window has {len(values)} frames, not {expected_frames}")
    present = [value for value in values if value is not None]
    missing = len(values) - len(present)
    if not present or missing > MAX_MISSING_SHARE * expected_frames:
        raise ComparisonError(
            f"{missing} of {expected_frames} frames in the measured window have no value"
            f" (more than {MAX_MISSING_SHARE:.0%})"
        )
    return float(percentile(present, STATISTICS[statistic])), missing


def median_of_runs(values: Sequence[float]) -> float:
    if not values:
        raise ComparisonError("no runs to take a median of")
    return float(statistics.median(values))


def bar_ratio(godotnext: float, ue: float) -> float:
    if ue <= 0.0:
        raise ComparisonError(f"the UE median is {ue}; a ratio needs a positive UE value")
    return godotnext / ue


def bar_passes(godotnext: float, ue: float, direction: str) -> bool:
    """§23.3: lower-is-better passes at GodotNext <= 1.20 x UE, higher-is-better at >= UE / 1.20."""
    ratio = bar_ratio(godotnext, ue)
    if direction == "lower":
        return ratio <= UE_BAR + TOLERANCE
    if direction == "higher":
        return ratio >= 1.0 / UE_BAR - TOLERANCE
    raise ComparisonError(f"unknown direction {direction!r}")


def evaluate_line(
    spec: LineSpec,
    godotnext_runs: Sequence[float] | None,
    ue_runs: Sequence[float] | None,
    *,
    judge: bool,
    platform: str = "windows",
    godotnext_missing: Sequence[int] | None = None,
    ue_missing: Sequence[int] | None = None,
) -> dict[str, object]:
    """One report row. ``*_runs`` are per-run statistics, or None when that side has no columns.

    ``*_missing`` count the frames per run that had no value for the line (dropped).
    """
    status = spec.status
    reason = spec.reason
    if platform == "linux" and status != "no_equivalent":
        status = "report_only"
        reason = LINUX_REASON
    godotnext_median = median_of_runs(godotnext_runs) if godotnext_runs else None
    ue_median = median_of_runs(ue_runs) if ue_runs else None
    ratio = None
    if godotnext_median is not None and ue_median is not None:
        ratio = bar_ratio(godotnext_median, ue_median)
    verdict: str | None = None
    notes: list[str] = []
    if spec.godotnext is None:
        notes.append(UNDEFINED_GODOTNEXT_NOTE)
    if spec.ue is not None and spec.ue.columns is None:
        notes.append(UNNAMED_UE_BAR_NOTE if status == "ue_bar" else UNNAMED_UE_NOTE)
    if status == "ue_bar":
        if not judge:
            notes.append(DRY_RUN_NOTE)
        elif godotnext_median is None or ue_median is None:
            missing = "GodotNext column group" if godotnext_median is None else "UE column"
            raise ComparisonError(f"line {spec.id}: the {missing} is not set, so the line cannot be judged")
        else:
            verdict = "pass" if bar_passes(godotnext_median, ue_median, spec.direction) else "fail"
    return {
        "id": spec.id,
        "name": spec.name,
        "gates": list(spec.gates),
        "status": status,
        "reason": reason,
        "statistic": spec.statistic,
        "direction": spec.direction,
        "godotnext": {
            "selection": None if spec.godotnext is None else spec.godotnext.to_json(),
            "runs": list(godotnext_runs) if godotnext_runs else [],
            "missing_frames": list(godotnext_missing) if godotnext_missing else [],
            "median": godotnext_median,
        },
        "ue": {
            "selection": None if spec.ue is None else spec.ue.to_json(),
            "runs": list(ue_runs) if ue_runs else [],
            "missing_frames": list(ue_missing) if ue_missing else [],
            "median": ue_median,
        },
        "ratio": ratio,
        "bar": UE_BAR if status == "ue_bar" else None,
        "verdict": verdict,
        "notes": notes,
    }


@dataclass(frozen=True)
class RunRecord:
    """What the set checks need from one run, read from its manifest or runner record."""

    engine: str
    role: str
    index: int
    exit_code: int
    resolution: tuple[int, int] | None
    clocks_locked: bool
    godotnext_commit: str = ""
    godotnext_editor_binary: bool | None = None
    ue_build_version: str = ""
    ue_run_mode: str = ""
    ue_vsync: str = ""
    gpu_after: dict[str, object] | None = None


def interleave_order(runs: int, warmup: bool) -> list[tuple[str, str, int]]:
    """(role, engine, index): one unmeasured warm-up launch each, then G U G U ... (§23.4)."""
    if runs <= 0:
        raise ComparisonError("a comparison needs at least one run per engine")
    order: list[tuple[str, str, int]] = []
    if warmup:
        order += [("warmup", "godotnext", 0), ("warmup", "ue", 0)]
    for index in range(1, runs + 1):
        order += [("timing", "godotnext", index), ("timing", "ue", index)]
    return order


def check_set(
    records: Sequence[RunRecord],
    *,
    judge: bool,
    ue_run_mode: str,
    ue_build_version: str | None,
    job_kind: str = "timing",
) -> dict[str, object]:
    """Refuse a set that breaks §23.4. Returns the set's shared facts for the report.

    A judged set (PQ) also needs timing jobs (clock locks and the power-plan check), the
    warm-up launches, a pinned UE build and a GodotNext release export (§23.4, §25.4).
    """
    order = [(record.role, record.engine, record.index) for record in records]
    timing = [record for record in records if record.role == "timing"]
    runs = len(timing) // 2
    warmup = any(record.role == "warmup" for record in records)
    if runs == 0 or order != interleave_order(runs, warmup):
        raise ComparisonError("the runs are not interleaved GodotNext, UE, GodotNext, UE (§23.4)")
    failed = [f"{record.engine} {record.role} {record.index}" for record in records if record.exit_code != 0]
    if failed:
        raise ComparisonError("runs failed: " + ", ".join(failed))
    if judge:
        if runs != RUNS_PER_ENGINE:
            raise ComparisonError(f"a judged set has {RUNS_PER_ENGINE} runs per engine, not {runs} (§23.4)")
        if job_kind != "timing":
            raise ComparisonError(f"a judged set runs timing jobs, not {job_kind!r} (clock locks, power plan; §23.4)")
        if not warmup:
            raise ComparisonError("a judged set starts with the warm-up launches (§23.4)")
        if ue_build_version is None:
            raise ComparisonError("a judged set pins the UE build in ue.build_version (§25.4)")
    resolutions = {record.resolution for record in timing}
    if None in resolutions:
        raise ComparisonError("a run does not record its resolution")
    if len(resolutions) != 1:
        raise ComparisonError(f"the runs differ in resolution: {sorted(r for r in resolutions if r is not None)}")
    locked = {record.clocks_locked for record in timing}
    if len(locked) != 1:
        raise ComparisonError("clocks are locked for some runs and not others (§23.4: the same lock for both)")
    godotnext_timing = [record for record in timing if record.engine == "godotnext"]
    commits = {record.godotnext_commit for record in godotnext_timing}
    if len(commits) != 1 or "" in commits:
        raise ComparisonError(f"the GodotNext runs do not share one recorded commit: {sorted(commits)}")
    notes: list[str] = []
    editor = {record.godotnext_editor_binary for record in godotnext_timing}
    if editor != {False}:
        mismatch = "GodotNext ran from an editor binary, or did not record its build, not a release export (§23.4)"
        if judge:
            raise ComparisonError(mismatch)
        notes.append(mismatch[0].upper() + mismatch[1:] + ".")
    ue_timing = [record for record in timing if record.engine == "ue"]
    versions = {record.ue_build_version for record in ue_timing}
    if len(versions) != 1 or "" in versions:
        raise ComparisonError(f"the UE runs do not share one recorded build version: {sorted(versions)}")
    version = next(iter(versions))
    if ue_build_version is not None and version != ue_build_version:
        raise ComparisonError(f"the UE build is {version}; the comparison config pins {ue_build_version}")
    if ue_build_version is None:
        notes.append(f"The UE build is not pinned (ue.build_version); the runs recorded {version}.")
    modes = {record.ue_run_mode for record in ue_timing}
    if modes != {ue_run_mode}:
        raise ComparisonError(
            f"the UE runs recorded run mode {sorted(modes)}; the comparison config says {ue_run_mode}"
        )
    vsync = {record.ue_vsync for record in ue_timing}
    if vsync != {"0"}:
        raise ComparisonError(f"UE recorded vsyncenabled {sorted(vsync)}; VSync is off on both sides (§23.4)")
    resolution = next(iter(resolutions))
    assert resolution is not None
    return {
        "runs_per_engine": runs,
        "warmup_launches": warmup,
        "job_kind": job_kind,
        "resolution": list(resolution),
        "clocks_locked": next(iter(locked)),
        "godotnext_commit": next(iter(commits)),
        "godotnext_release_export": editor == {False},
        "ue_build_version": version,
        "ue_run_mode": ue_run_mode,
        "gpu_after": [
            {"engine": record.engine, "index": record.index, **(record.gpu_after or {})} for record in timing
        ],
        "notes": notes,
    }


def _text(data: dict[str, object], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise ComparisonError(f"{key} must be a non-empty string")
    return value


def _godotnext_selection(line_id: str, value: object) -> GodotNextSelection | None:
    if value is None:
        return None
    data = as_dict(value)
    kinds = [key for key in ("all_passes", "columns", "prefix") if key in data]
    if len(kinds) != 1:
        raise ComparisonError(f"line {line_id}: godotnext takes one of all_passes, columns or prefix, not both")
    for key in ("exclude", "include"):
        if key in data and kinds != ["prefix"]:
            raise ComparisonError(f"line {line_id}: godotnext {key} goes with prefix")
    if kinds == ["all_passes"]:
        if data.get("all_passes") is not True:
            raise ComparisonError(f"line {line_id}: godotnext all_passes is true")
        return GodotNextSelection(all_passes=True)
    if kinds == ["prefix"]:
        prefix = str(data.get("prefix") or "")
        if not prefix or not prefix.endswith("_"):
            raise ComparisonError(f"line {line_id}: godotnext prefix is a column-name prefix ending in _")
        exclude = tuple(str(column) for column in as_list(data.get("exclude", [])))
        include = tuple(str(column) for column in as_list(data.get("include", [])))
        for column in include:
            if not column or column.startswith(prefix) or column in exclude:
                raise ComparisonError(
                    f"line {line_id}: godotnext include names a column outside the prefix and not excluded: {column!r}"
                )
        return GodotNextSelection(prefix=prefix, exclude=exclude, include=include)
    columns = tuple(str(column) for column in as_list(data.get("columns")))
    if not columns:
        raise ComparisonError(f"line {line_id}: godotnext needs columns, all_passes or prefix")
    return GodotNextSelection(columns=columns)


def _ue_selection(line_id: str, value: object) -> UeSelection | None:
    if value is None:
        return None
    data = as_dict(value)
    group = _text(data, "group")
    raw = data.get("columns")
    if raw is None:
        return UeSelection(group=group, columns=None)
    columns = tuple(str(column) for column in as_list(raw))
    if not columns:
        raise ComparisonError(f"line {line_id}: UE columns are a non-empty list, or null until named")
    return UeSelection(group=group, columns=columns)
