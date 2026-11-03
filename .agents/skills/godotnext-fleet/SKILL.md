---
name: godotnext-fleet
description: Describes the GodotNext build and test machines (the M5 Max, the RTX 4080 SUPER machine with the RTX 4080 SUPER, the RTX 5090 machine and the Linux VM), what each is for, and how to reach and use them. Use when a task needs a GPU run, a Windows or Vulkan build, a remote machine, or a rendered check of an HTML page.
---

# Use the GodotNext machines

| Machine | GPU | Use | Status |
|---|---|---|---|
| M5 Max (the local Mac) | Apple M5 Max (Apple10), Metal | Daily build loop; Metal-first P0 and P1 work; PR basics until the RTX 4080 SUPER machine is in use (§24); Metal lines in PQ | In use |
| the RTX 4080 SUPER machine (the RTX 4080 SUPER machine, Windows 11) | RTX 4080 SUPER (`--gpu-index 1`) | Vulkan and D3D12 work (P2, PD), DLSS builds with the lead's own SDK, the RTX 4080 SUPER PR run, NVIDIA lines in PQ | Reached over Tailscale |
| Second machine | RTX 5090 | Every NVIDIA line in PQ; multi frame generation | To be set up before PQ (#227) |
| the Linux VM (Linux VM) | none (llvmpipe) | CPU, doctest and software-Vulkan smoke runs; never DLSS-enabled builds | Available |

The Intel UHD 770 in the RTX 4080 SUPER machine (`--gpu-index 0`) runs nothing in v1. Mid-range NVIDIA, AMD and Intel GPUs ship "enabled, unvalidated" (§23.2).

## Reaching the RTX 4080 SUPER machine

1. The RTX 4080 SUPER machine is reached over Tailscale with the RTX 4080 SUPER machine SSH alias from your own `~/.ssh/config`. Check the connection first:
   ```
   tailscale status
   ```
   If Tailscale is stopped, ask the user to start it. Don't change network settings yourself.
2. Run PowerShell remotely by piping a script, which avoids quoting problems:
   ```
   ssh -o BatchMode=yes the RTX 4080 SUPER machine "powershell -NoProfile -Command -" < script.ps1
   ```
3. GPU rendering needs the interactive Session-1 task runner from the harness (#12). A plain SSH session (Session 0) can't create a desktop swapchain. Use SSH for builds, file transfer and headless tests.

Build commands for each machine are in plan §13.1.

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
