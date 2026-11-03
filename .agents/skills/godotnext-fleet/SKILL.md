---
name: godotnext-fleet
description: Describes the GodotNext build and test hardware (Apple M5 Max, NVIDIA RTX 4080 SUPER, NVIDIA RTX 5090 and a GPU-less Linux VM), what each is for, and how to use it. Use when a task needs a GPU run, a Windows or Vulkan build, a remote machine, or a rendered check of an HTML page.
---

# Use the GodotNext hardware

| Hardware | System | Use | Status |
|---|---|---|---|
| Apple M5 Max (Apple10) | macOS, Metal | Daily build loop; Metal-first P0 and P1 work; PR basics until the RTX 4080 SUPER is in use (§24); Metal lines in PQ | In use |
| NVIDIA RTX 4080 SUPER | Windows 11, Vulkan and D3D12 | Vulkan and D3D12 work (P2, PD), DLSS builds with the lead's own SDK, the RTX 4080 SUPER PR run, NVIDIA lines in PQ | Remote |
| NVIDIA RTX 5090 | Windows 11 | Every NVIDIA line in PQ; multi frame generation | To be set up before PQ (#227) |
| Linux x86_64 VM, no GPU | Linux, llvmpipe | CPU, doctest and software-Vulkan smoke runs; never DLSS-enabled builds | Available |

The RTX 4080 SUPER machine also has an Intel UHD 770 (`--gpu-index 0`), which runs nothing in v1. Use `--gpu-index 1` for the RTX 4080 SUPER. Mid-range NVIDIA, AMD and Intel GPUs ship "enabled, unvalidated" (§23.2).

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
3. GPU rendering needs the interactive Session-1 task runner from the harness (#12). A plain SSH session (Session 0) can't create a desktop swapchain. Use SSH for builds, file transfer and headless tests.

Build commands for each platform are in plan §13.1.

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
