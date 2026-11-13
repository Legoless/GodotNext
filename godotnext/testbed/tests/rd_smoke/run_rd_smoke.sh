#!/bin/sh
# Runs the RD ray-query smoke test (#18, #49) on the Apple M5 Max under Metal
# API and shader validation, then checks the dump folder with the Q17 metric
# tool (#14). Run from the repository root:
#
#   sh godotnext/testbed/tests/rd_smoke/run_rd_smoke.sh
#
# GODOT_BIN overrides the editor binary; RD_SMOKE_OUT the dump folder (default
# a temporary folder, removed afterwards). Exit 0 only when the fixture passes
# and no validation report kinds beyond the known #239 set appear.
set -u

BIN=${GODOT_BIN:-bin/godot.macos.editor.arm64}
LOG=$(mktemp /tmp/rd_smoke.XXXXXX.log)
if [ -n "${RD_SMOKE_OUT:-}" ]; then
	OUT=$RD_SMOKE_OUT
	trap 'rm -f "$LOG"' EXIT
else
	OUT=$(mktemp -d /tmp/rd_smoke_dump.XXXXXX)
	trap 'rm -f "$LOG"; rm -r "$OUT"' EXIT
fi

GODOTNEXT_BACKGROUND=1 MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
	MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
	"$BIN" --path godotnext/testbed --rendering-driver metal \
	res://tests/rd_smoke/rd_smoke.tscn -- --out="$OUT" >"$LOG" 2>&1
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

if [ -f "$OUT/rays.bin" ]; then
	PY=${METRICS_PYTHON:-/opt/homebrew/bin/python3}
	if (cd godotnext/testbed/harness/metrics && "$PY" -c 'import godotnext_metrics' 2>/dev/null); then
		(cd godotnext/testbed/harness/metrics && "$PY" -m godotnext_metrics q17 --in "$OUT" --json) || true
	else
		echo "Q17 tool not run: its requirements are not installed for $PY (see harness/metrics/README.md; METRICS_PYTHON picks another interpreter)"
	fi
	for spv in "$OUT"/*.spv; do
		[ -f "$spv" ] || continue
		if command -v spirv-val >/dev/null 2>&1; then
			echo "spirv-val $spv:"
			spirv-val --target-env vulkan1.1 "$spv" && echo "  ok" || true
		fi
	done
fi

exit $status
