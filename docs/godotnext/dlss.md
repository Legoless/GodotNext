# DLSS developer and distributor duties

Companion to `dlss-license.md` (plan §19). This page is for game developers who
build DLSS in with their own NVIDIA SDK and for anyone distributing such a
build. It is not legal advice.

## Official builds

Official GodotNext editor and export-template binaries exclude
`modules/dlss` and contain no NVIDIA object code. They are 100% MIT and
DLSS-free. Do not add NVIDIA files to the repository: the CI guard
(`misc/scripts/check_proprietary_sdk_files.py`) rejects them.

## Building with DLSS (developers only)

`modules/dlss` is disabled by default (`is_enabled() == False`). Developers who
accept NVIDIA's RTX SDKs License directly can fetch the pinned SDK outside any
git work tree using the helper script:

```
python3 misc/scripts/install_dlss_sdk.py --accept-nvidia-license
```

`install_dlss_sdk.py` clones `NVIDIA/DLSS` at the pinned tag (`v310.9.16`) into
`~/.godotnext/dlss-sdk/v310.9.16` (outside any git repository) and verifies the
pinned `lib/*/rel/` SHA-256 digests.

### Windows (MSVC or clang-cl, release CRT only)

```
scons platform=windows target=editor dev_mode=yes d3d12=yes \
  module_dlss_enabled=yes dlss_backend=ngx dlss_sdk_path=<their SDK>
```

- `use_mingw=yes` and `debug_crt=yes` (`/MDd`) are rejected at configure time
  because the NVIDIA DLSS SDK ships MSVC release-CRT static libraries
  (`nvsdk_ngx_s.lib` for `/MT` when `use_static_cpp=yes`, otherwise
  `nvsdk_ngx_d.lib` for `/MD`).
- Build outputs get the `.dlss` module suffix (e.g.
  `godot.windows.editor.x86_64.dlss.exe`), the unmodified `rel/` runtime DLL is
  copied into `bin/`, and `bin/dlss_runtime.json` records `{sdk_tag, variant, sha256, files}`.
- Pass `dlss_dev_runtime=yes` only for local debugging (copies `dev/` runtimes
  and emits a startup warning; never distribute or export `dev/` runtimes).

### Linux (native x86_64 Vulkan)

```
scons platform=linuxbsd target=editor dev_mode=yes \
  module_dlss_enabled=yes dlss_backend=ngx dlss_sdk_path=<their SDK>
```

- Links against `<sdk>/lib/Linux_x86_64/libnvsdk_ngx.a` (or `libsdk_nvngx.a`)
  and copies `libnvidia-ngx-dlss.so.<ver>` alongside `bin/dlss_runtime.json`.
- Native Linux x86_64 builds are validated directly on the RTX 5090 Linux
  workstation (WSL2 on the Windows RTX 4080 SUPER machine is not required once
  native Linux NVIDIA hardware is available).

### Mock backend (CI and local tests without the SDK)

```
scons platform=linuxbsd target=editor dev_mode=yes \
  module_dlss_enabled=yes dlss_backend=mock
./bin/godot.linuxbsd.editor.x86_64.dlss --headless --test --tc="*[DLSS]*"
```

`dlss_backend=mock` compiles and tests without any SDK and contains zero
`NVSDK_NGX` or `nvngx_dlss` symbols/strings. CI builds and tests only the
default and mock backends. DLSS-enabled `ngx` builds are allowed only on owned
NVIDIA RTX machines, never on rented/cloud GPUs (Supplement §3; the five
questions in `dlss-license.md`).

## Distributor duties (§19(f))

A build that links NGX contains NVIDIA object code. Distributing it requires:

- NVIDIA-compatible terms for the NVIDIA portions: license §2(c) (terms at
  least as protective), §2(d) (report non-compliant use), §4(b) and §4(e);
- the EULA terms referenced by §2(c);
- the notices in Supplement §1, §3, §4, §6.1–6.2 and §9.5 of the guides;
- the binding NVIDIA Marks attribution in Supplement §7.1(b), with the
  §7.2(c) approval for any mark use;
- the notification before commercial release (Supplement §4).

The About line in a DLSS-enabled editor needs NVIDIA's §7.2(c) approval before
that editor is distributed. RR runtime redistribution follows RR guide §4.2:
only the `rel/` release library ships, after a SHA-256 match; the NDA-package
DLLs must not be distributed.

## Export

The exporter warns about the distributor duties above and never copies
NVIDIA binaries from official builds. A DLSS-enabled export copies only the
`rel/` files listed in the manifest, after the SHA-256 check (GU+-8,
`docs/godotnext/dlss.md` is the record).
