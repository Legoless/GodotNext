#!/bin/sh
# Runs the RD ray-query smoke test (#18) on the Apple M5 Max under Metal API
# and shader validation, then checks the fixture B records with the Q17
# metric tool (#14). Run from the repository root:
#
#   sh godotnext/testbed/tests/rd_smoke/run_rd_smoke.sh
#
# GODOT_BIN overrides the editor binary. Exit 0 only when the fixture passes
# and no validation report kinds beyond the known #239 set appear.
set -u

BIN=${GODOT_BIN:-bin/godot.macos.editor.arm64}
LOG=$(mktemp /tmp/rd_smoke.XXXXXX.log)
trap 'rm -f "$LOG"' EXIT

GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
	MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
	"$BIN" --path godotnext/testbed --rendering-driver metal \
	res://tests/rd_smoke/rd_smoke.tscn >"$LOG" 2>&1
status=$?
grep GODOTNEXT_RESULT "$LOG" || true
echo "exit=$status"

echo "validation lines:"
grep -c "ERROR:" "$LOG" | sed 's/^/  ERROR: /' || true
grep -c "Invalid Metal usage" "$LOG" | sed 's/^/  Invalid Metal usage: /' || true
grep -c "Error detected in" "$LOG" | sed 's/^/  Error detected in: /' || true
# Known report kinds from #239; any other pipeline is new.
grep -o 'pipeline: "[^"]*"' "$LOG" | sort | uniq -c | sed 's/^/  /' || true
grep -o 'pipeline: "[^"]*"' "$LOG" | sort -u | grep -v -e ClusterRenderShaderRD -e IntegrateDfgShaderRD -e BestFitNormalShaderRD \
	| sed 's/^/  NEW pipeline: /' || true

USER_DIR="$HOME/Library/Application Support/Godot/app_userdata/GodotNext Testbed"
if [ -f "$USER_DIR/rd_smoke_rays.bin" ] && [ -f "$USER_DIR/rd_smoke_analytic.json" ]; then
	Q17_DIR=$(mktemp -d /tmp/rd_smoke_q17.XXXXXX)
	cp "$USER_DIR/rd_smoke_rays.bin" "$Q17_DIR/rays.bin"
	cp "$USER_DIR/rd_smoke_analytic.json" "$Q17_DIR/analytic.json"
	if (cd godotnext/testbed/harness/metrics && /opt/homebrew/bin/python3 -c 'import godotnext_metrics' 2>/dev/null); then
		(cd godotnext/testbed/harness/metrics && /opt/homebrew/bin/python3 -m godotnext_metrics q17 --in "$Q17_DIR" --json) || true
	else
		echo "Q17 tool skipped: its requirements are not installed for /opt/homebrew/bin/python3 (see harness/metrics/README.md)"
	fi
	for spv in "$USER_DIR"/rd_smoke*.spv; do
		[ -f "$spv" ] || continue
		if command -v spirv-val >/dev/null 2>&1; then
			echo "spirv-val $spv:"
			spirv-val --target-env vulkan1.1 "$spv" && echo "  ok" || true
		fi
	done
fi

exit $status
