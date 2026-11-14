#!/usr/bin/env bash
# run_fog_check.sh passes its own scene and expected tier (#269). No GPU and no Godot binary
# needed: the runner runs against a fake godot that records its arguments and the override.cfg it
# saw. Before #269 a call without the scene rendered the project's main scene, which never quits.

set -uo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
testbed=$(cd "$script_dir/../.." && pwd)
runner="$script_dir/run_fog_check.sh"
root_override="$testbed/override.cfg"
scene=res://tests/wrc/fog_check.tscn

failures=0
check() {
	if [[ "$1" == "0" ]]; then
		return
	fi
	failures=$((failures + 1))
	echo "FAIL: $2" >&2
}

if [[ -e "$root_override" ]]; then
	echo "test_run_fog_check.sh: $root_override already exists; not touching it" >&2
	exit 2
fi
work=$(mktemp -d)
cleanup() {
	rm -f "$root_override"
	rm -rf "$work"
}
trap cleanup EXIT

fake_godot="$work/fake_godot.sh"
cat >"$fake_godot" <<'EOF'
#!/usr/bin/env bash
# Records one argument per line and a copy of the override, then acts as FAKE_MODE says.
printf '%s\n' "$@" >"$FAKE_DIR/args"
cp "$TESTBED_DIR/override.cfg" "$FAKE_DIR/override.cfg" 2>/dev/null || true
case "${FAKE_MODE:-pass}" in
error) echo "ERROR: fake validation line" ;;
fail) exit 3 ;;
esac
echo 'GODOTNEXT_RESULT {"pass":true}'
EOF
chmod +x "$fake_godot"

# run_case <name> <runner arguments...>: runs the runner with the fake godot, in a fresh folder.
run_case() {
	local name=$1
	shift
	case_dir="$work/$name"
	mkdir -p "$case_dir"
	FAKE_DIR="$case_dir" TESTBED_DIR="$testbed" "$runner" "$@" >"$case_dir/log" 2>&1
	case_status=$?
}

# expect_tail <file> <lines...>: the file ends with exactly these lines.
expect_tail() {
	local file=$1
	shift
	local expected
	expected=$(printf '%s\n' "$@")
	[[ "$(tail -n $# "$file" 2>/dev/null)" == "$expected" ]]
}

run_case off --hwrt=off -- "$fake_godot" --path "$testbed" --audio-driver Dummy
check $case_status "--hwrt=off exits 0"
expect_tail "$case_dir/args" --audio-driver Dummy "$scene" -- --expected-tier=SWRT
check $? "--hwrt=off passes the scene and --expected-tier=SWRT after the caller's arguments"
grep -qx 'global_illumination/nextgi/enabled=true' "$case_dir/override.cfg"
check $? "--hwrt=off enables NextGI in override.cfg"
grep -qx 'global_illumination/nextgi/hardware_ray_tracing=1' "$case_dir/override.cfg"
check $? "--hwrt=off writes hardware_ray_tracing=1"
[[ ! -e "$root_override" ]]
check $? "--hwrt=off leaves no override.cfg behind"

run_case auto --hwrt=auto -- "$fake_godot" --path "$testbed"
check $case_status "--hwrt=auto exits 0"
expect_tail "$case_dir/args" "$scene" -- --expected-tier=HWRT
check $? "--hwrt=auto passes the scene and --expected-tier=HWRT"
grep -qx 'global_illumination/nextgi/hardware_ray_tracing=0' "$case_dir/override.cfg"
check $? "--hwrt=auto writes hardware_ray_tracing=0"

run_case default -- "$fake_godot" --path "$testbed"
check $case_status "the default tier exits 0"
expect_tail "$case_dir/args" "$scene" -- --expected-tier=HWRT
check $? "the default tier is Auto and expects HWRT"

run_case force --hwrt=force --quality=2 -- "$fake_godot" --path "$testbed"
check $case_status "--hwrt=force exits 0"
expect_tail "$case_dir/args" "$scene" -- --expected-tier=HWRT
check $? "--hwrt=force expects HWRT"
grep -qx 'global_illumination/nextgi/hardware_ray_tracing=2' "$case_dir/override.cfg"
check $? "--hwrt=force writes hardware_ray_tracing=2"
grep -qx 'global_illumination/nextgi/quality=2' "$case_dir/override.cfg"
check $? "--quality=2 writes quality=2"

run_case scene --hwrt=off -- "$fake_godot" --path "$testbed" "$scene"
[[ $case_status -eq 2 && ! -e "$case_dir/args" ]]
check $? "a scene among the Godot arguments exits 2 before Godot runs"
[[ ! -e "$root_override" ]]
check $? "a refused scene leaves no override.cfg behind"

run_case separator --hwrt=off -- "$fake_godot" --path "$testbed" -- --expected-tier=SWRT
[[ $case_status -eq 2 && ! -e "$case_dir/args" ]]
check $? "a '--' among the Godot arguments exits 2 before Godot runs"

run_case no_command --hwrt=off --
[[ $case_status -eq 2 ]]
check $? "a missing Godot command exits 2"

FAKE_MODE=error run_case error --hwrt=off -- "$fake_godot" --path "$testbed"
[[ $case_status -eq 1 ]]
check $? "an ERROR: line exits 1"

FAKE_MODE=fail run_case fail --hwrt=off -- "$fake_godot" --path "$testbed"
[[ $case_status -eq 3 ]]
check $? "Godot's failing status passes through"
[[ ! -e "$root_override" ]]
check $? "a failed run leaves no override.cfg behind"

if [[ $failures -eq 0 ]]; then
	echo "test_run_fog_check: ok"
	exit 0
fi
echo "test_run_fog_check: $failures failure(s)" >&2
exit 1
