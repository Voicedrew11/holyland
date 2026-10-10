#requires -Version 5.1
<#
.SYNOPSIS
Incrementally builds ps2EntryRunner and copies its native runtime files.
.DESCRIPTION
Uses an already configured CMake build tree. It does not regenerate game
code, configure a new build, replace game data, copy save files, or remove
anything. An FFmpeg-enabled build also stages its adjacent runtime DLLs.
CMake is discovered from CMakeCache.txt, then PATH, unless supplied.
.EXAMPLE
.\Rebuild-KFIV.ps1 -BuildDirectory C:\KFIV-dev\runner-build -GameDirectory C:\KFIV
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$CMakePath,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')][string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 256)][int]$Parallel = [Math]::Min(8, [Environment]::ProcessorCount),
    [string]$BuiltExecutable,
    [string]$LogDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-WindowsArgument {
    param([string]$Value)
    $escaped = $Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) {
    throw "BuildDirectory does not exist: $BuildDirectory"
}
if (-not (Test-Path -LiteralPath $GameDirectory -PathType Container)) {
    throw "GameDirectory does not exist: $GameDirectory"
}
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).ProviderPath
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "An already configured CMake build is required: $cachePath"
}
if ([string]::IsNullOrWhiteSpace($CMakePath)) {
    $cacheCommand = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_COMMAND:INTERNAL=(.*)$' | Select-Object -First 1
    if ($null -ne $cacheCommand) {
        $candidate = $cacheCommand.Matches[0].Groups[1].Value
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $CMakePath = $candidate }
    }
    if ([string]::IsNullOrWhiteSpace($CMakePath)) {
        $cmakeCommand = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $cmakeCommand) { $CMakePath = $cmakeCommand.Source }
    }
}
if ([string]::IsNullOrWhiteSpace($CMakePath) -or -not (Test-Path -LiteralPath $CMakePath -PathType Leaf)) {
    throw 'CMake could not be found in the build cache or PATH. Supply -CMakePath.'
}
$cmakeExecutable = (Resolve-Path -LiteralPath $CMakePath).ProviderPath
if ([string]::IsNullOrWhiteSpace($BuiltExecutable)) {
    # Visual Studio multi-configuration layout used by this Windows build.
    $BuiltExecutable = Join-Path (Join-Path (Join-Path $buildPath 'ps2xRuntime') $Configuration) 'ps2EntryRunner.exe'
}
$artifactPath = [IO.Path]::GetFullPath($BuiltExecutable)
$destinationPath = Join-Path $gamePath 'ps2EntryRunner.exe'
if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    $LogDirectory = Join-Path $buildPath ('kfiv-rebuild-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'), [Guid]::NewGuid().ToString('N').Substring(0, 8))
}
$logPath = [IO.Path]::GetFullPath($LogDirectory)
if (Test-Path -LiteralPath $logPath) {
    if (-not (Test-Path -LiteralPath $logPath -PathType Container) -or
        (Get-ChildItem -LiteralPath $logPath -Force | Measure-Object).Count -ne 0) {
        throw "LogDirectory must be new or empty: $logPath"
    }
}
$null = New-Item -ItemType Directory -Path $logPath -Force
$stdoutPath = Join-Path $logPath 'stdout.log'
$stderrPath = Join-Path $logPath 'stderr.log'
$arguments = @('--build', $buildPath, '--config', $Configuration, '--target', 'ps2EntryRunner', '--parallel', [string]$Parallel)
$argumentString = ($arguments | ForEach-Object { ConvertTo-WindowsArgument $_ }) -join ' '
$buildProcess = $null
$buildExitCode = $null
try {
    $buildProcess = Start-Process -FilePath $cmakeExecutable -ArgumentList $argumentString -WorkingDirectory $buildPath -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    $null = $buildProcess.Handle
    Write-Host "Incremental CMake build PID $($buildProcess.Id); logs: $logPath"
    while (-not $buildProcess.WaitForExit(500)) { }
    $buildExitCode = $buildProcess.ExitCode
}
finally {
    if ($null -ne $buildProcess) { $buildProcess.Dispose() }
}
if ($null -eq $buildExitCode -or $buildExitCode -ne 0) {
    throw "CMake build exited with code $buildExitCode. Inspect $stdoutPath and $stderrPath"
}
if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
    throw "Build succeeded but the runner is missing: $artifactPath. Supply -BuiltExecutable for a different generator layout."
}
$runtimeDlls = @()
if (Select-String -LiteralPath $cachePath -Pattern '^PS2X_ENABLE_FFMPEG:BOOL=ON$' -Quiet) {
    $runtimeDlls = @(Get-ChildItem -LiteralPath (Split-Path -Parent $artifactPath) -Filter '*.dll' -File)
    if ($runtimeDlls.Count -eq 0) {
        throw 'FFmpeg is enabled but no runtime DLLs were staged beside the built executable.'
    }
}
foreach ($runtimeDll in $runtimeDlls) {
    $dllDestination = Join-Path $gamePath $runtimeDll.Name
    if ($runtimeDll.FullName -ne $dllDestination) {
        Copy-Item -LiteralPath $runtimeDll.FullName -Destination $dllDestination -Force
    }
}
if ($artifactPath -ne $destinationPath) {
    Copy-Item -LiteralPath $artifactPath -Destination $destinationPath -Force
}
[pscustomobject]@{
    Executable = $destinationPath
    BuiltExecutable = $artifactPath
    SHA256 = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
    Configuration = $Configuration
    RuntimeDllCount = $runtimeDlls.Count
    BuildExitCode = $buildExitCode
    StdoutLog = $stdoutPath
    StderrLog = $stderrPath
}
