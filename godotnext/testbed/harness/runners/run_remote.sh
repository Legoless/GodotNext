#!/usr/bin/env bash
# Runs harness jobs on the RTX 4080 SUPER machine from the M5 Max (#237).
#
# Usage:
#   run_remote.sh job <job.json> <local output directory>
#   run_remote.sh selftest [run_selftest.ps1 arguments...] [--fetch <local directory>]
#
# job: copies the job file over, runs runners\win_session1.ps1 on it there (Session 1, with the
# --gpu-index fleet.md gives for the job's device), copies the job's output directory back, and
# exits with the job's exit code. Give the job's "out" relative to the repository root, for
# example "godotnext/testbed/captures/remote/<name>".
# selftest: runs harness\selftest\run_selftest.ps1 there with the given arguments; --fetch copies
# godotnext/testbed/captures/<label> back afterwards (label: -Label, else the -Scene file name,
# else selftest).
#
# The machine and its checkout come from the environment, or from the git-ignored local/fleet.env:
#   GODOTNEXT_RTX_HOST  the SSH host alias of the RTX 4080 SUPER machine (your ~/.ssh/config)
#   GODOTNEXT_RTX_REPO  the repository root there, for example C:/Users/<user>/Projects/GodotNext
#   GODOTNEXT_RTX_SSH_OPTS  optional extra ssh/scp options, for example "-i <key>"
# The checkout there must be at the commit you mean to test; this script does not sync code.
# The machine's OpenSSH default shell must be PowerShell (it is on the RTX 4080 SUPER machine).

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../../.." && pwd)
if [[ -f "$repo_root/local/fleet.env" ]]; then
	# shellcheck disable=SC1091
	source "$repo_root/local/fleet.env"
fi
host=${GODOTNEXT_RTX_HOST:-}
remote_repo=${GODOTNEXT_RTX_REPO:-}

usage() {
	sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

if [[ -z "$host" || -z "$remote_repo" ]]; then
	echo "run_remote.sh: set GODOTNEXT_RTX_HOST and GODOTNEXT_RTX_REPO (or write them to local/fleet.env)" >&2
	exit 2
fi
if [[ $# -lt 1 ]]; then
	usage >&2
	exit 2
fi
remote_repo=${remote_repo//\\//}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)
# GODOTNEXT_RTX_SSH_OPTS: extra ssh and scp options, split on spaces (for example "-i <key>" for a
# machine reached without a ~/.ssh/config alias, such as the cloud Windows VM).
if [[ -n "${GODOTNEXT_RTX_SSH_OPTS:-}" ]]; then
	read -r -a extra_ssh_opts <<<"$GODOTNEXT_RTX_SSH_OPTS"
	ssh_opts+=("${extra_ssh_opts[@]}")
fi

# remote_command <one line of PowerShell>: runs it there through stdin and returns its exit code.
# The machine's OpenSSH default shell is PowerShell, which ends a session with 1 whenever a child
# process failed; the trailing "exit $LASTEXITCODE" passes the child's code through instead.
remote_command() {
	# shellcheck disable=SC2016
	ssh "${ssh_opts[@]}" "$host" 'powershell -NoProfile -ExecutionPolicy Bypass -Command -; exit $LASTEXITCODE' <<<"$1"
}

# ps_quote <value>: a PowerShell single-quoted string.
ps_quote() {
	local value=${1//\'/\'\'}
	printf "'%s'" "$value"
}

# remote_ps <script path relative to the repository> <arguments...>
remote_ps() {
	local line="& $(ps_quote "$remote_repo/$1")"
	shift
	local arg
	for arg in "$@"; do
		# A quoted '-Name' would bind as a value, so parameter names go bare.
		if [[ "$arg" =~ ^-[A-Za-z]+$ ]]; then
			line+=" $arg"
		else
			line+=" $(ps_quote "$arg")"
		fi
	done
	# exit N is lost when PowerShell reads its commands from stdin; Environment.Exit is not.
	remote_command "$line; [Environment]::Exit(\$LASTEXITCODE)"
}

mode=$1
shift
case "$mode" in
job)
	if [[ $# -ne 2 ]]; then
		usage >&2
		exit 2
	fi
	job=$1
	dest=$2
	if [[ ! -f "$job" ]]; then
		echo "run_remote.sh: job file '$job' not found" >&2
		exit 2
	fi
	out=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["out"])' "$job")
	case "$out" in
	/* | [A-Za-z]:*)
		echo "run_remote.sh: give the job's out relative to the repository root, so it can be copied back" >&2
		exit 2
		;;
	esac
	name=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("name", "job"))' "$job")
	remote_job="godotnext/testbed/captures/remote_jobs/$name.json"
	remote_command "New-Item -ItemType Directory -Force $(ps_quote "$remote_repo/godotnext/testbed/captures/remote_jobs") | Out-Null"
	scp -q "${ssh_opts[@]}" "$job" "$host:$remote_repo/$remote_job"
	set +e
	remote_ps godotnext/testbed/harness/runners/win_session1.ps1 -JobFile "$remote_repo/$remote_job"
	status=$?
	set -e
	mkdir -p "$dest"
	if ! scp -q -r "${ssh_opts[@]}" "$host:$remote_repo/$out" "$dest/"; then
		echo "run_remote.sh: could not copy $out back" >&2
		[[ $status -eq 0 ]] && status=1
	fi
	exit "$status"
	;;
selftest)
	fetch=""
	args=()
	label=""
	scene=""
	while [[ $# -gt 0 ]]; do
		case "$1" in
		--fetch)
			fetch=$2
			shift 2
			;;
		-Label)
			label=$2
			args+=("$1" "$2")
			shift 2
			;;
		-Scene)
			scene=$2
			args+=("$1" "$2")
			shift 2
			;;
		*)
			args+=("$1")
			shift
			;;
		esac
	done
	if [[ -z "$label" ]]; then
		if [[ -n "$scene" ]]; then
			label=$(basename "$scene" .tscn)
		else
			label=selftest
		fi
	fi
	set +e
	remote_ps godotnext/testbed/harness/selftest/run_selftest.ps1 "${args[@]+"${args[@]}"}"
	status=$?
	set -e
	if [[ -n "$fetch" ]]; then
		mkdir -p "$fetch"
		scp -q -r "${ssh_opts[@]}" "$host:$remote_repo/godotnext/testbed/captures/$label" "$fetch/" || status=1
	fi
	exit "$status"
	;;
--help | -h)
	usage
	;;
*)
	usage >&2
	exit 2
	;;
esac
