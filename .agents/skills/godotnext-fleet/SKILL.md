---
name: godotnext-fleet
description: Describes the GodotNext build and test hardware (Apple M5 Max, NVIDIA RTX 4080 SUPER, NVIDIA RTX 5090, two cloud NVIDIA L4 VMs for Linux and Windows, and a GPU-less Linux VM), what each is for, and how to use it. Use when a task needs a GPU run, a Linux or Windows build, a remote machine, or a rendered check of an HTML page.
---

# Use the GodotNext hardware

| Hardware | System | Use | Status |
|---|---|---|---|
| Apple M5 Max (Apple10) | macOS, Metal | Daily build loop; Metal-first P0 and P1 work; PR basics until the RTX 4080 SUPER is in use (§24); Metal lines in PQ | In use |
| NVIDIA RTX 4080 SUPER | Windows 11, Vulkan and D3D12 | Vulkan and D3D12 work (P2, PD), DLSS builds with the lead's own SDK, the RTX 4080 SUPER PR run, NVIDIA lines in PQ | Remote |
| NVIDIA RTX 5090 | Windows 11 | Every NVIDIA line in PQ; multi frame generation | To be set up before PQ (#227) |
| Linux x86_64 VM, no GPU | Linux, llvmpipe | CPU, doctest and software-Vulkan smoke runs; never DLSS-enabled builds | Available |
| Cloud Linux VM, NVIDIA L4 (16 vCPU, 64 GB) | Ubuntu 24.04, Vulkan (NVIDIA 580 driver), headless X on the L4 | Linux builds and doctests, Linux Vulkan validation runs on a second NVIDIA (Ada) card; never DLSS-enabled builds | In use since 2026-11-14; stop when idle |
| Cloud Windows VM, NVIDIA L4 (16 vCPU, 64 GB) | Windows Server 2025, Vulkan and D3D12 (NVIDIA GRID driver, WDDM) | Windows builds, Vulkan and D3D12 validation runs beside the RTX 4080 SUPER; never DLSS-enabled builds | In use since 2026-11-14; stop when idle |

The RTX 4080 SUPER machine also has an Intel UHD 770, which runs nothing in v1. In interactive Session 1 (where all rendered Godot runs happen), both Vulkan and D3D12 enumerate the NVIDIA GeForce RTX 4080 SUPER at `--gpu-index 0` and the Intel UHD 770 at `--gpu-index 1` (`nvidia-smi` also lists the RTX 4080 SUPER at `-i 0`, whereas SSH Session-0 `vulkaninfo` lists Intel first and uses `--json=1` for the RTX 4080 SUPER; resolve indices by device name from `docs/godotnext/fleet.md`). Mid-range NVIDIA, AMD and Intel GPUs ship "enabled, unvalidated" (§23.2).

## Using the RTX 4080 SUPER machine

1. It's reached over SSH through Tailscale, using the host alias in your own `~/.ssh/config`. Check the connection first:
   ```
   tailscale status
   ```
   If Tailscale is stopped, ask the user to start it. Don't change network settings yourself.
2. Run PowerShell remotely by piping a script, which avoids quoting problems:
   ```
   ssh -o BatchMode=yes <host> "powershell -NoProfile -Command -" < script.ps1
   ```
3. GPU rendering needs the interactive Session-1 task runner, `godotnext/testbed/harness/runners/win_session1.ps1` (#237). A plain SSH session (Session 0) can't create a desktop swapchain. Use SSH for builds, file transfer and headless tests, and the runner for every rendered run: from the M5 Max, `godotnext/testbed/harness/runners/run_remote.sh job <job.json> <dir>` (host alias and checkout path in the git-ignored `local/fleet.env`). It resolves `--gpu-index` by device name from `docs/godotnext/fleet.md`, locks clocks for timing jobs and returns Godot's exit code. See `godotnext/testbed/harness/README.md`.
4. The machine's OpenSSH default shell is PowerShell, which ends a session with exit code 1 whenever a child process fails. Append `; exit $LASTEXITCODE` to pass a child's code through. Piping a script into `powershell -Command -` drops multi-line blocks: copy a `.ps1` over and run it with `-File` instead.

Build commands for each platform are in plan §13.1.

## Using the cloud L4 VMs

The two cloud VMs are Compute Engine `g2-standard-16` machines with one NVIDIA L4 each. Their project, names, zones, IPs and the SSH key path are in the git-ignored `local/fleet.env` (`GODOTNEXT_GCE_*`, `GODOTNEXT_LINUX_*`, `GODOTNEXT_WIN_*`); the key itself is in `local/gcloud/`.

1. They bill while they run. Start one for a task and stop it afterwards with `gcloud compute instances start|stop <name> --project <project> --zone <zone>`. An external IP can change across a stop and start: read it with `gcloud compute instances describe`.
2. SSH in as `godotnext` with that key: `ssh -i <key> godotnext@<ip>`.
3. **Linux:** the checkout is `~/GodotNext`. Build with `scons platform=linuxbsd target=editor dev_mode=yes -j16`. Rendered runs use the headless X server on the L4 (`godotnext-xorg` service): set `DISPLAY=:0`, and `vulkaninfo --summary` lists the NVIDIA L4 and llvmpipe.
4. **Windows:** the SSH shell is `cmd`, not PowerShell as on the RTX 4080 SUPER machine, so don't append `; exit $LASTEXITCODE`; run a copied `.ps1` with `powershell -NoProfile -ExecutionPolicy Bypass -File <script>`. `nvidia-smi` reports the L4 in WDDM mode, which D3D12 and Vulkan rendering need.
5. **DLSS:** never on these VMs. NVIDIA's DLSS license (Supplement §3) leaves non-vGPU cloud use outside its scope, so DLSS-enabled builds run only on the owned cards.

## Checking an HTML page (the plan, the tracker)

Render it once in headless Chrome and look at the screenshot:
```
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --window-size=1240,2300 --virtual-time-budget=9000 --screenshot=/tmp/page.png "file://$PWD/<page>.html"
```
Don't use the Claude in Chrome browser tools for this.

## Rules

- **DLSS:** DLSS-enabled builds run only on the owned NVIDIA cards (§19). The NVIDIA SDK never enters the repository.
- **Timing:** don't run timing or quality measurements during the build. They belong to PQ (§22).
- **Background runs on the M5 Max:** set `GODOTNEXT_BACKGROUND=1` for every rendered (non-headless) Godot run, so it never takes the desktop's focus (`docs/godotnext/fleet.md`).
- **The UE install is a clean-room boundary (§19, §23.7).** Find it without walking it:
  - Locate the version folders one level deep only: `ls -d "<Epic Games folder>"/UE_*`, where the folder is the install location the Epic Games Launcher shows. On Windows, `Get-Item "<Epic Games folder>\UE_*"`.
  - Then name exact paths inside one version folder: `Engine/Binaries/Mac/UnrealEditor.app` and `Engine/Build/BatchFiles/RunUAT.sh` on macOS, `Engine\Binaries\Win64\UnrealEditor.exe` and `Engine\Build\BatchFiles\RunUAT.bat` on Windows.
  - Never list, walk, `find`, `grep` or `tree` the install root or a version folder recursively, and never point an agent's recursive directory-listing tool at them. Never open, list or search `Engine/Source`, `Engine/Shaders` or any plugin `Source` folder, including NVIDIA's DLSS plugin.
  - Read UE settings from Epic's public documentation and the editor's own settings UI only, not from the install's `.ini` files.
