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

`modules/dlss` is disabled by default. Developers who accept NVIDIA's terms
compile it against their own SDK checkout:

```
scons platform=windows target=editor dev_mode=yes d3d12=yes \
  module_dlss_enabled=yes dlss_backend=ngx dlss_sdk_path=<their SDK>
```

`dlss_backend=mock` compiles and tests without any SDK; CI builds and tests
only the mock backend. DLSS-enabled builds are allowed only on the owned RTX
4080 SUPER and RTX 5090 machines. Never on the Linux VM or a rented/cloud GPU
(Supplement §3; the five questions in `dlss-license.md`).

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
