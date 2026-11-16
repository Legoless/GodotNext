"""UE CSV profiler parsing (#229) against hand-written synthetic captures.

``documented.csv`` has only what Epic's public CSV Profiler page describes: one
row per frame, ``Category/Thread/Stat`` and ``Category/Stat`` headings and an
events column with semicolon-separated events. ``packaged.csv`` adds what a
UE 5.9 capture writes (observed from packaged builds and from the editor in
game mode): after the frames the header again,
complete, and one ``[key],value`` metadata row; a stat that first reports in
frame 3 adds a column at the end, so earlier rows are shorter; and a heading
that appears twice. Stat names and metadata values are invented; no file here
was written by UE.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from godotnext_metrics.ue_csv import (
    CsvProfileError,
    MissingColumnError,
    group_ms,
    parse_csv_profile,
    read_csv_profile,
    read_log_cvars,
    read_log_metadata,
    read_log_upscale_sizes,
    window_activity,
)

_FIXTURES = Path(__file__).resolve().parent / "fixtures" / "ue_csv"


def test_the_fixture_folder_is_hidden_from_godots_importer() -> None:
    # The folder sits inside the testbed project; without .gdignore Godot imports each CSV as a
    # translation table and logs an error per column.
    assert (_FIXTURES / ".gdignore").is_file()


def test_documented_capture_has_one_row_per_frame_and_no_metadata() -> None:
    profile = read_csv_profile(_FIXTURES / "documented.csv")
    assert profile.frames == 6
    assert profile.headers[1] == "Synthetic/RenderThread/Work"
    assert profile.metadata == {}
    assert profile.header_row_at_end is False
    assert profile.events()[1] == ["Boot", "Load"]
    assert profile.events()[0] == []
    assert profile.column("Synthetic/GPU/PassB") == [0.5, 0.4, None, 0.6, 0.7, 0.8]


def test_packaged_capture_reads_trailing_header_and_metadata() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    assert profile.frames == 8
    assert profile.header_row_at_end is True
    assert profile.metadata["config"] == "Development"
    assert profile.metadata["buildversion"] == "++Synthetic+Release-0.0-CL-1"
    assert profile.metadata["vsyncenabled"] == "0"
    assert profile.metadata["hasheaderrowatend"] == "1"
    # A quoted value keeps its comma.
    assert profile.metadata["commandline"] == "-synthetic -list=a,b"
    assert profile.events()[0] == ["Region: Synthetic - Begin##0.10", "Region: Synthetic - End##0.20"]
    # The trailing header is the complete one; rows from before the late stat read it as blank.
    assert len(profile.headers) == 8
    assert profile.column("Synthetic/GPU/LateStat") == [None, None, None, 0.1, 0.2, None, 0.3, 0.4]


def test_a_group_is_summed_per_frame_over_the_window() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    columns = ("Synthetic/GPU/PassA", "Synthetic/GPU/PassB", "Synthetic/GPU/PassC")
    # Two warm-up rows dropped, six kept; a blank cell adds 0.
    assert group_ms(profile, columns, warmup=2, frames=6) == [1.5, 3.0, 3.75, 6.0, 1.5, 3.0]
    assert group_ms(profile, ("Synthetic/GPU/PassA",), warmup=0, frames=2) == [5.0, 4.0]


def test_the_window_must_fit_in_the_capture() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    with pytest.raises(CsvProfileError, match="has 8 frames"):
        group_ms(profile, ("Synthetic/GPU/PassA",), warmup=3, frames=6)
    with pytest.raises(CsvProfileError, match="bad window"):
        group_ms(profile, ("Synthetic/GPU/PassA",), warmup=0, frames=0)
    with pytest.raises(CsvProfileError, match="at least one column"):
        group_ms(profile, (), warmup=0, frames=1)


def test_a_missing_column_is_an_error_not_zero() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    with pytest.raises(MissingColumnError, match="no column 'Synthetic/GPU/PassD'"):
        group_ms(profile, ("Synthetic/GPU/PassA", "Synthetic/GPU/PassD"), warmup=2, frames=6)


def test_a_repeated_heading_cannot_be_selected() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    with pytest.raises(CsvProfileError, match="appears 2 times"):
        profile.column("Counters/Repeated")
    # The other columns still read.
    assert profile.column("Synthetic/GameThread/Work")[0] == 9.0


def test_a_column_without_a_value_in_the_window_is_an_error() -> None:
    text = "events,Synthetic/GPU/Idle,Synthetic/GPU/Busy\n,1.0,1.0\n,,2.0\n,,3.0\n"
    profile = parse_csv_profile(text)
    with pytest.raises(CsvProfileError, match="no value in the measured window"):
        group_ms(profile, ("Synthetic/GPU/Idle",), warmup=1, frames=2)
    assert group_ms(profile, ("Synthetic/GPU/Busy",), warmup=1, frames=2) == [2.0, 3.0]


def test_malformed_captures_are_refused() -> None:
    with pytest.raises(CsvProfileError, match="empty"):
        parse_csv_profile("\n\n")
    with pytest.raises(CsvProfileError, match="has 2 cells"):
        parse_csv_profile("events,A/B,A/C\n,1.0\n")
    with pytest.raises(CsvProfileError, match="a metadata row before the last frame"):
        parse_csv_profile("events,A/B\n,1.0\n[config],Test\n,2.0\n")
    with pytest.raises(CsvProfileError, match="does not"):
        parse_csv_profile("events,A/B\n,1.0\nevents,A/C\n[HasHeaderRowAtEnd],1\n")
    with pytest.raises(CsvProfileError, match="has 3 cells; the header has 2"):
        parse_csv_profile("events,A/B\n,1.0,2.0\n")
    with pytest.raises(CsvProfileError, match="has 1 cells; the header has 3"):
        parse_csv_profile("events,A/B\n,1.0\nx\nevents,A/B,A/C\n[HasHeaderRowAtEnd],1\n")
    with pytest.raises(CsvProfileError, match="is not \\[key\\],value"):
        parse_csv_profile("events,A/B\n,1.0\n[config]Test\n")
    with pytest.raises(CsvProfileError, match="starts with a metadata row"):
        parse_csv_profile("[config],Test\n")
    with pytest.raises(CsvProfileError, match="not a number"):
        parse_csv_profile("events,A/B\n,abc\n").column("A/B")


def test_metadata_values_keep_their_commas_and_quotes() -> None:
    # The command line is written as it is, not CSV-escaped: quotes and a comma inside it.
    text = (
        "events,A/B\n,1.0\nevents,A/B\n"
        '[HasHeaderRowAtEnd],1,[os],Synthetic OS [1.2.3] ,[commandline],"" -x -ExecCmds="a 1,b 2""",'
        "[config],Development,[empty],\n"
    )
    profile = parse_csv_profile(text)
    assert profile.frames == 1
    assert profile.metadata["config"] == "Development"
    assert profile.metadata["os"] == "Synthetic OS [1.2.3]"
    assert "-ExecCmds" in profile.metadata["commandline"]
    assert "b 2" in profile.metadata["commandline"]
    assert profile.metadata["empty"] == ""


def test_a_long_events_cell_is_read() -> None:
    events = ";".join(f"Region: Synthetic {index} - Begin##{index}.0" for index in range(10000))
    assert len(events) > 131072
    profile = parse_csv_profile(f"events,A/B\n{events},1.0\n,2.0\n")
    assert len(profile.events()[0]) == 10000
    assert profile.column("A/B") == [1.0, 2.0]


def test_log_metadata_lines_are_read() -> None:
    log = "\n".join([
        '[2026.11.15-12.00.00:000][  0]LogCsvProfiler: Display: Metadata set : config="Development"',
        'LogCsvProfiler: Display: Metadata set : buildversion="++Synthetic+Release-0.0-CL-1"',
        'LogCsvProfiler: Display: Metadata set : vsyncenabled="0"',
        "LogSomething: an unrelated line",
    ])
    assert read_log_metadata(log) == {
        "config": "Development",
        "buildversion": "++Synthetic+Release-0.0-CL-1",
        "vsyncenabled": "0",
    }


def test_a_frame_without_a_sample_has_no_value_not_zero() -> None:
    # The second window row has every selected cell at 0 (a GPU-stat sample the profiler missed)
    # and the third has them blank; neither counts as a 0 ms frame.
    text = "events,G/A,G/B,G/Other\n,1.0,1.0,1.0\n,0,0,1.0\n,,,1.0\n,0.5,0,1.0\n"
    profile = parse_csv_profile(text)
    assert group_ms(profile, ("G/A", "G/B"), warmup=0, frames=4) == [2.0, None, None, 0.5]


def test_a_column_that_first_reports_inside_the_window_is_an_error() -> None:
    profile = read_csv_profile(_FIXTURES / "packaged.csv")
    # LateStat first reports at row 3; a window that starts at row 2 would count row 2 as 0 ms.
    with pytest.raises(CsvProfileError, match="first reports at row 3, after the window starts"):
        group_ms(profile, ("Synthetic/GPU/LateStat",), warmup=2, frames=6)
    assert group_ms(profile, ("Synthetic/GPU/LateStat",), warmup=3, frames=4) == [0.1, 0.2, None, 0.3]


def test_quiet_columns_count_active_window_rows_over_repeated_headings() -> None:
    text = "events,PSO/Hitch,PSO/Hitch,PSO/Miss\n,1,0,2\n,0,1,0\n,0,0,0\n,0,1,0\n"
    profile = parse_csv_profile(text)
    assert window_activity(profile, ("PSO/Hitch", "PSO/Miss", "PSO/Absent"), warmup=1, frames=3) == {
        "PSO/Hitch": 2,
        "PSO/Miss": 0,
        "PSO/Absent": 0,
    }


def test_console_variable_echoes_are_read_from_the_log() -> None:
    log = "\n".join([
        "[2026.11.15-12.00.00:000][  0]LogConsoleResponse: Display: Cmd: synthetic.Value 1",
        '[2026.11.15-12.00.00:001][  0]synthetic.Value = "1"',
        '[2026.11.15-12.00.00:002][  0]synthetic.Other = "2.000000"      LastSetBy: Scalability',
        'LogConsoleResponse: Display: synthetic.Value = "0"      LastSetBy: Console',
        '[2026.11.15-12.00.00:003][  0]LogSomething: synthetic.Valuex = "9"',
        "LogSomething: synthetic.Value is mentioned without an echo",
    ])
    assert read_log_cvars(log, ["synthetic.Value", "synthetic.Other", "synthetic.Missing"]) == {
        "synthetic.Value": "0",
        "synthetic.Other": "2.000000",
    }


def test_the_tsr_draw_event_sizes_are_read_from_a_gpu_profile() -> None:
    # Synthetic lines in the observed shape: the draw event's name, its parameters in parentheses, then
    # the rendering and display resolutions. Other TSR passes and sizes without the arrow do not count.
    log = "\n".join([
        "[2026.11.16-09.43.05:000][  2]LogSynthetic:  1.00ms  TemporalSuperResolution(synthetic=1) 1920x1080 -> 3840x2160",
        "[2026.11.16-09.43.05:001][  2]LogSynthetic:  0.10ms  TSR SyntheticPass 1920x1080",
        "[2026.11.16-09.43.05:002][  2]LogSynthetic:  0.20ms  TemporalSuperResolution 1920x1080",
        "[2026.11.16-09.43.05:003][  2]LogSynthetic:  0.30ms  SomethingElse(x) 960x540 -> 1920x1080",
        "[2026.11.16-09.43.09:000][  9]LogSynthetic:  1.00ms  TemporalSuperResolution(synthetic=2) 1280x720->2560x1440",
    ])
    assert read_log_upscale_sizes(log) == [(1920, 1080, 3840, 2160), (1280, 720, 2560, 1440)]
    assert read_log_upscale_sizes("no profile here") == []
