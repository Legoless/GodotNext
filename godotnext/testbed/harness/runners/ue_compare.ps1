# Interleaved GodotNext/UE comparison set on the RTX 4080 SUPER machine (#229, plan §23.4).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File ue_compare.ps1 -Config <config.json> -Out <dir>
#            -UeProject <project.uproject> [-UeExe <UnrealEditor.exe>] [-UeCsvDir <dir>] [-Python <python.exe>]
#            [-Godot <exe>] [-Runs N] [-Kind timing|run] [-NoWarmup] [-Judge] [-BusyWaitMinutes N] [-BusyRetrySeconds N]
#
# 1. python -m godotnext_metrics.ue_compare plan writes <Out>\plan.json and the job files from the
#    comparison config (godotnext/testbed/ue_compare/<set>.json, relative to the repository root or
#    absolute).
# 2. Each job runs through win_session1.ps1 in the plan's order: one unmeasured warm-up launch of
#    each engine, then GodotNext, UE, GodotNext, UE ... The UE side is the installed editor in game
#    mode on the comparison project (plan v3.9.3); its warm-up launch fills the derived-data and PSO
#    caches. The jobs set exclusive_gpu, so a job does not start while another engine uses the GPU:
#    on exit 6 the job is tried again every -BusyRetrySeconds (default 5; other projects' jobs can
#    leave short gaps) for up to -BusyWaitMinutes (default 20), then the set stops. <Out>\results.json records each exit code as the set goes. The set stops at the
#    first failed job.
# 3. python -m godotnext_metrics.ue_report writes <Out>\report\ue_compare_report.json and .md: a
#    dry run (numbers, no verdict, not a measurement) unless -Judge, which is for PQ.
#
# -Kind run launches without clock locks or the power-plan check, for a functional dry run while
# the machine is shared; timing runs lock the clocks as win_session1.ps1 does. The Python
# interpreter needs harness/metrics/requirements.txt: -Python, or GODOTNEXT_UE_PYTHON in the
# git-ignored local\ue_bench.env of this checkout. -UeExe is the UE 5.9 install's
# ...\Engine\Binaries\Win64\UnrealEditor.exe, or GODOTNEXT_UE_EDITOR in that file. -UeProject is the
# port's .uproject in the private UE comparison storage. -UeCsvDir defaults to the per-user folder
# the editor writes CSV profiles to in game mode (%LOCALAPPDATA%\UnrealEngine\5.9\Saved\Profiling\CSV).
# <Out> holds UE captures and logs: keep it in that storage and commit none of it.
#
# Exit codes: 0 when every job ran and the report was written, 2 for bad arguments or a refused
# plan or report, otherwise the failed job's win_session1.ps1 exit code.

param(
	[Parameter(Mandatory = $true)][string]$Config,
	[Parameter(Mandatory = $true)][string]$Out,
	[Parameter(Mandatory = $true)][string]$UeProject,
	[string]$UeExe = '',
	[string]$UeCsvDir = '',
	[string]$Python = '',
	[string]$Godot = '',
	[int]$Runs = 0,
	[ValidateSet('timing', 'run')][string]$Kind = 'timing',
	[switch]$NoWarmup,
	[switch]$Judge,
	[double]$BusyWaitMinutes = 20,
	[int]$BusyRetrySeconds = 5
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$MetricsDir = Join-Path $RepoRoot 'godotnext\testbed\harness\metrics'
$Runner = Join-Path $PSScriptRoot 'win_session1.ps1'

function Fail([int]$Code, [string]$Message) {
	[Console]::Error.WriteLine("ue_compare.ps1: $Message")
	exit $Code
}

# UTF-8 without a byte-order mark, as win_session1.ps1 writes JSON.
function Write-Utf8([string]$Path, [string]$Text) {
	[System.IO.File]::WriteAllText($Path, $Text + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Read-LocalSetting([string]$Name) {
	$file = Join-Path $RepoRoot 'local\ue_bench.env'
	if (-not (Test-Path -LiteralPath $file)) {
		return ''
	}
	foreach ($line in Get-Content -LiteralPath $file) {
		if ($line -match "^\s*$Name\s*=\s*(.*?)\s*$") {
			return $Matches[1].Trim('"')
		}
	}
	return ''
}

# Python's output goes to the host, so the function returns only the exit code. A child's stderr
# must not stop this script: Windows PowerShell 5.1 turns redirected native stderr into errors.
function Invoke-Metrics([string[]]$Arguments) {
	Push-Location -LiteralPath $MetricsDir
	$ErrorActionPreference = 'Continue'
	try {
		& $Python @Arguments | Out-Host
		return $LASTEXITCODE
	} finally {
		Pop-Location
	}
}

if (-not $Python) {
	$Python = Read-LocalSetting 'GODOTNEXT_UE_PYTHON'
}
if (-not $Python) {
	Fail 2 'name the Python interpreter with -Python or GODOTNEXT_UE_PYTHON in local\ue_bench.env'
}
if (-not $UeExe) {
	$UeExe = Read-LocalSetting 'GODOTNEXT_UE_EDITOR'
}
if (-not $UeExe) {
	Fail 2 'name the installed UnrealEditor.exe with -UeExe or GODOTNEXT_UE_EDITOR in local\ue_bench.env'
}
if (-not $UeCsvDir) {
	$UeCsvDir = Join-Path $env:LOCALAPPDATA 'UnrealEngine\5.9\Saved\Profiling\CSV'
}
if (-not [System.IO.Path]::IsPathRooted($Out)) {
	Fail 2 '-Out must be an absolute directory'
}
if (-not [System.IO.Path]::IsPathRooted($Config)) {
	$Config = Join-Path $RepoRoot $Config
}
if (-not (Test-Path -LiteralPath $Config -PathType Leaf)) {
	Fail 2 "comparison config '$Config' not found"
}
$env:PYTHONUTF8 = '1'

# ---------- the plan
New-Item -ItemType Directory -Force $Out | Out-Null
$planArgs = @('-m', 'godotnext_metrics.ue_compare', 'plan', '--config', $Config, '--out', $Out, '--ue-exe', $UeExe, '--ue-project', $UeProject, '--ue-csv-dir', $UeCsvDir, '--kind', $Kind)
if ($Godot) {
	$planArgs += @('--godot', $Godot)
}
if ($Runs -gt 0) {
	$planArgs += @('--runs', [string]$Runs)
}
if ($NoWarmup) {
	$planArgs += '--no-warmup'
}
$planCode = Invoke-Metrics $planArgs
if ($planCode -ne 0) {
	Fail 2 "the plan was refused (exit $planCode)"
}
$plan = Get-Content -Raw -Encoding UTF8 (Join-Path $Out 'plan.json') | ConvertFrom-Json

# ---------- the interleaved runs
$resultsPath = Join-Path $Out 'results.json'
$results = New-Object System.Collections.ArrayList
$failed = $null
foreach ($entry in @($plan.jobs)) {
	$startedAt = (Get-Date).ToString('o')
	Write-Output "ue_compare: $($entry.name): $($entry.role) $($entry.engine) $($entry.index)"
	# The machine is shared: while another engine uses the GPU the runner exits 6; wait and retry.
	$busyDeadline = (Get-Date).AddMinutes($BusyWaitMinutes)
	$busyRetries = 0
	while ($true) {
		$ErrorActionPreference = 'Continue'
		& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Runner -JobFile $entry.job
		$code = $LASTEXITCODE
		$ErrorActionPreference = 'Stop'
		if ($code -ne 6 -or (Get-Date) -gt $busyDeadline) {
			break
		}
		$busyRetries++
		Start-Sleep -Seconds $BusyRetrySeconds
	}
	[void]$results.Add([ordered]@{
			name = $entry.name
			role = $entry.role
			engine = $entry.engine
			index = $entry.index
			exit_code = $code
			busy_retries = $busyRetries
			started = $startedAt
			finished = (Get-Date).ToString('o')
		})
	Write-Utf8 $resultsPath (ConvertTo-Json -InputObject @($results.ToArray()) -Depth 4)
	if ($code -ne 0) {
		$failed = $entry
		break
	}
}
if ($null -ne $failed) {
	Fail $code "job $($failed.name) failed with exit code $code; results.json lists the runs so far"
}

# ---------- the report
$reportDir = Join-Path $Out 'report'
$reportArgs = @('-m', 'godotnext_metrics.ue_report', '--compare', $Out, '--out', $reportDir)
if ($Judge) {
	$reportArgs += '--judge'
}
$reportCode = Invoke-Metrics $reportArgs
if ($reportCode -ne 0) {
	Fail 2 "the report was refused (exit $reportCode)"
}
Get-Content -Raw -Encoding UTF8 (Join-Path $reportDir 'ue_compare_report.md') | Write-Output
exit 0
