# GU+ metric fixtures (#202)

Small files written by hand from the tools' public documentation, and one CSV recorded with
PresentMon. No tool binary is in the tree. The folder's `.gdignore` keeps the files out of the
testbed's editor import, which would otherwise read each CSV as a translation table and write a
`.translation` file per column beside it.

| File | Written from |
|---|---|
| `presentmon_2_6_0_fg_off.csv` | The PresentMon 2.6.0 console README's default CSV columns, plus `MsPCLatency` (`--track_pc_latency`). Twelve rows of one swap chain: the first has no previous display change, one row is dropped (`DisplayedTime` NA), one window frame has no PC latency, and one frame is a 24 ms hitch. |
| `presentmon_2_6_0_recorded.csv` | Recorded, not written: PresentMon 2.6.0 (the release's `PresentMon-2.6.0-x64.exe`, checked against its published SHA-256) on the static-UI scene, D3D12, 1920x1080 with VSync, on the cloud Windows VM (NVIDIA L4) on 2026-11-15, through `presentmon_metrics record`. Kept: the header, data rows 301–530 and the last row of the 1,793-row CSV, in their order, with LF line endings. Other Godot processes presented during the recording, so it has three process / swap chain pairs; the run is PID 3432. Its header is the console's real default set: no `DisplayedTime` or `DisplayLatency` column, which the README lists, and `TimeInMs`, `CPUStartTimeInMs` and `MsFlipDelay`, which the hand-written file lacks. No Streamline, so `MsPCLatency` is `NA`. |
| `presentmon_v2_metrics.csv` | The PresentMon 2.3.0 README's columns, which `--v2_metrics` writes (`FrameTime`, `GPUBusy`, `DisplayedTime`; no PC latency). |
| `nsys_dx12_workload.sql` | Two tables under the names the Nsight Systems documentation gives for the SQLite export, with only the columns `nsys_dlssg.py` reads (`DX12_WORKLOAD`: `start`, `end`, `frameId`, `objectNameId`, `textId`; `StringIds`: `id`, `value`), written for this fixture, and invented rows. The tests load it into a temporary SQLite file. |

The expected values in `tests/test_fg_metrics.py` are worked out by hand from these rows; for the
recorded CSV they were computed from its rows with awk, independently of the parser.
