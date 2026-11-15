"""Read UE CSV profiler captures for the UE 5.9 comparison (#229, plan §23.4).

Epic's public CSV Profiler page describes the file: one row per frame, one
column per stat, timer headings ``Category/Thread/Stat`` and other headings
``Category/Stat``, and an events column whose cells hold several events
separated by semicolons. UE 5.9 (the editor in game mode, and packaged builds)
also ends the file with the header row again and one metadata row of
``[key],value`` pairs, ``[HasHeaderRowAtEnd],1`` first (observed on the RTX 4080
SUPER machine; ``buildversion``, ``gpu``, ``rhiname``, ``rhifeaturelevel``,
``vsyncenabled`` and ``systemresolution.resx``/``.resy`` are among its keys, and
``-csvMetadata=Key=Value`` adds a lower-cased ``key``). The trailing header is the complete one:
a stat that first reports during the capture adds a column at the end, so the
first header is a prefix of it and earlier rows are shorter. Their missing
cells read as blank. The metadata row is not CSV-escaped: a value such as the
command line keeps its commas and quotes, so it is read as raw text, split only
where the next ``,[key],`` begins. Timer values are milliseconds.

Columns are selected by their exact header, as the settings record names them.
A header that appears twice in one capture cannot be selected (a real 5.9
capture repeats some, such as ``PSO/ComputePSOHitch``); a window check reads
every column of that name. Nothing here knows a UE stat name: the comparison
config supplies them.

The UE log (``-abslog``) is read for two things: the value a console variable
echoes when ``-ExecCmds`` names it without a value (``<name> = "<value>"``
after the line's time prefix, observed in the 5.9 editor in game mode), and the
``Metadata set`` lines.
"""

from __future__ import annotations

import csv
import io
import math
import re
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

EVENTS_HEADER = "events"
# A capture's first frames can carry hundreds of kilobytes of events in one cell.
_FIELD_LIMIT = 1 << 30

_METADATA_KEY = re.compile(r"^\[(.+)\]$")
_METADATA_LINE = re.compile(r"^\[[^\[\],]+\]")
_METADATA_SPLIT = re.compile(r",(?=\[[^\[\],]+\](?:,|$))")
_METADATA_PAIR = re.compile(r"^\[([^\[\],]+)\](?:,(.*))?$", re.DOTALL)
_LOG_METADATA = re.compile(r'Metadata set : ([A-Za-z0-9_]+)="(.*)"\s*$')
# A queried console variable echoes `<name> = "<value>"`, right after the log line's
# `[time][frame]` prefix (no category) and followed by `LastSetBy: <source>` (observed, UE 5.9).
_LOG_CVAR = re.compile(r'(?:^|[\s:\]])([A-Za-z][A-Za-z0-9_.]*) = "([^"]*)"')


class CsvProfileError(ValueError):
    """The capture is malformed, too short, or a column cannot be selected."""


class MissingColumnError(CsvProfileError):
    """A column the comparison config names is not in the capture."""


@dataclass
class CsvProfile:
    headers: list[str]
    rows: list[list[str]]
    metadata: dict[str, str] = field(default_factory=dict)
    header_row_at_end: bool = False

    @property
    def frames(self) -> int:
        return len(self.rows)

    def has_column(self, header: str) -> bool:
        return header in self.headers

    def events(self) -> list[list[str]]:
        """Events per frame. Empty lists when the capture has no events column."""
        index = self._events_index()
        if index is None:
            return [[] for _ in self.rows]
        return [[event for event in row[index].split(";") if event] for row in self.rows]

    def column(self, header: str) -> list[float | None]:
        """Per-frame values of one column. A blank cell is None."""
        index = self._index(header)
        return [_number(row[index], number, header) for number, row in enumerate(self.rows)]

    def columns_named(self, header: str) -> list[list[float | None]]:
        """Every column with this header (a capture can repeat one); empty when there is none."""
        found: list[list[float | None]] = []
        for index, name in enumerate(self.headers):
            if name != header:
                continue
            values: list[float | None] = []
            for number, row in enumerate(self.rows):
                values.append(_number(row[index], number, header))
            found.append(values)
        return found

    def _index(self, header: str) -> int:
        found = [index for index, name in enumerate(self.headers) if name == header]
        if not found:
            raise MissingColumnError(f"the capture has no column {header!r}")
        if len(found) > 1:
            raise CsvProfileError(f"column {header!r} appears {len(found)} times in the capture")
        return found[0]

    def _events_index(self) -> int | None:
        found = [index for index, name in enumerate(self.headers) if name.lower() == EVENTS_HEADER]
        return found[0] if len(found) == 1 else None


def parse_csv_profile(text: str) -> CsvProfile:
    csv.field_size_limit(max(csv.field_size_limit(), _FIELD_LIMIT))
    lines = text.splitlines()
    metadata: dict[str, str] = {}
    while lines and (not lines[-1].strip() or _METADATA_LINE.match(lines[-1].lstrip())):
        line = lines.pop()
        if line.strip():
            metadata = {**_metadata_pairs(line, len(lines) + 1), **metadata}
    rows = [row for row in csv.reader(io.StringIO("\n".join(lines))) if any(cell.strip() for cell in row)]
    if not rows:
        raise CsvProfileError(
            "the capture starts with a metadata row, not a header" if metadata else "the capture is empty"
        )
    first = rows[0]
    body = rows[1:]
    headers = first
    header_at_end = False
    if body and body[-1][: len(first)] == first:
        headers = body[-1]
        body = body[:-1]
        header_at_end = True
    elif metadata.get("hasheaderrowatend") == "1":
        raise CsvProfileError("the metadata says the header repeats at the end, and it does not")
    frames: list[list[str]] = []
    for number, row in enumerate(body, start=2):
        if _METADATA_KEY.match(row[0].strip()):
            raise CsvProfileError(f"row {number}: a metadata row before the last frame")
        if not len(first) <= len(row) <= len(headers):
            raise CsvProfileError(f"row {number} has {len(row)} cells; the header has {len(headers)}")
        frames.append(row + [""] * (len(headers) - len(row)))
    return CsvProfile(headers=headers, rows=frames, metadata=metadata, header_row_at_end=header_at_end)


def read_csv_profile(path: Path) -> CsvProfile:
    return parse_csv_profile(path.read_text(encoding="utf-8-sig", errors="strict"))


def read_log_metadata(text: str) -> dict[str, str]:
    """``Metadata set : key="value"`` lines a UE log prints when a capture starts."""
    found: dict[str, str] = {}
    for line in text.splitlines():
        match = _LOG_METADATA.search(line)
        if match:
            found[match.group(1).lower()] = match.group(2)
    return found


def read_log_cvars(text: str, names: Sequence[str]) -> dict[str, str]:
    """The last value each console variable echoed in the log (``<name> = "<value>"``)."""
    wanted = set(names)
    found: dict[str, str] = {}
    for line in text.splitlines():
        match = _LOG_CVAR.search(line)
        if match and match.group(1) in wanted:
            found[match.group(1)] = match.group(2)
    return found


def group_ms(profile: CsvProfile, headers: Sequence[str], warmup: int, frames: int) -> list[float | None]:
    """Per-frame sum of a stat group's columns over the measured window.

    The window drops ``warmup`` rows and keeps the next ``frames`` rows. A
    blank cell adds 0. A frame whose cells are all blank or 0 has no sample (a
    GPU-stat readback the profiler missed) and is None. A column with no value
    anywhere in the window is an error, so a stat that did not run cannot pass
    as 0 ms, and so is a column that first reports after the window starts.
    """
    if not headers:
        raise CsvProfileError("a stat group needs at least one column")
    if warmup < 0 or frames <= 0:
        raise CsvProfileError(f"bad window: warmup {warmup}, frames {frames}")
    if profile.frames < warmup + frames:
        raise CsvProfileError(
            f"the capture has {profile.frames} frames; the window needs {warmup} warm-up + {frames} measured"
        )
    totals = [0.0] * frames
    sampled = [False] * frames
    for header in headers:
        column = profile.column(header)
        values = column[warmup : warmup + frames]
        if all(value is None for value in values):
            raise CsvProfileError(f"column {header!r} has no value in the measured window")
        first = next(number for number, value in enumerate(column) if value is not None)
        if first > warmup:
            raise CsvProfileError(f"column {header!r} first reports at row {first}, after the window starts ({warmup})")
        for index, value in enumerate(values):
            if value is not None:
                totals[index] += value
                if value != 0.0:
                    sampled[index] = True
    return [total if sampled[index] else None for index, total in enumerate(totals)]


def window_activity(profile: CsvProfile, headers: Sequence[str], warmup: int, frames: int) -> dict[str, int]:
    """Rows of the measured window where a column of each header is non-zero (all same-named columns).

    The report uses it for stats that must be quiet once the warm-up is over, such as PSO
    misses and hitches. A header the capture does not have counts as quiet.
    """
    activity: dict[str, int] = {}
    for header in headers:
        rows: set[int] = set()
        for values in profile.columns_named(header):
            for index, value in enumerate(values[warmup : warmup + frames]):
                if value:
                    rows.add(index)
        activity[header] = len(rows)
    return activity


def _number(cell: str, number: int, header: str) -> float | None:
    text = cell.strip()
    if not text:
        return None
    try:
        value = float(text)
    except ValueError:
        raise CsvProfileError(f"frame {number}: column {header!r} holds {text!r}, not a number") from None
    if not math.isfinite(value):
        raise CsvProfileError(f"frame {number}: column {header!r} is not finite")
    return value


def _metadata_pairs(line: str, number: int) -> dict[str, str]:
    pairs: dict[str, str] = {}
    for token in _METADATA_SPLIT.split(line.strip()):
        match = _METADATA_PAIR.match(token)
        if not match:
            raise CsvProfileError(f"line {number}: metadata {token[:40]!r} is not [key],value")
        value = (match.group(2) or "").strip()
        if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
            value = value[1:-1].replace('""', '"')
        pairs[match.group(1).lower()] = value
    return pairs
