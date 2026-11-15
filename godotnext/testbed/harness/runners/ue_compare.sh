#!/usr/bin/env bash
# Runs an interleaved GodotNext/UE comparison set on the RTX 4080 SUPER machine from the M5 Max (#229).
#
# Usage:
#   ue_compare.sh <config> <output directory there> <comparison .uproject there> [ue_compare.ps1 arguments...]
#
# <config> is relative to the repository root, for example godotnext/testbed/ue_compare/swrt_kitbash_high.json.
# This runs harness\runners\ue_compare.ps1 in the comparison checkout there, prints its output
# (the report's Markdown last) and passes its exit code through. Nothing is copied back: the output
# directory holds UE captures and logs and stays in the private UE comparison storage on that
# machine. The UE side is the installed editor in game mode (plan v3.9.3); its path comes from
# GODOTNEXT_UE_EDITOR in the comparison checkout's local/ue_bench.env there, or -UeExe. Extra
# arguments go to ue_compare.ps1, for example -Kind run -Runs 1 for a dry run.
#
# The machine and the checkout come from the environment, or from the git-ignored local/fleet.env:
#   GODOTNEXT_RTX_HOST      the SSH host alias of the RTX 4080 SUPER machine (your ~/.ssh/config)
#   GODOTNEXT_UE_REPO       the comparison checkout there, at the commit you mean to run; not the
#                           checkout GODOTNEXT_RTX_REPO names, which landing checks use
#   GODOTNEXT_RTX_SSH_OPTS  optional extra ssh options, for example "-i <key>"
# The machine's OpenSSH default shell must be PowerShell (it is on the RTX 4080 SUPER machine).

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../../.." && pwd)
if [[ -f "$repo_root/local/fleet.env" ]]; then
	# shellcheck disable=SC1091
	source "$repo_root/local/fleet.env"
fi
host=${GODOTNEXT_RTX_HOST:-}
remote_repo=${GODOTNEXT_UE_REPO:-}

usage() {
	sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

if [[ $# -lt 3 ]]; then
	usage >&2
	exit 2
fi
if [[ -z "$host" || -z "$remote_repo" ]]; then
	echo "ue_compare.sh: set GODOTNEXT_RTX_HOST and GODOTNEXT_UE_REPO (or write them to local/fleet.env)" >&2
	exit 2
fi
remote_repo=${remote_repo//\\//}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)
if [[ -n "${GODOTNEXT_RTX_SSH_OPTS:-}" ]]; then
	read -r -a extra_ssh_opts <<<"$GODOTNEXT_RTX_SSH_OPTS"
	ssh_opts+=("${extra_ssh_opts[@]}")
fi

# ps_quote <value>: a PowerShell single-quoted string.
ps_quote() {
	local value=${1//\'/\'\'}
	printf "'%s'" "$value"
}

config=$1
out=$2
ue_project=$3
shift 3
line="& $(ps_quote "$remote_repo/godotnext/testbed/harness/runners/ue_compare.ps1")"
line+=" -Config $(ps_quote "$config") -Out $(ps_quote "$out") -UeProject $(ps_quote "$ue_project")"
for arg in "$@"; do
	# A quoted '-Name' would bind as a value, so parameter names go bare.
	if [[ "$arg" =~ ^-[A-Za-z]+$ ]]; then
		line+=" $arg"
	else
		line+=" $(ps_quote "$arg")"
	fi
done
# exit N is lost when PowerShell reads its commands from stdin; Environment.Exit is not.
# shellcheck disable=SC2016
ssh "${ssh_opts[@]}" "$host" 'powershell -NoProfile -ExecutionPolicy Bypass -Command -; exit $LASTEXITCODE' \
	<<<"$line; [Environment]::Exit(\$LASTEXITCODE)"
