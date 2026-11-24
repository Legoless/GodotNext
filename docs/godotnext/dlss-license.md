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

## Pins (recorded under lead sign-off, 2026-11-24)

The pinned NVIDIA DLSS SDK tag was cloned outside any git work tree under explicit lead authorization on 2026-11-24 (no SDK file is committed to this repository).

| Item | Value |
|---|---|
| Tag | `v310.9.16` (`9981c6350dc58c0ce21f024fd65e31e851c31689`) |
| Linux static library | `lib/Linux_x86_64/libnvsdk_ngx.a` (SHA-256 `d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df`). Note: DLSS Programming Guide rev `310.6.0` §5.1.2 still writes `libsdk_nvngx.a`, while tag `v310.9.16` ships `libnvsdk_ngx.a`; `modules/dlss/config.py` accepts either name. |
| Windows static libraries | `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` (`/MT`) and `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib` (`/MD`) |
| Guide revisions at tag | DLSS Programming Guide `310.6.0` (March 2026); DLSS-FG Programming Guide `310.7.0`; DLSS-RR Integration Guide (with §3.4.12 DoF guide) |
| U1 / U1M check | Confirmed unchanged against §2.1: RTX 4080 Preset J/K Performance = `0.47 / 0.71 / 1.49 ms` (×1.25 = `0.59 / 0.89 / 1.86 ms`) and `85.77 / 143.54 / 307.37 MB` (×1.25 = `107 / 179 / 384 MB`) at 1080p / 1440p / 2160p. |

### `lib/*/rel/` SHA-256 pins (`v310.9.16`)

| Relative path | SHA-256 |
|---|---|
| `lib/Windows_x86_64/rel/nvngx_dlss.dll` | `d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e` |
| `lib/Windows_x86_64/rel/nvngx_dlssd.dll` | `b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee` |
| `lib/Windows_x86_64/rel/nvngx_dlssg.dll` | `8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7` |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16` | `30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6` |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16` | `a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96` |
| `lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16` | `574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7` |
| `lib/Linux_aarch64/rel/libnvidia-ngx-dlss.so.310.9.16` | `ac59314b13b99f7688c13953052bbdbd5868460eccc95cb19fc27fa08ae3e64c` |
| `lib/Linux_aarch64/rel/libnvidia-ngx-dlssd.so.310.9.16` | `e63031b4351a591e42ee226744d649303279365e4e3ae0fac2a2a7f02c51b346` |
| `lib/Linux_aarch64/rel/libnvidia-ngx-dlssg.so.310.9.16` | `c45500c57ef47db3cfe5da4bc32921fc1f10995254b1ac9592ca31a4a92bb678` |
| `lib/Windows_aarch64/rel/nvngx_dlss.dll` | `922348b0f4af9571d60ad1514fc225a87da3a4da64bc6b93f6b6f3adb4baa852` |
| `lib/Windows_aarch64/rel/nvngx_dlssd.dll` | `bf952d457bbc30f6ada719ae46fe0fb135246587d05b112455056127bb2f1667` |
| `lib/Windows_aarch64/rel/nvngx_dlssg.dll` | `9cd0a94cd9ee3d1a0880cc2ed5a7985c19e5b0f09d17b6e538bb719c528ab9f9` |
| `lib/Windows_arm64ec/rel/nvngx_dlss.dll` | `a811fdc559ac48752361e27614f4b76b2e13987457c77039273086bc8b65882d` |
| `lib/Windows_arm64ec/rel/nvngx_dlssd.dll` | `a1187568263cffa74d1be671dfa117e92808e915c8cedb942c2a0679750ea350` |
| `lib/Windows_arm64ec/rel/nvngx_dlssg.dll` | `0b853e47497ddccfb2860f3e6c0e48273f568c9400a0b286662fdf2fd044dded` |

## The five questions for NVIDIA (plan §19(j))

Status: **not sent** (reason: lead chooses timing before commercial/public distribution; closing #98 does not trigger sending, and agents never send mail).

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
