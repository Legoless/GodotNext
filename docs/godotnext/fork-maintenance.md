# Fork maintenance

GodotNext is a fork of [Godot Engine](https://github.com/godotengine/godot).
This document says how the fork stays connected to upstream history, how
upstream releases are merged, and how changes to upstream files are kept
small and countable. The plan references are to
[the NextGI/NextGeo plan](nextgi-nextgeo-plan.html): §12 (P0 history graft,
gate G0-5), §13.1 (fork CI hygiene), §15 (upstream policy) and §17 item 1.

## Where the fork starts

Commit `7cfeb473cc58d124c56b302fb313541471d96163` ("Initial GodotNext
snapshot from Godot master", short `7cfeb47`) is a single-commit import of
upstream `godotengine/godot@95c0fa5`. It has no parent, so a plain clone
carries no upstream history and `git merge-base` against upstream finds
nothing. The import's tree is
`adad3afdd695892d1d5fc565a2837fe9e628b562`.

`master` is not rewritten to fix this. Instead, a git *replace ref* grafts
the import commit onto the upstream commit it came from:
`7cfeb47` is locally replaced by a graft commit whose parent is `95c0fa5`.
With the graft in place, `git merge-base HEAD upstream/master` resolves to
`95c0fa5` (gate G0-5), and upstream merges, `git log`, `git blame` and the
upstream-delta report (below) all work against real upstream history.

Re-creating the fork directly on upstream history instead would need a
force-push of `master`. That is the lead's explicit decision only; it is
not part of normal fork maintenance and is not done by agents or scripts.

## Replace refs do not travel by default

Two limits of replace refs shape everything below:

- **They are not fetched by default.** A graft created in one clone does
  not reach other clones on its own. The graft is published once as
  `refs/replace/*` on `origin`, and every clone that wants upstream
  history fetches it explicitly (see "Clone setup").
- **GitHub and GitHub Actions checkouts ignore them.** The GitHub web UI
  does not show grafted history, and `actions/checkout` does not fetch
  `refs/replace/*`. CI must not depend on the graft; it exists for local
  merges and reports. Git commands that must see the un-grafted history
  can be run with `git --no-replace-objects ...` or
  `GIT_NO_REPLACE_OBJECTS=1`.

## Creating and publishing the graft (one time, lead)

Run this once in a full (not shallow, not single-branch) clone. The same
steps, with checks, are in
[`misc/scripts/godotnext_graft.sh`](../../misc/scripts/godotnext_graft.sh);
the script never pushes unless given `--push`.

1. Add the upstream remote if it is missing, and fetch upstream `master`:

   ```sh
   git remote add upstream https://github.com/godotengine/godot.git
   git fetch upstream master
   ```

   If `95c0fa5` is still not reachable afterwards (for example in a
   filtered clone), fetch it by its full SHA:

   ```sh
   git fetch upstream <full 40-character SHA of 95c0fa5>
   ```

2. Compare the import tree with the upstream tree:

   ```sh
   git rev-parse 95c0fa5^{tree}
   git rev-parse 7cfeb47^{tree}
   ```

   If the two trees differ, record the difference under
   "Import tree comparison" below with:

   ```sh
   git diff --stat 95c0fa5 7cfeb47
   ```

3. Create the graft and verify the merge base:

   ```sh
   git replace --graft 7cfeb47 95c0fa5
   git merge-base HEAD upstream/master
   ```

   The second command must print the full SHA of `95c0fa5`.

4. Publish the graft (lead only; agents do not push refs):

   ```sh
   git push origin 'refs/replace/*'
   git ls-remote origin 'refs/replace/*'
   ```

   The second command must list the graft ref.

5. Verify from a fresh clone:

   ```sh
   git clone <origin URL> godotnext-fresh
   cd godotnext-fresh
   git remote add upstream https://github.com/godotengine/godot.git
   git fetch upstream master
   git fetch origin 'refs/replace/*:refs/replace/*'
   git merge-base HEAD upstream/master
   ```

   The last command must again print the full SHA of `95c0fa5`. This is
   gate G0-5; G0 re-runs it from a fresh clone in the final test phase.

### Import tree comparison

Recorded when the graft is created (step 2 above):

- `7cfeb47^{tree}`: `adad3afdd695892d1d5fc565a2837fe9e628b562`
- `95c0fa5^{tree}`: _to be filled in when the graft is created._
- `git diff --stat 95c0fa5 7cfeb47`: _to be filled in if the trees differ;
  if they are identical, say so here._

## Clone setup

A clone that merges upstream, runs the upstream-delta report against
`upstream/master`, or needs upstream history for `log`/`blame` fetches the
graft once:

```sh
git remote add upstream https://github.com/godotengine/godot.git
git fetch upstream master
git fetch origin 'refs/replace/*:refs/replace/*'
```

To keep the graft updated on every later `git fetch`, add the refspec to
the `origin` fetch list once:

```sh
git config --add remote.origin.fetch '+refs/replace/*:refs/replace/*'
```

Clones that never merge upstream do not need the graft; building and CI
work without it.

## Merge cadence

Upstream is merged at each upstream `4.x-stable` tag, not continuously.

- Each tag merge goes through a `chore/upstream-merge-<tag>` pull
  request, for example `chore/upstream-merge-4.6-stable`. The merge needs
  a clone with the graft (above), so the merge base is real upstream
  history.
- Before the pull request is merged, run the §13.1 repository checks on
  the merge result: the doctests, the doctool diff, and the NextGI-off
  regression subset (the tests that cover upstream behavior with NextGI
  disabled, which is the default):

  ```sh
  ./bin/godot.<platform>.editor.<arch> --headless --test --force-colors
  ./bin/godot.<platform>.editor.<arch> --doctool --headless
  git diff --exit-code -- doc/classes && git ls-files --others --exclude-standard -- doc/classes | (! grep .)
  ```

- Attach the upstream-delta report (below) for the merge result to the
  pull request, so growth in upstream-file edits is visible at every tag.
- The first upstream tag merge is its own task; nothing in this document
  starts it.

## Named-hook rule

Fork code stays out of upstream files wherever possible, so upstream
merges stay mechanical:

- New code goes in new files.
- When an upstream file must be edited, the edit sits in a marked block
  opened by a `// GodotNext: <hook_name>` comment, where `<hook_name>`
  names the hook (for example the subsystem and what it hooks). Keep the
  block as small as the hook allows, so it can be found, reviewed and
  re-applied by searching for the marker.

Lines changed in upstream files are tracked at each gate with
[`misc/scripts/godotnext_upstream_delta.py`](../../misc/scripts/godotnext_upstream_delta.py),
which reports the changed-line totals, the largest files, and every
`GodotNext:` marker in the changed upstream files:

```sh
python3 misc/scripts/godotnext_upstream_delta.py
python3 misc/scripts/godotnext_upstream_delta.py --json
```

Both forms diff `HEAD` against the merge base with `upstream/master` by
default (override with `--base`), keep only paths that already exist at
the merge base (new fork files are not upstream delta), and exit with
code 2 when there is no merge base, for example in a clone without the
graft.

## Upstream pull requests

There are no upstream Godot pull requests during v1 (§19). RenderingDevice
and driver ray-tracing fixes that could be upstreamed later stay as
isolated commits (§15 upstream policy), so they can be extracted without
untangling them from NextGI/NextGeo work.
