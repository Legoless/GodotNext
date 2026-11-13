# Session-1 runner for the RTX 4080 SUPER machine (#237).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File win_session1.ps1 -JobFile <job.json>
#
# A plain SSH login is Session 0, where Godot cannot create a desktop swapchain. This runner
# starts the job in the logged-on console session (Session 1) through a one-time scheduled task
# (schtasks /IT /RL HIGHEST), waits for it, and returns Godot's exit code. It works the same when
# it is started from Session 1.
#
# The job file (JSON):
#   name                 label for the task and the log files (letters, digits, - and _)
#   device               device name from docs/godotnext/fleet.md, e.g. "NVIDIA GeForce RTX 4080 SUPER"
#   driver               "vulkan" (default) or "d3d12"
#   kind                 "run" (default), "capture", "timing" or "validation"
#   godot                editor binary, absolute or relative to the repository root
#                        (default bin\godot.windows.editor.x86_64.console.exe)
#   args                 Godot arguments without --rendering-driver and --gpu-index; the runner
#                        adds both, resolved from fleet.md. "{out}" in an argument becomes the
#                        absolute output directory.
#   out                  output directory, absolute or relative to the repository root
#   timeout_minutes      default 30
#   fail_on_validation   true: exit 1 when Godot exits 0 but logged an "ERROR:" or
#                        "Validation Error" line (default true for kind "validation")
#   lock_clocks          true: lock the GPU and memory clocks to the fleet.md values for the run
#                        and reset them afterwards (default true for kind "timing")
#
# Kind "timing" requires the High performance (or Ultimate Performance) power plan. Kind
# "validation" sets NV_ALLOW_RAYTRACING_VALIDATION=1 and VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1.
# Every job gets GODOTNEXT_HARNESS_JOB, the runner record the harness copies into manifest.json
# (host, clock, power, override). A per-scene override.cfg next to --scene is copied to the
# testbed root for this launch only, as runners/scene_override.sh does on the M5 Max (#240).
#
# The output directory gets run.log (stdout, then stderr), done.json and runner_job.json.
# Exit codes: Godot's, or 2 for a bad job, 3 for a timeout, 4 when the task could not start.

param(
	[Parameter(Mandatory = $true)][string]$JobFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$TestbedRoot = Join-Path $RepoRoot 'godotnext\testbed'
$FleetFile = Join-Path $RepoRoot 'docs\godotnext\fleet.md'

function Fail([int]$Code, [string]$Message) {
	[Console]::Error.WriteLine("win_session1.ps1: $Message")
	exit $Code
}

# UTF-8 without a byte-order mark: Set-Content -Encoding UTF8 writes one in Windows PowerShell 5.1,
# and JSON readers such as Python's reject it.
function Write-Utf8([string]$Path, [string]$Text) {
	[System.IO.File]::WriteAllText($Path, $Text + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Get-Field($Object, [string]$Name, $Default) {
	if ($null -ne $Object -and $Object.PSObject.Properties.Name -contains $Name -and $null -ne $Object.$Name) {
		return $Object.$Name
	}
	return $Default
}

# Windows command-line quoting (the rules CommandLineToArgvW and the MSVC runtime parse).
function Format-Argument([string]$Arg) {
	if ($Arg.Length -gt 0 -and $Arg -notmatch '[\s"]') {
		return $Arg
	}
	$builder = New-Object System.Text.StringBuilder
	[void]$builder.Append('"')
	$slashes = 0
	foreach ($ch in $Arg.ToCharArray()) {
		if ($ch -eq '\') {
			$slashes++
			continue
		}
		if ($ch -eq '"') {
			[void]$builder.Append('\', 2 * $slashes + 1)
		} elseif ($slashes -gt 0) {
			[void]$builder.Append('\', $slashes)
		}
		$slashes = 0
		[void]$builder.Append($ch)
	}
	[void]$builder.Append('\', 2 * $slashes)
	[void]$builder.Append('"')
	return $builder.ToString()
}

# The device table of fleet.md: name -> Session-1 Vulkan and D3D12 --gpu-index, nvidia-smi index.
function Get-FleetDevice([string]$Name, [string]$Driver) {
	if (-not (Test-Path $FleetFile)) {
		Fail 2 "docs/godotnext/fleet.md is missing; device indices come from its device table"
	}
	# Only the rows of the table whose header starts with "| Device name |".
	$known = @()
	$inTable = $false
	foreach ($line in Get-Content $FleetFile) {
		if ($line -match '^\|\s*Device name\s*\|') {
			$inTable = $true
			continue
		}
		if (-not $inTable) {
			continue
		}
		if ($line -notmatch '^\|') {
			$inTable = $false
			continue
		}
		if ($line -match '^\|\s*-') {
			continue
		}
		$cells = @($line.Trim().Trim('|').Split('|') | ForEach-Object { $_.Trim().Trim('`') })
		if ($cells.Count -lt 6) {
			continue
		}
		$known += $cells[0]
		if ($cells[0] -ne $Name) {
			continue
		}
		$column = 2
		if ($Driver -eq 'd3d12') {
			$column = 3
		}
		$index = $cells[$column]
		if ($index -notmatch '^\d+$') {
			Fail 2 "fleet.md gives no $Driver --gpu-index for '$Name'"
		}
		$nvsmi = $null
		if ($cells[4] -match '^\d+$') {
			$nvsmi = [int]$cells[4]
		}
		return [pscustomobject]@{ Name = $Name; GpuIndex = [int]$index; NvsmiIndex = $nvsmi }
	}
	Fail 2 ("unknown device '$Name'. fleet.md lists: " + (($known | Select-Object -Unique) -join ', '))
}

# The timing clock locks of fleet.md ("Base graphics clock" and "Base memory clock" rows).
function Get-FleetClocks() {
	$gpu = $null
	$mem = $null
	foreach ($line in Get-Content $FleetFile) {
		if ($line -match '^\|\s*Base graphics clock\s*\|\s*`(\d+)`') {
			$gpu = [int]$Matches[1]
		} elseif ($line -match '^\|\s*Base memory clock\s*\|\s*`(\d+)`') {
			$mem = [int]$Matches[1]
		}
	}
	if ($null -eq $gpu -or $null -eq $mem) {
		Fail 2 'fleet.md has no "Base graphics clock" and "Base memory clock" rows for the clock locks'
	}
	return [pscustomobject]@{ Gpu = $gpu; Memory = $mem }
}

function Get-PowerRecord() {
	$scheme = (& powercfg /getactivescheme) -join ' '
	$name = ''
	if ($scheme -match '\(([^)]+)\)') {
		$name = $Matches[1]
	}
	$mode = -1
	switch -Regex ($name) {
		'^(High performance|Ultimate Performance)$' { $mode = 2 }
		'^Balanced$' { $mode = 0 }
		'^Power saver$' { $mode = 1 }
	}
	$batteries = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
	$onAc = $true
	foreach ($battery in $batteries) {
		# BatteryStatus 2 is "on AC"; anything else on a machine with a battery is battery power.
		if ($battery.BatteryStatus -ne 2) {
			$onAc = $false
		}
	}
	$thermal = (& nvidia-smi --query-gpu=temperature.gpu,clocks_throttle_reasons.active --format=csv,noheader 2>$null) -join '; '
	if (-not $thermal) {
		$thermal = 'unparsed'
	}
	return [ordered]@{
		on_ac = $onAc
		powermode = $mode
		powermode_name = $name
		thermal = "gpu temperature, throttle reasons: $thermal"
		before = [ordered]@{ powercfg = $scheme; batteries = $batteries.Count }
	}
}

function Find-Argument([string[]]$Arguments, [string]$Flag) {
	for ($i = 0; $i -lt $Arguments.Count; $i++) {
		$arg = $Arguments[$i]
		if ($arg -like "--$Flag=*") {
			return $arg.Substring($Flag.Length + 3)
		}
		if ($arg -eq "--$Flag" -and $i + 1 -lt $Arguments.Count) {
			return $Arguments[$i + 1]
		}
	}
	return ''
}

# ---------- read the job
if (-not (Test-Path $JobFile)) {
	Fail 2 "job file '$JobFile' not found"
}
try {
	$job = Get-Content -Raw $JobFile | ConvertFrom-Json
} catch {
	Fail 2 "job file '$JobFile' is not valid JSON: $($_.Exception.Message)"
}

$name = [string](Get-Field $job 'name' 'job')
if ($name -notmatch '^[A-Za-z0-9_-]{1,64}$') {
	Fail 2 "job name '$name' must be 1-64 letters, digits, - or _"
}
$deviceName = [string](Get-Field $job 'device' '')
if (-not $deviceName) {
	Fail 2 'the job names no device'
}
$driver = [string](Get-Field $job 'driver' 'vulkan')
if ($driver -ne 'vulkan' -and $driver -ne 'd3d12') {
	Fail 2 "driver must be vulkan or d3d12, not '$driver'"
}
$kind = [string](Get-Field $job 'kind' 'run')
if (@('run', 'capture', 'timing', 'validation') -notcontains $kind) {
	Fail 2 "kind must be run, capture, timing or validation, not '$kind'"
}
$godot = [string](Get-Field $job 'godot' 'bin\godot.windows.editor.x86_64.console.exe')
if (-not [System.IO.Path]::IsPathRooted($godot)) {
	$godot = Join-Path $RepoRoot $godot
}
if (-not (Test-Path $godot)) {
	Fail 2 "Godot binary '$godot' not found"
}
$outDir = [string](Get-Field $job 'out' '')
if (-not $outDir) {
	Fail 2 'the job names no out directory'
}
if (-not [System.IO.Path]::IsPathRooted($outDir)) {
	$outDir = Join-Path $RepoRoot $outDir
}
$timeout = [double](Get-Field $job 'timeout_minutes' 30)
$failOnValidation = [bool](Get-Field $job 'fail_on_validation' ($kind -eq 'validation'))
$lockClocks = [bool](Get-Field $job 'lock_clocks' ($kind -eq 'timing'))
$userArgs = @(Get-Field $job 'args' @() | ForEach-Object { [string]$_ })
foreach ($flag in @('--rendering-driver', '--gpu-index')) {
	if ($userArgs | Where-Object { $_ -eq $flag -or $_ -like "$flag=*" }) {
		Fail 2 "the job passes $flag itself; the runner sets it from fleet.md"
	}
}

$device = Get-FleetDevice $deviceName $driver
$clocks = $null
if ($lockClocks) {
	if ($null -eq $device.NvsmiIndex) {
		Fail 2 "fleet.md gives no nvidia-smi index for '$deviceName', so its clocks cannot be locked"
	}
	$clocks = Get-FleetClocks
}
$power = Get-PowerRecord
if ($kind -eq 'timing' -and $power.powermode -ne 2) {
	Fail 2 "timing jobs need the High performance power plan; the active plan is '$($power.powermode_name)'"
}

New-Item -ItemType Directory -Force $outDir | Out-Null
$outDir = (Resolve-Path $outDir).Path
$userArgs = @($userArgs | ForEach-Object { $_.Replace('{out}', $outDir) })
$godotArgs = @('--rendering-driver', $driver, '--gpu-index', [string]$device.GpuIndex) + $userArgs

# ---------- per-scene override.cfg (#240)
$overrideRecord = [ordered]@{ applied = $false; source = ''; sha256 = '' }
$rootOverride = Join-Path $TestbedRoot 'override.cfg'
$sceneArg = Find-Argument $userArgs 'scene'
$sceneOverride = $null
if ($sceneArg -like 'res://*') {
	$relative = $sceneArg.Substring(6)
	$folder = ''
	if ($relative.Contains('/')) {
		$folder = $relative.Substring(0, $relative.LastIndexOf('/'))
	}
	if ($folder) {
		$candidate = Join-Path $TestbedRoot ($folder.Replace('/', '\') + '\override.cfg')
		if (Test-Path $candidate) {
			$sceneOverride = $candidate
			$overrideRecord = [ordered]@{
				applied = $true
				source = "res://$folder/override.cfg"
				sha256 = (Get-FileHash -Algorithm SHA256 $candidate).Hash.ToLowerInvariant()
			}
		}
	}
}

# ---------- the runner record the harness reads (GODOTNEXT_HARNESS_JOB)
$clockRecord = [ordered]@{ locked = $false; detail = 'Clocks are not locked for this job.' }
if ($lockClocks) {
	$clockRecord = [ordered]@{
		locked = $true
		detail = "nvidia-smi -i $($device.NvsmiIndex) --lock-gpu-clocks=$($clocks.Gpu),$($clocks.Gpu) --lock-memory-clocks=$($clocks.Memory),$($clocks.Memory) for the run, reset with -rgc and -rmc afterwards (fleet.md)"
		gpu_mhz = $clocks.Gpu
		memory_mhz = $clocks.Memory
		nvidia_smi_index = $device.NvsmiIndex
	}
}
$runnerJob = [ordered]@{
	host = $deviceName
	device = [ordered]@{ name = $deviceName; driver = $driver; gpu_index = $device.GpuIndex; nvidia_smi_index = $device.NvsmiIndex }
	kind = $kind
	clock = $clockRecord
	power = $power
	override = $overrideRecord
}
$runnerJobPath = Join-Path $outDir 'runner_job.json'
Write-Utf8 $runnerJobPath ($runnerJob | ConvertTo-Json -Depth 6)

# ---------- the Session-1 script
$stateDir = Join-Path $env:LOCALAPPDATA 'GodotNextHarness'
New-Item -ItemType Directory -Force $stateDir | Out-Null
$token = [guid]::NewGuid().ToString('N').Substring(0, 12)
$taskName = "GodotNextHarness-$name-$token"
$innerPath = Join-Path $stateDir "$name-$token.ps1"
$donePath = Join-Path $outDir 'done.json'
$stdoutPath = Join-Path $outDir 'run.stdout.log'
$stderrPath = Join-Path $outDir 'run.stderr.log'
$logPath = Join-Path $outDir 'run.log'
foreach ($path in @($donePath, $stdoutPath, $stderrPath, $logPath)) {
	Remove-Item -Force -ErrorAction SilentlyContinue $path
}

function Format-PsString([string]$Value) {
	return "'" + $Value.Replace("'", "''") + "'"
}

$commandLine = ($godotArgs | ForEach-Object { Format-Argument $_ }) -join ' '
$env_lines = @("`$env:GODOTNEXT_HARNESS_JOB = $(Format-PsString $runnerJobPath)")
if ($kind -eq 'validation') {
	$env_lines += '$env:NV_ALLOW_RAYTRACING_VALIDATION = ''1'''
	$env_lines += '$env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = ''1'''
}
$lockLines = ''
$unlockLines = ''
if ($lockClocks) {
	$i = $device.NvsmiIndex
	$lockLines = @"
& nvidia-smi -i $i --lock-gpu-clocks=$($clocks.Gpu),$($clocks.Gpu) | Out-Null
if (`$LASTEXITCODE -ne 0) { throw 'nvidia-smi could not lock the GPU clock' }
& nvidia-smi -i $i --lock-memory-clocks=$($clocks.Memory),$($clocks.Memory) | Out-Null
if (`$LASTEXITCODE -ne 0) { throw 'nvidia-smi could not lock the memory clock' }
# The clocks settle within a second or two; read them back until both show the locked values.
for (`$wait = 0; `$wait -lt 40; `$wait++) {
	`$locked = (& nvidia-smi -i $i --query-gpu=clocks.gr,clocks.mem --format=csv,noheader) -join ' '
	if (`$locked -eq '$($clocks.Gpu) MHz, $($clocks.Memory) MHz') { break }
	Start-Sleep -Milliseconds 250
}
if (`$locked -ne '$($clocks.Gpu) MHz, $($clocks.Memory) MHz') { throw "the clocks did not lock: `$locked" }
"@
	$unlockLines = @"
& nvidia-smi -i $i -rgc | Out-Null
& nvidia-smi -i $i -rmc | Out-Null
"@
}
$overrideCopy = ''
$overrideRemove = ''
if ($sceneOverride) {
	$overrideCopy = @"
if (Test-Path $(Format-PsString $rootOverride)) { throw 'the testbed root already has an override.cfg; not overwriting it' }
Copy-Item -LiteralPath $(Format-PsString $sceneOverride) -Destination $(Format-PsString $rootOverride)
`$overrideCopied = `$true
"@
	$overrideRemove = "if (`$overrideCopied) { Remove-Item -Force -ErrorAction SilentlyContinue $(Format-PsString $rootOverride) }"
}

$inner = @"
`$ErrorActionPreference = 'Stop'
`$code = 4
`$errorText = ''
`$locked = ''
`$overrideCopied = `$false
`$started = (Get-Date).ToString('o')
Set-Location -LiteralPath $(Format-PsString $RepoRoot)
$($env_lines -join "`r`n")
try {
$overrideCopy
$lockLines
	`$process = Start-Process -FilePath $(Format-PsString $godot) -ArgumentList $(Format-PsString $commandLine) -RedirectStandardOutput $(Format-PsString $stdoutPath) -RedirectStandardError $(Format-PsString $stderrPath) -NoNewWindow -PassThru
	# Reading the handle keeps ExitCode available after the process exits.
	`$null = `$process.Handle
	`$process.WaitForExit()
	`$code = `$process.ExitCode
} catch {
	`$errorText = `$_.Exception.Message
} finally {
$unlockLines
$overrideRemove
	`$doneText = [ordered]@{ exit_code = `$code; error = `$errorText; locked_clocks = `$locked; started = `$started; finished = (Get-Date).ToString('o'); session = (Get-Process -Id `$PID).SessionId } | ConvertTo-Json
	[System.IO.File]::WriteAllText($(Format-PsString $donePath), `$doneText, (New-Object System.Text.UTF8Encoding `$false))
}
"@
Write-Utf8 $innerPath $inner

# ---------- start it in Session 1 and wait
$taskCommand = "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$innerPath`""
if ($taskCommand.Length -gt 261) {
	Fail 2 "the task command is longer than schtasks allows: $taskCommand"
}
$started = Get-Date
$created = & schtasks /Create /F /TN $taskName /SC ONCE /ST 23:59 /RL HIGHEST /IT /TR $taskCommand 2>&1
if ($LASTEXITCODE -ne 0) {
	Remove-Item -Force -ErrorAction SilentlyContinue $innerPath
	Fail 4 "schtasks /Create failed: $($created -join ' ')"
}
try {
	$ran = & schtasks /Run /TN $taskName 2>&1
	if ($LASTEXITCODE -ne 0) {
		Fail 4 "schtasks /Run failed (is a user logged on at the console?): $($ran -join ' ')"
	}
	$deadline = $started.AddMinutes($timeout)
	while (-not (Test-Path $donePath)) {
		if ((Get-Date) -gt $deadline) {
			& schtasks /End /TN $taskName 2>&1 | Out-Null
			Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $godot } | Stop-Process -Force -ErrorAction SilentlyContinue
			Fail 3 "the job did not finish within $timeout minutes"
		}
		Start-Sleep -Milliseconds 500
	}
	# done.json is written last; give the writer a moment to close it.
	$done = $null
	for ($attempt = 0; $attempt -lt 20 -and $null -eq $done; $attempt++) {
		try {
			$done = Get-Content -Raw $donePath | ConvertFrom-Json
		} catch {
			Start-Sleep -Milliseconds 250
		}
	}
} finally {
	& schtasks /Delete /F /TN $taskName 2>&1 | Out-Null
	Remove-Item -Force -ErrorAction SilentlyContinue $innerPath
}
if ($null -eq $done) {
	Fail 4 'done.json could not be read'
}

# run.log: stdout, then stderr.
$log = New-Object System.Collections.Generic.List[string]
foreach ($path in @($stdoutPath, $stderrPath)) {
	if (Test-Path $path) {
		foreach ($line in [System.IO.File]::ReadAllLines($path)) {
			$log.Add($line)
		}
		Remove-Item -Force $path
	}
}
[System.IO.File]::WriteAllLines($logPath, $log)
foreach ($line in $log) {
	Write-Output $line
}

# The power record after the run goes into the manifest the harness wrote, as on the M5 Max.
$manifestPath = Join-Path $outDir 'manifest.json'
if (Test-Path $manifestPath) {
	$after = Get-PowerRecord
	$manifestText = Get-Content -Raw $manifestPath
	$manifest = $manifestText | ConvertFrom-Json
	if ($manifest.PSObject.Properties.Name -contains 'runner' -and $manifest.runner.PSObject.Properties.Name -contains 'power') {
		$manifest.runner.power | Add-Member -Force -NotePropertyName after -NotePropertyValue $after.before
		Write-Utf8 $manifestPath ($manifest | ConvertTo-Json -Depth 12)
	}
}

$code = [int]$done.exit_code
if ($done.error) {
	[Console]::Error.WriteLine("win_session1.ps1: the Session-1 job failed: $($done.error)")
	if ($code -eq 0) {
		$code = 4
	}
}
# Case-sensitive: the Vulkan loader logs lower-case "error:" lines that are not errors.
$errors = @($log | Where-Object { $_ -cmatch 'ERROR:' }).Count
$validation = @($log | Where-Object { $_ -cmatch 'Validation Error' }).Count
Write-Output ('GODOTNEXT_RUNNER {"device":"' + $deviceName + '","driver":"' + $driver + '","gpu_index":' + $device.GpuIndex + ',"session":' + $done.session + ',"clocks_locked":' + $lockClocks.ToString().ToLowerInvariant() + ',"errors":' + $errors + ',"validation_errors":' + $validation + ',"status":' + $code + '}')
if ($failOnValidation -and $code -eq 0 -and ($errors -ne 0 -or $validation -ne 0)) {
	[Console]::Error.WriteLine('win_session1.ps1: the run logged validation lines; the bar is 0')
	$code = 1
}
exit $code
