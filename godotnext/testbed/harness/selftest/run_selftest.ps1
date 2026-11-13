# Self-test driver for the harness on the RTX 4080 SUPER machine (#237): the Windows twin of
# harness/run_selftest.sh.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File run_selftest.ps1
#            [-Mode capture|timing|all] [-Runs N] [-Frames N] [-Warmup N] [-Seed N]
#            [-Scene res://...] [-Camera name] [-Label name] [-Device name] [-Driver vulkan|d3d12]
#
# Default: timing three times (warmup 120, frames 600, clocks locked) and capture twice (warmup 4,
# frames 8). Every launch goes through runners\win_session1.ps1, so it runs in Session 1 with the
# --gpu-index fleet.md gives for -Device. Capture hashes (SHA-256 of every PNG and EXR) must match
# across runs. The timing spread is printed and does not fail the run: the <=2% p95 spread is the
# PQ gate G0-4. Output goes to godotnext\testbed\captures\<label>\.

param(
	[ValidateSet('capture', 'timing', 'all')][string]$Mode = 'all',
	[int]$Runs = 0,
	[int]$Frames = 0,
	[int]$Warmup = -1,
	[int]$Seed = 1,
	[string]$Scene = '',
	[string]$Camera = '',
	[string]$Label = '',
	[string]$Device = 'NVIDIA GeForce RTX 4080 SUPER',
	[ValidateSet('vulkan', 'd3d12')][string]$Driver = 'vulkan'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$Testbed = Join-Path $RepoRoot 'godotnext\testbed'
$Runner = Join-Path $RepoRoot 'godotnext\testbed\harness\runners\win_session1.ps1'

function Fail([int]$Code, [string]$Message) {
	[Console]::Error.WriteLine("run_selftest.ps1: $Message")
	exit $Code
}

$sceneRes = 'res://harness/selftest/selftest.tscn'
$cameraName = 'CaptureCam'
if ($Scene) {
	$sceneRes = $Scene
}
if ($Camera) {
	$cameraName = $Camera
}
if (-not $Label) {
	if ($Scene) {
		$Label = [System.IO.Path]::GetFileNameWithoutExtension($sceneRes)
	} else {
		$Label = 'selftest'
	}
}
if ($Label -notmatch '^[A-Za-z0-9_.-]+$' -or $Label -eq '..' -or $Label -eq '.') {
	Fail 2 '-Label must be one folder name'
}
$capturesRoot = Join-Path $Testbed "captures\$Label"

$captureWarmup = 4
$captureFrames = 8
$timingWarmup = 120
$timingFrames = 600
if ($Warmup -ge 0) {
	$captureWarmup = $Warmup
	$timingWarmup = $Warmup
}
if ($Frames -gt 0) {
	$captureFrames = $Frames
	$timingFrames = $Frames
}
$captureRuns = 2
$timingRuns = 3
if ($Runs -gt 0) {
	$captureRuns = $Runs
	$timingRuns = $Runs
}

# The movie writer's --quit-after counts settle draws as well as the captured window: 900 is the
# specialization budget, 30 the flat streak and 120 margin, as in run_selftest.sh.
function Get-QuitAfter([int]$WarmupValue, [int]$FrameValue) {
	return 900 + 30 + 120 + $WarmupValue + $FrameValue
}

function Get-CaptureList([int]$WarmupValue, [int]$FrameValue) {
	$window = $WarmupValue + $FrameValue
	$slotA = [Math]::Min($WarmupValue, $window - 1)
	$slotB = [Math]::Min($WarmupValue + 3, $window - 1)
	if ($slotA -eq $slotB) {
		return "$slotA"
	}
	return "$slotA,$slotB"
}

function Invoke-Job([string]$JobName, [string]$Kind, [string]$Out, [string[]]$GodotArgs) {
	$job = [ordered]@{
		name = $JobName
		device = $Device
		driver = $Driver
		kind = $Kind
		args = $GodotArgs
		out = $Out
		timeout_minutes = 30
	}
	New-Item -ItemType Directory -Force $Out | Out-Null
	$jobPath = Join-Path $Out 'job.json'
	[System.IO.File]::WriteAllText($jobPath, ($job | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding $false))
	& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Runner -JobFile $jobPath | Out-Null
	$status = $LASTEXITCODE
	if (Test-Path (Join-Path $Testbed 'override.cfg')) {
		Fail 1 'the runner left a root override.cfg behind'
	}
	return $status
}

function Test-Manifest([string]$Out, [bool]$ExpectLocked) {
	$path = Join-Path $Out 'manifest.json'
	if (-not (Test-Path $path)) {
		Fail 1 "manifest.json missing in $Out"
	}
	$manifest = Get-Content -Raw $path | ConvertFrom-Json
	foreach ($key in @('scaling_3d_mode', 'scaling_3d_scale', 'output_size', 'runner')) {
		if ($manifest.PSObject.Properties.Name -notcontains $key) {
			Fail 1 "manifest missing $key"
		}
	}
	$power = $manifest.runner.power
	foreach ($key in @('on_ac', 'powermode', 'powermode_name', 'thermal', 'before', 'after')) {
		if ($power.PSObject.Properties.Name -notcontains $key) {
			Fail 1 "manifest runner.power missing $key"
		}
	}
	$clock = $manifest.runner.clock
	if ([bool]$clock.locked -ne $ExpectLocked) {
		Fail 1 "manifest runner.clock.locked is $($clock.locked), expected $ExpectLocked"
	}
	if ($ExpectLocked -and ($clock.PSObject.Properties.Name -notcontains 'gpu_mhz' -or $clock.PSObject.Properties.Name -notcontains 'memory_mhz')) {
		Fail 1 'manifest runner.clock does not record the locked clocks'
	}
	if ($manifest.PSObject.Properties.Name -contains 'dlss') {
		Fail 1 'self-test manifest unexpectedly included dlss'
	}
	Write-Host "manifest fields: scaling_3d_mode scaling_3d_scale output_size runner.power runner.clock (locked=$ExpectLocked)"
}

function Invoke-Capture([int]$Index) {
	$out = Join-Path $capturesRoot "capture_$Index"
	if (Test-Path $out) {
		Remove-Item -Recurse -Force $out
	}
	$list = Get-CaptureList $captureWarmup $captureFrames
	$quitAfter = Get-QuitAfter $captureWarmup $captureFrames
	$godotArgs = @('--path', $Testbed, '--write-movie', '{out}\frame.png', '--fixed-fps', '60', '--quit-after', "$quitAfter", '--',
		'--mode=capture', "--scene=$sceneRes", "--camera=$cameraName", '--out={out}', "--seed=$Seed",
		"--frames=$captureFrames", "--warmup=$captureWarmup", "--capture-frames=$list")
	$status = Invoke-Job "selftest-capture-$Index" 'capture' $out $godotArgs
	Get-ChildItem $out -Filter '*.wav' -ErrorAction SilentlyContinue | Remove-Item -Force
	if ($status -ne 0) {
		Fail $status "capture run $Index exited $status (log: $out\run.log)"
	}
	if (-not (Select-String -Quiet -Path (Join-Path $out 'run.log') -Pattern 'harness: capture complete')) {
		Fail 1 "capture run $Index did not finish the captured window"
	}
	$pngs = @(Get-ChildItem $out -Filter '*.png').Count
	$exrs = @(Get-ChildItem $out -Filter '*.exr').Count
	$expected = $captureWarmup + $captureFrames
	if ($pngs -ne $expected) {
		Fail 1 "capture kept $pngs png files, expected $expected"
	}
	if ($exrs -eq 0) {
		Fail 1 'capture wrote no exr files'
	}
	Write-Host "capture $Index files: png=$pngs exr=$exrs"
	Test-Manifest $out $false
}

function Get-Hashes([string]$Directory) {
	$rows = [ordered]@{}
	Get-ChildItem $Directory -File | Where-Object { $_.Extension -eq '.png' -or $_.Extension -eq '.exr' } | Sort-Object Name | ForEach-Object {
		$rows[$_.Name] = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
	}
	return $rows
}

function Compare-Captures() {
	if ($captureRuns -lt 2) {
		Write-Output 'one capture run, hash comparison not possible'
		return
	}
	$base = Get-Hashes (Join-Path $capturesRoot 'capture_1')
	$pngs = @($base.Keys | Where-Object { $_ -like '*.png' }).Count
	$exrs = @($base.Keys | Where-Object { $_ -like '*.exr' }).Count
	if ($pngs -eq 0 -or $exrs -eq 0) {
		Fail 1 'capture_1 has no png/exr pair to compare'
	}
	for ($index = 2; $index -le $captureRuns; $index++) {
		$other = Get-Hashes (Join-Path $capturesRoot "capture_$index")
		$names = @($base.Keys) + @($other.Keys) | Sort-Object -Unique
		$mismatch = @($names | Where-Object { $base[$_] -ne $other[$_] })
		if ($mismatch.Count -gt 0) {
			foreach ($n in $mismatch) {
				[Console]::Error.WriteLine("hash mismatch: capture_$index\$n")
			}
			Fail 1 "capture_$index differs from capture_1 in $($mismatch.Count) files"
		}
	}
	Write-Output "capture sha256: png=$pngs exr=$exrs runs=$captureRuns all matched"
}

function Invoke-Timing([int]$Index) {
	$out = Join-Path $capturesRoot "timing_$Index"
	if (Test-Path $out) {
		Remove-Item -Recurse -Force $out
	}
	$quitAfter = Get-QuitAfter $timingWarmup $timingFrames
	$godotArgs = @('--path', $Testbed, '--disable-vsync', '--gpu-profile', '--quit-after', "$quitAfter", '--',
		'--mode=timing', "--scene=$sceneRes", "--camera=$cameraName", '--out={out}', "--seed=$Seed",
		"--frames=$timingFrames", "--warmup=$timingWarmup")
	$status = Invoke-Job "selftest-timing-$Index" 'timing' $out $godotArgs
	if ($status -ne 0) {
		Fail $status "timing run $Index exited $status (log: $out\run.log)"
	}
	if (-not (Select-String -Quiet -Path (Join-Path $out 'run.log') -Pattern 'harness: timing complete')) {
		Fail 1 "timing run $Index did not finish"
	}
	$summary = Get-Content -Raw (Join-Path $out 'summary.json') | ConvertFrom-Json
	$wall = $summary.columns.wall_ms
	Write-Host ("timing {0}: frames={1} wall p50={2:N3} p95={3:N3} ms" -f $Index, $summary.frames, $wall.p50, $wall.p95)
	Test-Manifest $out $true
	return [double]$wall.p95
}

New-Item -ItemType Directory -Force $capturesRoot | Out-Null
if ($Mode -eq 'timing' -or $Mode -eq 'all') {
	$p95 = @()
	for ($i = 1; $i -le $timingRuns; $i++) {
		$value = Invoke-Timing $i
		$p95 += $value
	}
	if ($p95.Count -gt 1) {
		$min = ($p95 | Measure-Object -Minimum).Minimum
		$max = ($p95 | Measure-Object -Maximum).Maximum
		$spread = 0.0
		if ($min -gt 0) {
			$spread = ($max - $min) / $min * 100.0
		}
		Write-Output ("timing wall p95 spread {0:N2}% over {1} runs (report only; the <=2% bound is G0-4 in PQ)" -f $spread, $p95.Count)
	}
}
if ($Mode -eq 'capture' -or $Mode -eq 'all') {
	for ($i = 1; $i -le $captureRuns; $i++) {
		Invoke-Capture $i
	}
	Compare-Captures
}
Write-Output 'run_selftest.ps1: done'
exit 0
