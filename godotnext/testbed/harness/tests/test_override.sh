#!/usr/bin/env bash
# Per-scene override.cfg handling (#240). No GPU and no Godot binary needed:
# the helper is sourced directly, and linux_vm.sh runs against a fake godot.

set -uo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
harness_dir=$(cd "$script_dir/.." && pwd)
testbed=$(cd "$harness_dir/.." && pwd)
root_override="$testbed/override.cfg"
scene_override="$testbed/scenes/feature_parity_room/override.cfg"
scene_res="res://scenes/feature_parity_room/feature_parity_room.tscn"

failures=0
check() {
	if [[ "$1" == "0" ]]; then
		return
	fi
	failures=$((failures + 1))
	echo "FAIL: $2" >&2
}

if [[ -e "$root_override" ]]; then
	echo "test_override.sh: $root_override already exists; not touching it" >&2
	exit 2
fi
cleanup() {
	rm -f "$root_override"
	rm -rf "$work"
}
work=$(mktemp -d)
trap cleanup EXIT

# shellcheck source=../runners/scene_override.sh
source "$harness_dir/runners/scene_override.sh"

found=$(scene_override_find_arg scene --mode=capture --scene="$scene_res" --out=/tmp/x)
[[ "$found" == "$scene_res" ]]
check $? "--scene=value is found"
found=$(scene_override_find_arg scene --mode capture --scene "$scene_res" --out /tmp/x)
[[ "$found" == "$scene_res" ]]
check $? "--scene value is found"
found=$(scene_override_find_arg scene --mode=capture --out=/tmp/x)
[[ -z "$found" ]]
check $? "missing --scene is empty"

scene_override_apply "$testbed" "$scene_res"
check $? "apply succeeds for the Feature Parity Room"
[[ "$SCENE_OVERRIDE_APPLIED" == "1" ]]
check $? "apply reports applied"
[[ "$SCENE_OVERRIDE_SOURCE" == "res://scenes/feature_parity_room/override.cfg" ]]
check $? "apply reports the res:// source"
[[ "${#SCENE_OVERRIDE_SHA256}" == "64" ]]
check $? "apply reports a sha256"
cmp -s "$root_override" "$scene_override"
check $? "root override.cfg matches the scene folder copy"
scene_override_remove
[[ ! -e "$root_override" ]]
check $? "remove deletes the root override.cfg"

scene_override_apply "$testbed" "res://harness/selftest/selftest.tscn"
check $? "apply succeeds when the scene has no override"
[[ "$SCENE_OVERRIDE_APPLIED" == "0" && -z "$SCENE_OVERRIDE_SOURCE" && -z "$SCENE_OVERRIDE_SHA256" ]]
check $? "no override records empty source and sha256"
[[ ! -e "$root_override" ]]
check $? "no override writes no root file"

printf '[test]\n\nkeep=me\n' >"$root_override"
if scene_override_apply "$testbed" "$scene_res" 2>"$work/refuse.log"; then
	check 1 "apply refuses to overwrite an existing root override.cfg"
else
	check 0 "apply refuses to overwrite an existing root override.cfg"
fi
grep -q "keep=me" "$root_override"
check $? "refused apply leaves the existing file untouched"
rm -f "$root_override"

if scene_override_apply "$testbed" "res://../outside.tscn" 2>"$work/escape.log"; then
	check 1 "a scene path escaping the testbed is rejected"
else
	check 0 "a scene path escaping the testbed is rejected"
fi

fake_godot="$work/fake_godot.sh"
cat >"$fake_godot" <<'EOF'
#!/usr/bin/env bash
if [[ "${EXPECT_OVERRIDE:-0}" == "1" ]]; then
	cmp -s "$TESTBED_DIR/override.cfg" "$TESTBED_DIR/scenes/feature_parity_room/override.cfg" || exit 3
else
	[[ ! -e "$TESTBED_DIR/override.cfg" ]] || exit 3
fi
echo "fake godot ran"
EOF
chmod +x "$fake_godot"

out="$work/out"
TESTBED_DIR="$testbed" EXPECT_OVERRIDE=1 GODOT="$fake_godot" \
	"$harness_dir/runners/linux_vm.sh" functional -- \
	--path "$testbed" -- \
	--mode=capture --scene="$scene_res" --camera=cam_0 --out="$out" \
	--seed=1 --frames=1 >"$work/linux.log" 2>&1
check $? "linux_vm functional run with an override succeeds"
[[ ! -e "$root_override" ]]
check $? "linux_vm leaves no root override.cfg behind"
python3 - "$out/runner_job.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    job = json.load(handle)
record = job.get("override", {})
if record.get("applied") is not True:
    sys.exit(1)
if record.get("source") != "res://scenes/feature_parity_room/override.cfg":
    sys.exit(1)
if len(record.get("sha256", "")) != 64:
    sys.exit(1)
PY
check $? "runner job records the applied override"

if [[ $failures -eq 0 ]]; then
	echo "test_override: ok"
	exit 0
fi
echo "test_override: $failures failure(s)" >&2
exit 1
