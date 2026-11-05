# DLSS license packet

Engineering record for the NVIDIA RTX SDKs (plan §19, GU-6). It holds URLs,
version numbers, section numbers and short quotes only. **No NVIDIA header,
library, DLL, sample or documentation text is committed.** This document is
not legal advice; obtain counsel before a commercial release.

## Distribution model (decided 2026-11-02)

Official GodotNext editor and export-template binaries exclude `modules/dlss`
and stay 100% MIT and DLSS-free. Developers compile DLSS in with their own
NVIDIA SDK and accept NVIDIA's terms themselves. The Linux VM runs only default
and mock builds; DLSS-enabled test builds run on owned NVIDIA cards (§23.2).

## License references

NVIDIA RTX SDKs License, v. 2024-03-14:
https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt

| Section | Subject | Short reference |
|---|---|---|
| §1(c) | distribution of SDK software in object-code form | "as incorporated in object code format into a software application" |
| §2(a) | additional functionality | applications must add material functionality beyond the SDK |
| §2(c) | protective terms for the NVIDIA portions | distribute under terms at least as protective as the license |
| §2(d) | non-compliance notification | notify NVIDIA in writing of known non-compliant distribution |
| §4(b), §4(e) | limitations | no OSS-licensing of the SDK itself; other restrictions |
| §19 | licensing contact | nvidia-rtx-license-questions@nvidia.com |

DLSS SDK Supplement (same file): §1 (definitions/identification), §3 (cloud
use: non-vGPU cloud use is outside the license), §4 (notification before
commercial release), §6.1–6.2 (over-the-air updates), §7.1(b) (binding NVIDIA
Marks attribution), §7.2(c) (trademark approval).

NGX SPDX banner: `LicenseRef-NvidiaProprietary`.

DLSS Programming Guide 310.6.0: §4.2, §4.2.2, §4.2.3 and §9.5 (redistribution
and notices). DLSS RR guide §4.2: the public `lib/…/rel/` folder holds the
release library; the DLLs "included in the NDA SDK package" must not be
distributed and require contacting NVIDIA.

Streamline: MIT except `sl_nvperf.h` / `sl_nvperf.dll` (Nsight Perf SDK
license). `NVIDIA-RTX/godot`: MIT, but its `thirdparty/streamline/include`
holds `sl_nvperf.h`; `nvapi_minimal.h` provenance is unverified. Neither is
used as an input.

## Forbidden inputs (plan §19)

DLSS sample/Donut source, the UE DLSS plugin source, NvRTX, and any NVIDIA
source under MIT-incompatible terms are never read. UE projects stay outside
the repository.

**§19(k) disclosure:** on 2026-11-02 the research step read the NGX headers,
the DLSS/RR/FG/RTX UI guides, Streamline documentation and NVIDIA-RTX/godot
copies in a session scratchpad outside the repository. None is committed.

## Pins (lead action; agents never accept the SDK terms)

The lead clones the pinned tag outside any git work tree and records:

| Item | Status |
|---|---|
| Tag | `v310.9.16` (plan §18.2 and the GitHub releases page); **not cloned by an agent** |
| `lib/*/rel/` SHA-256 | pending the lead's pin-recording clone |
| Linux static filename | `libsdk_nvngx.a` per DLSS guide §5.1; verify at the pinned tag |
| Guide revision at the tag | pending |
| U1/U1M checks after re-reading the guide | pending; log any change in Appendix A |

## The five questions for NVIDIA (plan §19(j))

Status: **not sent**. The lead chooses the time; closing this packet does not
trigger sending, and agents never send mail.

1. Is MIT source that `#include`s, but does not contain, NGX headers
   acceptable in a public repository?
2. May GodotNext publish editor and template binaries with NGX linked, and on
   what terms?
3. Is the public `lib/…/rel/nvngx_dlssd.dll` redistributable (RR guide §4.2)?
4. Will NVIDIA assign an engine type or application ID, or should each game
   use its own Project ID with `ENGINE_TYPE_CUSTOM`?
5. May we build and run DLSS-enabled test binaries on a cloud VM we control
   (GCP), given Supplement §3?

Answers are recorded here when they arrive.

## CI guard

`misc/scripts/check_proprietary_sdk_files.py` checks `git ls-files` (or
`--root DIR`) against the SDK basenames and exits 1 on a hit. The
`.github/workflows/static_checks.yml` step runs it on the checkout and on a
planted fixture outside the checkout; `.gitignore` carries the same patterns.
