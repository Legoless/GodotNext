# NextGI tier rule source log

The plan's clean-room protocol (§19) requires a source log per subsystem. This file
records every outside input used for the software-tier rule (#187).

## No outside input

The tier order, the SWRT display name, the level drop to a floor of 3, the
5 ms auto-quality split, the compute-unit class and the experimental runtime
toggle were written from the plan (§3.1, §21.0.2, §21.2 L1) and the issue.
No Unreal Engine source, shader, plugin, NvRTX sample or decompile was opened.

## Copied files

None.

## Mac HWRT policy (#131)

No outside source code was consulted. The macOS default (`hardware_ray_tracing.macos = Off`), the Apple8+ opt-in skipping only `NOT_HW_ACCELERATED` while keeping Apple7 on `NOT_HW_ACCELERATED` and preserving all safety/OS/driver checks, the `HWRT (EXPERIMENTAL)` overlay/log label on all Macs, and the non-script `highestFamily` query were implemented from the plan (§21.2 L11, §25) and issue #131. No Unreal Engine source, shader, plugin, NvRTX sample, or decompile was opened.
