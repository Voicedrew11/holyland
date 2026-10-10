#requires -Version 5.1
<#
.SYNOPSIS
Runs the native KFIV runner in an isolated, hidden Windows test session.
.DESCRIPTION
The supplied game directory is read only. DATA is linked with a junction;
SLUS_203.18, SYSTEM.CNF, IOPRP224.IMG and IOP are copied. The test has its
own mc0/mc1 directories. Every run directory must be new or empty, and is
retained for inspection. No directory cleanup is performed.
.EXAMPLE
.\Test-KFIV.ps1 -Executable C:\KFIV\ps2EntryRunner.exe -GameDirectory C:\KFIV -RunDirectory C:\KFIV-tests\title -ExitTick 900
.EXAMPLE
.\Test-KFIV.ps1 -Executable C:\KFIV\ps2EntryRunner.exe -GameDirectory C:\KFIV -ExitTick 10500 -DumpEvery 150 -Stats -InputScript '1100:START:10,1500:CROSS:10,1900:CROSS:10' -Lockstep
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$RunDirectory,
    [ValidateRange(1, 2147483647)][int]$ExitTick = 900,
    [ValidateRange(0, 2147483647)][int]$DumpEvery = 0,
    [string]$InputScript = '',
    [switch]$Stats,
    [switch]$ThreadsAtExit,
    [switch]$Lockstep,
    [switch]$CopyMemoryCards,
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 1800,
    [ValidateRange(0, 60)][int]$GracefulExitSeconds = 10,
    [hashtable]$Environment = @{},
    [switch]$SkipFinalFrameCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ExistingPath {
    param([string]$Path, [ValidateSet('Leaf', 'Container')][string]$Kind)
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "Required $Kind path does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

# Start-Process joins its ArgumentList on Windows. Quote the ELF explicitly.
function ConvertTo-WindowsArgument {
    param([string]$Value)
    $escaped = $Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

$runnerPath = Get-ExistingPath $Executable Leaf
$sourceGamePath = Get-ExistingPath $GameDirectory Container
$dataPath = Get-ExistingPath (Join-Path $sourceGamePath 'DATA') Container
foreach ($discFile in @('SLUS_203.18', 'SYSTEM.CNF', 'IOPRP224.IMG')) {
    $null = Get-ExistingPath (Join-Path $sourceGamePath $discFile) Leaf
}
$null = Get-ExistingPath (Join-Path $sourceGamePath 'IOP') Container

if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $runName = 'run-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'), [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $RunDirectory = Join-Path (Join-Path ([IO.Path]::GetTempPath()) 'KFIV-tests') $runName
}
$runPath = [IO.Path]::GetFullPath($RunDirectory)
$sourcePrefix = $sourceGamePath.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($runPath.Equals($sourceGamePath, [StringComparison]::OrdinalIgnoreCase) -or
    $runPath.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory must be outside the supplied GameDirectory to keep source game data read only.'
}
if (Test-Path -LiteralPath $runPath) {
    if (-not (Test-Path -LiteralPath $runPath -PathType Container)) {
        throw "RunDirectory is not a directory: $runPath"
    }
    if ((Get-ChildItem -LiteralPath $runPath -Force | Measure-Object).Count -ne 0) {
        throw "RunDirectory must be new or empty; existing files are retained: $runPath"
    }
}

$testGamePath = Join-Path $runPath 'game'
$controlPath = Join-Path $runPath 'ctrl'
$stdoutPath = Join-Path $runPath 'stdout.log'
$stderrPath = Join-Path $runPath 'stderr.log'
$finalFramePath = Join-Path $runPath 'final.png'
$resultPath = Join-Path $runPath 'result.json'
$managedEnvironment = @{
    PS2X_HIDDEN = '1'
    PS2X_DUMP_DIR = $runPath
    PS2X_EXIT_TICK = [string]$ExitTick
    PS2X_CTRL = $controlPath
    PS2X_DUMP_EVERY = [string]$DumpEvery
    PS2X_INPUT = $InputScript
    PS2X_STATS = $(if ($Stats) { '1' } else { '0' })
    PS2X_THREADS_AT_EXIT = $(if ($ThreadsAtExit) { '1' } else { '0' })
    PS2X_GS_LOCKSTEP = $(if ($Lockstep) { '1' } else { '0' })
}
foreach ($key in $Environment.Keys) {
    if ([string]$key -notmatch '^PS2X_[A-Z0-9_]+$') {
        throw "Extra environment variables must have a PS2X_ name: $key"
    }
    if ($managedEnvironment.ContainsKey($key)) {
        throw "Use the named script parameter for the managed variable $key."
    }
    $managedEnvironment[$key] = $Environment[$key]
}

$null = New-Item -ItemType Directory -Path $testGamePath -Force
$null = New-Item -ItemType Junction -Path (Join-Path $testGamePath 'DATA') -Target $dataPath
foreach ($discFile in @('SLUS_203.18', 'SYSTEM.CNF', 'IOPRP224.IMG', 'IOP')) {
    Copy-Item -LiteralPath (Join-Path $sourceGamePath $discFile) -Destination $testGamePath -Recurse
}
foreach ($cardName in @('mc0', 'mc1')) {
    $sourceCardPath = Join-Path $sourceGamePath $cardName
    if ($CopyMemoryCards -and (Test-Path -LiteralPath $sourceCardPath -PathType Container)) {
        Copy-Item -LiteralPath $sourceCardPath -Destination $testGamePath -Recurse
    }
    else {
        $null = New-Item -ItemType Directory -Path (Join-Path $testGamePath $cardName)
    }
}

$previousEnvironment = @{}
$testProcess = $null
$runTimer = [Diagnostics.Stopwatch]::StartNew()
$startedUtc = [DateTime]::UtcNow.ToString('o')
$timedOut = $false
$forcedStop = $false
$runnerExitCode = $null
$runError = $null
try {
    foreach ($key in $managedEnvironment.Keys) {
        $previousEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        if ($null -eq $managedEnvironment[$key]) {
            Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
        }
        else {
            [Environment]::SetEnvironmentVariable($key, $managedEnvironment[$key], 'Process')
        }
    }
    $launchArguments = @{
        FilePath = $runnerPath
        ArgumentList = (ConvertTo-WindowsArgument (Join-Path $testGamePath 'SLUS_203.18'))
        WorkingDirectory = $testGamePath
        WindowStyle = 'Hidden'
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
        PassThru = $true
    }
    $testProcess = Start-Process @launchArguments
    # Retain the native handle before polling. Windows PowerShell 5.1 can
    # otherwise expose a null ExitCode for a redirected Start-Process child.
    $null = $testProcess.Handle
    Write-Host "Native KFIV test PID $($testProcess.Id); control file: $controlPath"
    while (-not $testProcess.WaitForExit(500)) {
        if ($runTimer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            $timedOut = $true
            # Give this specific test process a chance to stop itself.
            Set-Content -LiteralPath $controlPath -Value 'quit' -Encoding ASCII
            $stopTimer = [Diagnostics.Stopwatch]::StartNew()
            while (-not $testProcess.WaitForExit(500)) {
                if ($stopTimer.Elapsed.TotalSeconds -ge $GracefulExitSeconds) {
                    $forcedStop = $true
                    $testProcess.Kill()
                    $null = $testProcess.WaitForExit(5000)
                    break
                }
            }
            break
        }
    }
    if ($testProcess.HasExited) {
        $runnerExitCode = $testProcess.ExitCode
    }
}
catch {
    $runError = $_.Exception.Message
    if ($null -ne $testProcess -and -not $testProcess.HasExited) {
        $forcedStop = $true
        $testProcess.Kill()
        $null = $testProcess.WaitForExit(5000)
    }
}
finally {
    foreach ($key in $previousEnvironment.Keys) {
        if ($null -eq $previousEnvironment[$key]) {
            # PS/.NET binding can turn $null into an empty string; the
            # environment provider explicitly removes an originally absent key.
            Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
        }
        else {
            [Environment]::SetEnvironmentVariable($key, $previousEnvironment[$key], 'Process')
        }
    }
    $runTimer.Stop()
    if ($null -ne $testProcess) {
        $testProcess.Dispose()
    }
}

$result = [pscustomobject][ordered]@{
    Executable = $runnerPath
    SourceGameDirectory = $sourceGamePath
    RunDirectory = $runPath
    WorkingDirectory = $testGamePath
    StartedUtc = $startedUtc
    DurationSeconds = [Math]::Round($runTimer.Elapsed.TotalSeconds, 3)
    ExitTick = $ExitTick
    InputScript = $InputScript
    DumpEvery = $DumpEvery
    Stats = [bool]$Stats
    Lockstep = [bool]$Lockstep
    CopiedMemoryCards = [bool]$CopyMemoryCards
    TimedOut = $timedOut
    ForcedStop = $forcedStop
    ExitCode = $runnerExitCode
    Error = $runError
    StdoutLog = $stdoutPath
    StderrLog = $stderrPath
    ControlFile = $controlPath
    FinalFrame = $finalFramePath
    FinalFrameExists = (Test-Path -LiteralPath $finalFramePath -PathType Leaf)
    FrameCount = (Get-ChildItem -LiteralPath $runPath -Filter '*.png' -File | Measure-Object).Count
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result
if ($runError) { throw "KFIV test failed: $runError See $resultPath" }
if ($timedOut) { throw "KFIV test timed out after $TimeoutSeconds seconds. See $resultPath" }
if ($null -eq $runnerExitCode -or $runnerExitCode -ne 0) {
    throw "KFIV runner exited with code $runnerExitCode. See $stderrPath"
}
if (-not $SkipFinalFrameCheck -and -not $result.FinalFrameExists) {
    throw "KFIV runner exited without the expected final.png. See $stderrPath"
}
