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
#   engine               "godotnext" (default) or "ue": the UE 5.9 editor in game mode for the UE
#                        comparison (#229); see "UE jobs" below
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
#   env                  extra environment variables for the launch, as {"NAME": "value"};
#                        "{out}" in a value becomes the absolute output directory
#   override             a res:// override.cfg copied to the testbed root for this launch only,
#                        for a scene folder that has none of its own (the UE comparison's settings)
#   exclusive_gpu        true: refuse to start (exit 6) while another engine process uses the GPU:
#                        Godot, UnrealEditor or UnrealGame not started headless (--headless or
#                        -nullrhi); for kind "timing" also headless ones, ShaderCompileWorker,
#                        UnrealBuildTool and the MSVC compiler and linker. Checked before the task
#                        is scheduled and again in Session 1 just before the launch (default
#                        false; the UE comparison sets it, because the machine is shared)
#   priority             "Normal" (default) or "BelowNormal": the launch's process priority class;
#                        processes it starts inherit BelowNormal
#
# UE jobs ("engine": "ue") run the installed UE editor in game mode (plan v3.9.3):
#   exe                  the install's ...\Engine\Binaries\Win64\UnrealEditor.exe, absolute (from a
#                        private job file; the runner reads only that file's version)
#   project              the comparison project's .uproject, absolute; it goes first on the command line
#   args                 the rest of the command line, with -game; "{out}" becomes the output
#                        directory, and a -Key=value argument whose value has spaces is passed as
#                        -Key="value". The runner adds -csvMetadata=GodotNextRun=<token>.
#   csv_dir              the folder the editor writes CSV profiles to (in game mode the per-user
#                        engine folder, shared with every other UE 5.9 project of the user). The
#                        one profile whose metadata carries this launch's token is moved to
#                        <out>\ue_profile.csv (exit 5 when there is not exactly one); other files
#                        are never touched
#   kind                 "run" or "timing" (clock locks and the power plan as for Godot)
#   ok_exit_codes        the editor's exit codes that count as a finished run (default [0]); the
#                        runner then exits 0 and records the editor's own code in runner_job.json
# The runner waits for the editor process only, never for the processes it leaves running (its
# derived-data server and crash handler stay up after a game-mode exit and are shared), and on a
# timeout stops that one process by its id. A run that exits 0 counts only when the log it wrote
# with -abslog=<out>\ue.log shows the CSV profiler's exit request, no fatal error and no line that
# reports a material without its usage flag or the default material in its place (exit 7
# otherwise); runner_job.json records those booleans, that line count, the editor's version, its
# observed priority and the GPU clocks and temperature after the run. The editor's stdout and
# stderr are not redirected (its derived-data server would inherit the handles and outlive it); its
# log is ue.log.
#
# Kind "timing" requires the High performance (or Ultimate Performance) power plan. Kind
# "validation" sets NV_ALLOW_RAYTRACING_VALIDATION=1 and VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1.
# Every job gets GODOTNEXT_HARNESS_JOB, the runner record the harness copies into manifest.json
# (host, clock, power, override), plus "resolution" from a --resolution WxH argument, which the
# harness checks the viewport against (#270). A per-scene override.cfg next to --scene is copied
# to the testbed root for this launch only, as runners/scene_override.sh does on the M5 Max (#240).
#
# The output directory gets run.log (stdout, then stderr), done.json and runner_job.json.
# Exit codes: the engine's, or 2 for a bad job, 3 for a timeout, 4 when the task could not start,
# 5 when a UE job did not leave exactly one tagged CSV profile, 6 when exclusive_gpu found another
# engine process, 7 when a UE run exited 0 without the CSV profiler's exit, with a fatal error or
# with a material fallback line.

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

# A UE command line: a -Key=value argument whose value has spaces goes as -Key="value", the form
# Epic's command-line reference documents (for example -ExecCmds="cmd 1,cmd 2").
function Format-UeArgument([string]$Arg) {
	if ($Arg -match '^(-[A-Za-z0-9_.]+=)(.*\s.*)$') {
		$value = $Matches[2]
		if ($value.Contains('"') -or $value.EndsWith('\')) {
			Fail 2 "UE argument '$Arg' cannot be quoted: its value has a quote or ends with a backslash"
		}
		return $Matches[1] + '"' + $value + '"'
	}
	return Format-Argument $Arg
}

# Engine processes that would share the GPU with the job: Godot, UnrealEditor and UnrealGame unless
# started headless. -Strict (timing jobs) also counts headless ones and CPU-heavy tools. The same
# text runs in the Session-1 script just before the launch ($BusyCheck).
$BusyCheck = @'
function Get-BusyEngineProcesses([bool]$Strict) {
	$found = @()
	foreach ($process in @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)) {
		$processName = [string]$process.Name
		$command = [string]$process.CommandLine
		$engine = $processName -match '^(godot|UnrealEditor|UnrealGame)'
		$tool = $processName -match '^(ShaderCompileWorker|UnrealBuildTool|cl|link)\.exe$'
		if ($Strict) {
			if (-not ($engine -or $tool)) { continue }
		} elseif (-not $engine -or $command -match '(^|\s)(--headless|-nullrhi)(\s|$)') {
			continue
		}
		$found += "$processName (pid $($process.ProcessId))"
	}
	return $found
}
'@
. ([scriptblock]::Create($BusyCheck))

# The one CSV profile in $Directory whose metadata row (the file's last lines) carries the token
# that -csvMetadata=GodotNextRun=<token> wrote. Only files written since $Since are read.
function Find-TaggedCsvProfiles([string]$Directory, [string]$Token, [datetime]$Since) {
	if (-not $Directory -or -not (Test-Path -LiteralPath $Directory -PathType Container)) {
		return @()
	}
	$pattern = '\[godotnextrun\],' + [regex]::Escape($Token) + '(,|$)'
	$found = @()
	foreach ($file in @(Get-ChildItem -LiteralPath $Directory -Filter 'Profile*.csv' -File | Where-Object { $_.LastWriteTime -ge $Since })) {
		$tail = @(Get-Content -LiteralPath $file.FullName -Tail 2 -ErrorAction SilentlyContinue) -join "`n"
		if ($tail -match $pattern) {
			$found += $file.FullName
		}
	}
	return $found
}

# The number of lines in a UE log that report a material without its usage flag or the default
# material drawn in its place. Epic's Material Properties page (UE 5.9, Usage): "If you do not have
# the appropriate usage flag set, the default world grid material will be used in-game instead. This
# will have an appropriate message in the log of the game client." Content drawn that way leaves its
# own passes (a Nanite mesh then draws in the base pass, for example) while the run still exits
# cleanly. Case-insensitive; only the count is kept.
function Measure-UeMaterialFallbackLines([string]$Path) {
	return @(Select-String -LiteralPath $Path -Pattern 'usage flag|default ?material').Count
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
$engine = [string](Get-Field $job 'engine' 'godotnext')
if ($engine -ne 'godotnext' -and $engine -ne 'ue') {
	Fail 2 "engine must be godotnext or ue, not '$engine'"
}
$ueJob = $engine -eq 'ue'
$kind = [string](Get-Field $job 'kind' 'run')
$kinds = @('run', 'capture', 'timing', 'validation')
if ($ueJob) {
	$kinds = @('run', 'timing')
}
if ($kinds -notcontains $kind) {
	Fail 2 "kind must be $($kinds -join ', ') for a $engine job, not '$kind'"
}
$csvDir = ''
$project = ''
if ($ueJob) {
	# UE picks its RHI from its own command line; the fleet.md row gives the nvidia-smi index.
	$driver = 'd3d12'
	$exe = [string](Get-Field $job 'exe' '')
	if (-not $exe -or -not [System.IO.Path]::IsPathRooted($exe) -or $exe -notmatch '\\Engine\\Binaries\\Win64\\UnrealEditor\.exe$') {
		Fail 2 'a UE job names the installed editor ...\Engine\Binaries\Win64\UnrealEditor.exe by absolute path (plan v3.9.3)'
	}
	if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
		Fail 2 "UE editor '$exe' not found"
	}
	$project = [string](Get-Field $job 'project' '')
	if (-not $project -or -not [System.IO.Path]::IsPathRooted($project) -or -not $project.EndsWith('.uproject', [System.StringComparison]::OrdinalIgnoreCase)) {
		Fail 2 'a UE job names its comparison project .uproject by absolute path'
	}
	if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
		Fail 2 "UE project '$project' not found"
	}
	$csvDir = [string](Get-Field $job 'csv_dir' '')
	if (-not $csvDir -or -not [System.IO.Path]::IsPathRooted($csvDir)) {
		Fail 2 'a UE job names csv_dir, the folder the editor writes CSV profiles to, by absolute path'
	}
	foreach ($field in @('godot', 'override', 'fail_on_validation')) {
		if ($null -ne (Get-Field $job $field $null)) {
			Fail 2 "a UE job takes no $field"
		}
	}
	$okExitCodes = @(Get-Field $job 'ok_exit_codes' @(0) | ForEach-Object { [long]$_ })
	if ($okExitCodes -notcontains 0) {
		Fail 2 'ok_exit_codes lists the exit codes of a finished run, 0 among them'
	}
} else {
	$driver = [string](Get-Field $job 'driver' 'vulkan')
	if ($driver -ne 'vulkan' -and $driver -ne 'd3d12') {
		Fail 2 "driver must be vulkan or d3d12, not '$driver'"
	}
	$exe = [string](Get-Field $job 'godot' 'bin\godot.windows.editor.x86_64.console.exe')
	if (-not [System.IO.Path]::IsPathRooted($exe)) {
		$exe = Join-Path $RepoRoot $exe
	}
	if (-not (Test-Path $exe)) {
		Fail 2 "Godot binary '$exe' not found"
	}
}
$exclusiveGpu = [bool](Get-Field $job 'exclusive_gpu' $false)
$priority = [string](Get-Field $job 'priority' 'Normal')
if ($priority -ne 'Normal' -and $priority -ne 'BelowNormal') {
	Fail 2 "priority must be Normal or BelowNormal, not '$priority'"
}
$outDir = [string](Get-Field $job 'out' '')
if (-not $outDir) {
	Fail 2 'the job names no out directory'
}
if (-not [System.IO.Path]::IsPathRooted($outDir)) {
	$outDir = Join-Path $RepoRoot $outDir
}
$timeout = [double](Get-Field $job 'timeout_minutes' 30)
$failOnValidation = (-not $ueJob) -and [bool](Get-Field $job 'fail_on_validation' ($kind -eq 'validation'))
$lockClocks = [bool](Get-Field $job 'lock_clocks' ($kind -eq 'timing'))
$userArgs = @(Get-Field $job 'args' @() | ForEach-Object { [string]$_ })
if (-not $ueJob) {
	foreach ($flag in @('--rendering-driver', '--gpu-index')) {
		if ($userArgs | Where-Object { $_ -eq $flag -or $_ -like "$flag=*" }) {
			Fail 2 "the job passes $flag itself; the runner sets it from fleet.md"
		}
	}
} else {
	if (-not ($userArgs | Where-Object { $_ -eq '-game' })) {
		Fail 2 'a UE job runs the editor in game mode: its args need -game (plan v3.9.3)'
	}
	if ($userArgs | Where-Object { $_ -like '-csvMetadata*' }) {
		Fail 2 'the job passes -csvMetadata itself; the runner sets it to tag the launch''s CSV profile'
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
$extraEnv = [ordered]@{}
$envField = Get-Field $job 'env' $null
if ($null -ne $envField) {
	if ($envField -isnot [System.Management.Automation.PSCustomObject]) {
		Fail 2 'env must be an object of "NAME": "value" pairs'
	}
	foreach ($property in $envField.PSObject.Properties) {
		if ($property.Name -notmatch '^[A-Za-z_][A-Za-z0-9_]{0,127}$') {
			Fail 2 "env name '$($property.Name)' must be letters, digits and _, not starting with a digit"
		}
		$extraEnv[$property.Name] = ([string]$property.Value).Replace('{out}', $outDir)
	}
}
$token = [guid]::NewGuid().ToString('N').Substring(0, 12)
if ($ueJob) {
	# The CSV profiler adds -csvMetadata pairs to the capture's metadata row ([godotnextrun],<token>),
	# which is how the runner finds this launch's profile in the shared CSV folder.
	$csvToken = "$name-$token".ToLowerInvariant()
	$launchArgs = @($project) + $userArgs + @("-csvMetadata=GodotNextRun=$csvToken")
	$driverLabel = 'default'
	if ($userArgs | Where-Object { $_ -eq '-dx12' }) {
		$driverLabel = 'dx12'
	}
} else {
	$launchArgs = @('--rendering-driver', $driver, '--gpu-index', [string]$device.GpuIndex) + $userArgs
	$driverLabel = $driver
}

# ---------- per-scene override.cfg (#240)
$overrideRecord = [ordered]@{ applied = $false; source = ''; sha256 = '' }
$rootOverride = Join-Path $TestbedRoot 'override.cfg'
$sceneArg = ''
if (-not $ueJob) {
	$sceneArg = Find-Argument $userArgs 'scene'
}
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
$jobOverride = [string](Get-Field $job 'override' '')
if ($jobOverride) {
	if ($jobOverride -notmatch '^res://[A-Za-z0-9_./-]+\.cfg$' -or $jobOverride.Contains('..')) {
		Fail 2 "override must be a res:// path to a .cfg file, not '$jobOverride'"
	}
	if ($sceneOverride) {
		Fail 2 "the scene's folder has its own override.cfg ($($overrideRecord.source)); the job's override would replace it"
	}
	$candidate = Join-Path $TestbedRoot $jobOverride.Substring(6).Replace('/', '\')
	if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
		Fail 2 "override '$jobOverride' not found"
	}
	$sceneOverride = $candidate
	$overrideRecord = [ordered]@{
		applied = $true
		source = $jobOverride
		sha256 = (Get-FileHash -Algorithm SHA256 $candidate).Hash.ToLowerInvariant()
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
	engine = $engine
	device = [ordered]@{ name = $deviceName; driver = $driverLabel; gpu_index = $device.GpuIndex; nvidia_smi_index = $device.NvsmiIndex }
	kind = $kind
	clock = $clockRecord
	power = $power
	override = $overrideRecord
	env = $extraEnv
}
if ($ueJob) {
	# The report takes the run's resolution from the capture's metadata, not from these arguments.
	$runnerJob['exe'] = Split-Path -Leaf $exe
	$runnerJob['args'] = $userArgs
	$version = (Get-Item -LiteralPath $exe).VersionInfo
	$runnerJob['ue_editor'] = [ordered]@{ file_version = [string]$version.FileVersion; product_version = [string]$version.ProductVersion }
	$runnerJob['run_mode'] = 'editor_game'
} else {
	# The engine consumes --resolution, so the harness learns the requested output size from here (#270).
	$resolutionArg = Find-Argument $userArgs 'resolution'
	if ($resolutionArg -match '^(\d+)x(\d+)$') {
		$runnerJob['resolution'] = @([int]$Matches[1], [int]$Matches[2])
	}
}
# The machine is shared: a comparison job does not start while another engine uses the GPU.
if ($exclusiveGpu) {
	$busy = @(Get-BusyEngineProcesses ($kind -eq 'timing'))
	if ($busy.Count -gt 0) {
		Fail 6 ('another engine process is running: ' + ($busy -join '; '))
	}
	$runnerJob['exclusive_gpu'] = $true
}
$runnerJob['priority'] = $priority
$runnerJobPath = Join-Path $outDir 'runner_job.json'
Write-Utf8 $runnerJobPath ($runnerJob | ConvertTo-Json -Depth 6)

# ---------- the Session-1 script
$stateDir = Join-Path $env:LOCALAPPDATA 'GodotNextHarness'
New-Item -ItemType Directory -Force $stateDir | Out-Null
$taskName = "GodotNextHarness-$name-$token"
$innerPath = Join-Path $stateDir "$name-$token.ps1"
$donePath = Join-Path $outDir 'done.json'
$stdoutPath = Join-Path $outDir 'run.stdout.log'
$stderrPath = Join-Path $outDir 'run.stderr.log'
$logPath = Join-Path $outDir 'run.log'
$pidPath = Join-Path $outDir 'launch_pid.txt'
foreach ($path in @($donePath, $stdoutPath, $stderrPath, $logPath, $pidPath)) {
	Remove-Item -Force -ErrorAction SilentlyContinue $path
}

function Format-PsString([string]$Value) {
	return "'" + $Value.Replace("'", "''") + "'"
}

if ($ueJob) {
	$commandLine = ($launchArgs | ForEach-Object { Format-UeArgument $_ }) -join ' '
	$workDir = Split-Path -Parent $project
} else {
	$commandLine = ($launchArgs | ForEach-Object { Format-Argument $_ }) -join ' '
	$workDir = $RepoRoot
}
$env_lines = @("`$env:GODOTNEXT_HARNESS_JOB = $(Format-PsString $runnerJobPath)")
if ($kind -eq 'validation') {
	$env_lines += '$env:NV_ALLOW_RAYTRACING_VALIDATION = ''1'''
	$env_lines += '$env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = ''1'''
}
foreach ($key in $extraEnv.Keys) {
	$env_lines += "`$env:$key = $(Format-PsString $extraEnv[$key])"
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

# Priority: set on the Session-1 script itself, so the launch and what it starts inherit it.
$priorityLine = ''
if ($priority -ne 'Normal') {
	$priorityLine = "[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = '$priority'"
}
# The machine is shared: check again in Session 1, just before the launch.
$busyLines = ''
if ($exclusiveGpu) {
	$busyLines = @"
$BusyCheck
`$busyNow = @(Get-BusyEngineProcesses `$$($kind -eq 'timing'))
if (`$busyNow.Count -gt 0) { `$code = 6; throw ('another engine process is running: ' + (`$busyNow -join '; ')) }
"@
}
# The UE editor leaves its derived-data server and crash handler running after a game-mode exit;
# they are shared, so the runner waits for the editor alone and only notes which are still up.
$afterLines = ''
# A Godot launch shares the runner's redirected stdout and stderr. The UE editor does not: its
# derived-data server inherits such handles and outlives it, and its log goes to -abslog.
$startLine = "Start-Process -FilePath $(Format-PsString $exe) -ArgumentList $(Format-PsString $commandLine) -WorkingDirectory $(Format-PsString $workDir) -RedirectStandardOutput $(Format-PsString $stdoutPath) -RedirectStandardError $(Format-PsString $stderrPath) -NoNewWindow -PassThru"
if ($ueJob) {
	$startLine = "Start-Process -FilePath $(Format-PsString $exe) -ArgumentList $(Format-PsString $commandLine) -WorkingDirectory $(Format-PsString $workDir) -PassThru"
	$afterLines = "`t`$childRecords = @(Get-CimInstance Win32_Process -Filter `"ParentProcessId=`$(`$process.Id)`" -ErrorAction SilentlyContinue | ForEach-Object { [string]`$_.Name })"
}

$inner = @"
`$ErrorActionPreference = 'Stop'
`$code = 4
`$errorText = ''
`$locked = ''
`$overrideCopied = `$false
`$childRecords = @()
`$observedPriority = ''
`$gpuAfter = ''
`$started = (Get-Date).ToString('o')
Set-Location -LiteralPath $(Format-PsString $RepoRoot)
$($env_lines -join "`r`n")
$priorityLine
try {
$busyLines
$overrideCopy
$lockLines
	`$launched = Get-Date
	`$process = $startLine
	# Reading the handle keeps ExitCode available after the process exits.
	`$null = `$process.Handle
	[System.IO.File]::WriteAllText($(Format-PsString $pidPath), [string]`$process.Id)
	try { `$observedPriority = [string]`$process.PriorityClass } catch { }
	`$process.WaitForExit()
	`$code = `$process.ExitCode
	# The GPU's clocks and temperature right after the run, before any clock lock is reset (§23.4).
	`$gpuAfter = (& nvidia-smi --query-gpu=clocks.gr,clocks.mem,temperature.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits 2>`$null) -join '; '
$afterLines
} catch {
	`$errorText = `$_.Exception.Message
} finally {
$unlockLines
$overrideRemove
	`$doneText = [ordered]@{ exit_code = `$code; error = `$errorText; locked_clocks = `$locked; started = `$started; finished = (Get-Date).ToString('o'); session = (Get-Process -Id `$PID).SessionId; priority = `$observedPriority; gpu_after = `$gpuAfter; children = @(`$childRecords) } | ConvertTo-Json
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
			if ($ueJob) {
				# Only the editor process this job started, by its id: the install folder also holds
				# other projects' editors, and the editor's helper processes are shared.
				if (Test-Path -LiteralPath $pidPath) {
					$launchId = [int](Get-Content -Raw -LiteralPath $pidPath)
					$own = Get-CimInstance Win32_Process -Filter "ProcessId=$launchId" -ErrorAction SilentlyContinue
					if ($own -and [string]$own.ExecutablePath -eq $exe -and $own.CreationDate -ge $started) {
						Stop-Process -Id $launchId -Force -ErrorAction SilentlyContinue
						Write-Output "win_session1.ps1: stopped the timed-out editor (pid $launchId)"
					}
				}
			} else {
				Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force -ErrorAction SilentlyContinue
			}
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
		Remove-Item -Force -ErrorAction SilentlyContinue $path
	}
}
[System.IO.File]::WriteAllLines($logPath, $log)
if ($ueJob) {
	# UE output stays in the output directory (private storage); it is not echoed to the caller.
	Write-Output "win_session1.ps1: run.log has $($log.Count) lines of UE output (not echoed)"
} else {
	foreach ($line in $log) {
		Write-Output $line
	}
}

# The GPU record after the run: "<graphics MHz>; <memory MHz>; <temperature C>; <throttle reasons>".
function ConvertTo-GpuAfter([string]$Text) {
	$cells = @($Text.Split(',') | ForEach-Object { $_.Trim() })
	$number = 0.0
	foreach ($index in 0, 1, 2) {
		if ($cells.Count -lt 4 -or -not [double]::TryParse($cells[$index], [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
			return [ordered]@{ raw = $Text }
		}
	}
	$culture = [System.Globalization.CultureInfo]::InvariantCulture
	return [ordered]@{ clocks_gr_mhz = [double]::Parse($cells[0], $culture); clocks_mem_mhz = [double]::Parse($cells[1], $culture); temperature_c = [double]::Parse($cells[2], $culture); throttle_reasons = $cells[3] }
}

# A UE job: move the one CSV profile tagged with this launch's token, read the exit markers and the
# material fallback count from the log it wrote, and record them in runner_job.json (UE writes no
# manifest). Only booleans and counts
# are recorded; no UE log text leaves the output directory.
$csvProblem = ''
$exitProblem = ''
if ($ueJob) {
	$tagged = @(Find-TaggedCsvProfiles $csvDir $csvToken $started.AddSeconds(-5))
	$csvRecord = [ordered]@{ token = $csvToken; collected = $false; tagged_files = $tagged.Count }
	if ($tagged.Count -eq 1) {
		Move-Item -LiteralPath $tagged[0] -Destination (Join-Path $outDir 'ue_profile.csv') -Force
		$csvRecord.collected = $true
	} else {
		$csvProblem = "the UE run left $($tagged.Count) CSV profiles tagged $csvToken; expected one"
	}
	$ueLog = Join-Path $outDir 'ue.log'
	$exitRecord = [ordered]@{ code = [long]$done.exit_code; log = $false; csv_exit = $false; log_exit = $false; fatal = $null; material_fallback_lines = $null; children_alive_after = @($done.children) }
	if (Test-Path -LiteralPath $ueLog) {
		$exitRecord.log = $true
		$exitRecord.csv_exit = [bool](Select-String -LiteralPath $ueLog -SimpleMatch 'CsvProfiler.ExitAfterCsvProfiling' -Quiet)
		$exitRecord.log_exit = [bool](Select-String -LiteralPath $ueLog -SimpleMatch 'LogExit: Exiting.' -Quiet)
		$exitRecord.fatal = [bool](Select-String -LiteralPath $ueLog -Pattern 'Fatal error|=== Critical error ===|Unhandled Exception' -Quiet)
		$exitRecord.material_fallback_lines = Measure-UeMaterialFallbackLines $ueLog
	}
	if (-not $exitRecord.csv_exit -or $exitRecord.fatal -ne $false) {
		$exitProblem = "the UE run did not end through -ExitAfterCsvProfiling or logged a fatal error (csv_exit $($exitRecord.csv_exit), fatal $($exitRecord.fatal))"
	} elseif ($exitRecord.material_fallback_lines -ne 0) {
		$exitProblem = "the UE log has $($exitRecord.material_fallback_lines) line(s) reporting a material without its usage flag or the default material in its place, so some content did not draw as imported"
	}
	$runnerJob['csv'] = $csvRecord
	$runnerJob['exit'] = $exitRecord
	$runnerJob['engine_exit_code'] = [long]$done.exit_code
	$runnerJob['ok_exit_codes'] = $okExitCodes
	$runnerJob['observed_priority'] = [string]$done.priority
	$runnerJob['gpu_after'] = ConvertTo-GpuAfter ([string]$done.gpu_after)
	$runnerJob.power['after'] = (Get-PowerRecord).before
	Write-Utf8 $runnerJobPath ($runnerJob | ConvertTo-Json -Depth 6)
}

# The power record after the run goes into the manifest the harness wrote, as on the M5 Max.
$manifestPath = Join-Path $outDir 'manifest.json'
if (-not $ueJob -and (Test-Path $manifestPath)) {
	$after = Get-PowerRecord
	$manifestText = Get-Content -Raw $manifestPath
	$manifest = $manifestText | ConvertFrom-Json
	if ($manifest.PSObject.Properties.Name -contains 'runner' -and $manifest.runner.PSObject.Properties.Name -contains 'power') {
		$manifest.runner.power | Add-Member -Force -NotePropertyName after -NotePropertyValue $after.before
		if ([string]$done.gpu_after) {
			$manifest.runner | Add-Member -Force -NotePropertyName gpu_after -NotePropertyValue (ConvertTo-GpuAfter ([string]$done.gpu_after))
		}
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
if ($ueJob -and $code -ne 0 -and -not $done.error -and $okExitCodes -contains [long]$code) {
	Write-Output "win_session1.ps1: the UE editor exited with $code, listed in ok_exit_codes"
	$code = 0
}
if ($exitProblem -and $code -eq 0) {
	[Console]::Error.WriteLine("win_session1.ps1: $exitProblem")
	$code = 7
}
if ($csvProblem -and $code -eq 0) {
	[Console]::Error.WriteLine("win_session1.ps1: $csvProblem")
	$code = 5
}
# Case-sensitive: the Vulkan loader logs lower-case "error:" lines that are not errors. UE logs are
# not scanned.
$errors = 0
$validation = 0
# D3D12 GPU-based validation, from the line the engine prints when it turns it on (#272). Under it,
# atomics whose result is unused are lost (#277), so the line says that such results are not checked.
$gbv = $false
if (-not $ueJob) {
	$errors = @($log | Where-Object { $_ -cmatch 'ERROR:' }).Count
	$validation = @($log | Where-Object { $_ -cmatch 'Validation Error' }).Count
	$gbv = @($log | Where-Object { $_ -cmatch 'D3D12: GPU-based validation is on\.' }).Count -gt 0
}
$gbvFields = ''
if ($gbv) {
	$gbvFields = ',"d3d12_gpu_based_validation":true,"not_checked":"results that depend on atomics whose result is unused (#277)"'
}
Write-Output ('GODOTNEXT_RUNNER {"device":"' + $deviceName + '","driver":"' + $driverLabel + '","gpu_index":' + $device.GpuIndex + ',"session":' + $done.session + ',"clocks_locked":' + $lockClocks.ToString().ToLowerInvariant() + ',"errors":' + $errors + ',"validation_errors":' + $validation + ',"status":' + $code + ',"engine":"' + $engine + '"' + $gbvFields + '}')
if ($failOnValidation -and $code -eq 0 -and ($errors -ne 0 -or $validation -ne 0)) {
	[Console]::Error.WriteLine('win_session1.ps1: the run logged validation lines; the bar is 0')
	$code = 1
}
exit $code
