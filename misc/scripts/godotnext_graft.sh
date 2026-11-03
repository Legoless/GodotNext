#!/usr/bin/env bash
# GodotNext: create and verify the upstream history graft (plan §12, G0-5).
#
# Commit 7cfeb47 is a single-commit import of godotengine/godot@95c0fa5.
# This script fetches upstream, grafts 7cfeb47 onto 95c0fa5 with a replace
# ref, verifies the merge base, and prints the publish and fresh-clone
# steps. It never pushes unless --push is given; publishing refs/replace/*
# is the lead's step. See docs/godotnext/fork-maintenance.md.
#
# Usage:
#   misc/scripts/godotnext_graft.sh [--push] [UPSTREAM_COMMIT]
#
# UPSTREAM_COMMIT defaults to 95c0fa5 (or $GODOTNEXT_UPSTREAM_COMMIT). Use
# the full 40-character SHA if the short one is unreachable so the script
# can fetch it by SHA.

set -euo pipefail

IMPORT_COMMIT="7cfeb473cc58d124c56b302fb313541471d96163"
UPSTREAM_URL="https://github.com/godotengine/godot.git"
UPSTREAM_COMMIT="${GODOTNEXT_UPSTREAM_COMMIT:-95c0fa5}"
PUSH=0

fail() {
	echo "error: $*" >&2
	exit 1
}

for arg in "$@"; do
	case "$arg" in
	--push) PUSH=1 ;;
	-h | --help)
		sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	--*) fail "unknown option: $arg (see --help)" ;;
	*) UPSTREAM_COMMIT="$arg" ;;
	esac
done

git rev-parse --git-dir >/dev/null 2>&1 || fail "run this inside a GodotNext clone"
git cat-file -e "${IMPORT_COMMIT}^{commit}" 2>/dev/null ||
	fail "import commit $IMPORT_COMMIT not found; run this in a full GodotNext clone"

if git remote get-url upstream >/dev/null 2>&1; then
	current_url="$(git remote get-url upstream)"
	if [ "$current_url" != "$UPSTREAM_URL" ]; then
		echo "warning: remote 'upstream' already points to $current_url" >&2
		echo "warning: expected $UPSTREAM_URL; continuing with the existing remote" >&2
	fi
else
	echo "Adding remote 'upstream' ($UPSTREAM_URL)"
	git remote add upstream "$UPSTREAM_URL"
fi

echo "Fetching upstream master..."
git fetch upstream master

if ! upstream_full="$(git rev-parse -q --verify "${UPSTREAM_COMMIT}^{commit}" 2>/dev/null)"; then
	if [ "${#UPSTREAM_COMMIT}" -eq 40 ] && [[ "$UPSTREAM_COMMIT" =~ ^[0-9a-f]+$ ]]; then
		echo "Commit not reachable from fetched refs; fetching it by SHA..."
		git fetch upstream "$UPSTREAM_COMMIT"
		upstream_full="$(git rev-parse -q --verify "${UPSTREAM_COMMIT}^{commit}")" ||
			fail "could not fetch $UPSTREAM_COMMIT from upstream"
	else
		fail "upstream commit '$UPSTREAM_COMMIT' not found after fetch; pass its full 40-character SHA"
	fi
fi
echo "Upstream commit: $upstream_full"

import_tree="$(git rev-parse "${IMPORT_COMMIT}^{tree}")"
upstream_tree="$(git rev-parse "${upstream_full}^{tree}")"
echo "Import tree   ($IMPORT_COMMIT): $import_tree"
echo "Upstream tree ($upstream_full): $upstream_tree"
if [ "$import_tree" != "$upstream_tree" ]; then
	echo "warning: the import tree differs from the upstream tree." >&2
	echo "Record this diff --stat in docs/godotnext/fork-maintenance.md" >&2
	echo "('Import tree comparison') before publishing the graft:" >&2
	git diff --stat "$upstream_full" "$IMPORT_COMMIT" >&2
fi

replace_ref="refs/replace/$IMPORT_COMMIT"
if existing="$(git rev-parse -q --verify "$replace_ref" 2>/dev/null)"; then
	existing_parent="$(git rev-parse "${existing}^" 2>/dev/null || true)"
	if [ "$existing_parent" = "$upstream_full" ]; then
		echo "Graft already exists and already points at $upstream_full; keeping it."
	else
		fail "a replace ref for $IMPORT_COMMIT exists but is not a graft onto $upstream_full; inspect with 'git replace -l' and 'git cat-file -p $replace_ref', remove it with 'git replace -d $IMPORT_COMMIT' only if you are sure, then re-run"
	fi
else
	echo "Creating graft: $IMPORT_COMMIT -> $upstream_full"
	git replace --graft "$IMPORT_COMMIT" "$upstream_full"
fi

merge_base="$(git merge-base HEAD upstream/master)" ||
	fail "git merge-base HEAD upstream/master failed even with the graft"
[ "$merge_base" = "$upstream_full" ] ||
	fail "merge base is $merge_base, expected $upstream_full"
echo "Verified: git merge-base HEAD upstream/master = $merge_base"

if [ "$PUSH" -eq 1 ]; then
	echo "Publishing the graft..."
	git push origin 'refs/replace/*'
	echo "Published refs on origin:"
	git ls-remote origin 'refs/replace/*'
else
	cat <<EOF
The graft is created locally but not published. The lead publishes it with:

  git push origin 'refs/replace/*'
  git ls-remote origin 'refs/replace/*'
EOF
fi

cat <<'EOF'
Verify from a fresh clone (gate G0-5):

  git clone <origin URL> godotnext-fresh
  cd godotnext-fresh
  git remote add upstream https://github.com/godotengine/godot.git
  git fetch upstream master
  git fetch origin 'refs/replace/*:refs/replace/*'
  git merge-base HEAD upstream/master   # must print the upstream commit above

Clones that merge upstream can keep the graft with:

  git config --add remote.origin.fetch '+refs/replace/*:refs/replace/*'
EOF
