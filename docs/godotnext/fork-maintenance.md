# Fork maintenance

GodotNext is a fork of [Godot Engine](https://github.com/godotengine/godot).
This document says where the fork starts, how it treats upstream Godot
during v1, and how changes to upstream files are kept small and countable.
The plan references are to [the NextGI/NextGeo plan](nextgi-nextgeo-plan.html):
§12 (P0), §13.1 (fork CI hygiene), §15 (upstream policy), §17 item 1 and
§18.1 (the upstream decision).

## Where the fork starts

Commit `7cfeb473cc58d124c56b302fb313541471d96163` ("Initial GodotNext
snapshot from Godot master", short `7cfeb47`) is a single-commit import of
upstream `godotengine/godot@95c0fa5`, a Godot 4.8 beta. It has no parent,
so the fork carries no upstream history. The import's tree is
`adad3afdd695892d1d5fc565a2837fe9e628b562`.

## No upstream merges in v1

The lead decided on 2026-11-03 that GodotNext stays on this snapshot for
all of v1 (plan §18.1):

- No upstream release or branch is merged, so the fork needs no upstream
  history. No replace-ref graft is made, and gate check G0-5 is dropped.
- When a specific upstream fix is needed, cherry-pick it by hand, as
  described below.
- Re-creating the fork on upstream history would need a force-push of
  `master`. Only the lead can decide that; agents and scripts never do it.

## Taking a specific upstream fix

1. Find the upstream commit, for example on GitHub, and read its diff.
2. Apply it as a normal commit on `master`, for example
   `git apply` on the downloaded `.patch` of that commit, or by hand if it
   doesn't apply cleanly.
3. Name the upstream commit and pull request in the commit message, for
   example `Cherry-pick upstream godotengine/godot@<sha> (#<pr>): <title>`.
4. Run the §13.1 repository checks before committing:

   ```sh
   ./bin/godot.<platform>.editor.<arch> --headless --test --force-colors
   ./bin/godot.<platform>.editor.<arch> --doctool --headless
   git diff --exit-code -- doc/classes && git ls-files --others --exclude-standard -- doc/classes | (! grep .)
   ```

A cherry-picked upstream fix edits upstream files, so it appears in the
upstream-delta report (below). That is expected; the commit message says
where it came from.

## Named-hook rule

Fork code stays out of upstream files wherever possible:

- New code goes in new files.
- When an upstream file must be edited, the edit sits in a marked block
  opened by a `// GodotNext: <hook_name>` comment, where `<hook_name>`
  names the hook (for example the subsystem and what it hooks). Keep the
  block as small as the hook allows, so it can be found and reviewed by
  searching for the marker.
- The class reference XML in `doc/classes` is the exception: `--doctool`
  regenerates it and drops comments, so its edits stay unmarked.

Lines changed in upstream files are tracked at each gate with
[`misc/scripts/godotnext_upstream_delta.py`](../../misc/scripts/godotnext_upstream_delta.py),
which reports the changed-line totals, the largest files, and every
`GodotNext:` marker in the changed upstream files:

```sh
python3 misc/scripts/godotnext_upstream_delta.py
python3 misc/scripts/godotnext_upstream_delta.py --json
```

Both forms diff `HEAD` against the import commit `7cfeb47` by default
(override with `--base`). They keep only paths that already exist at the
import (new fork files are not upstream delta).

## Upstream pull requests

There are no upstream Godot pull requests during v1 (§19). RenderingDevice
and driver ray-tracing fixes that could be upstreamed later stay as
isolated commits (§15 upstream policy), so they can be extracted without
untangling them from NextGI/NextGeo work.
