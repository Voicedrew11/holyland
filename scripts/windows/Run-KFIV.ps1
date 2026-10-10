#requires -Version 5.1
<#
.SYNOPSIS
Opens the native runner with the extracted game directory as its working directory.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$Executable
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$gamePath = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $gamePath 'ps2EntryRunner.exe'
}
$runnerPath = (Resolve-Path -LiteralPath $Executable).ProviderPath
$elfPath = Join-Path $gamePath 'SLUS_203.18'
if (-not (Test-Path -LiteralPath $elfPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $gamePath 'DATA') -PathType Container)) {
    throw 'GameDirectory must contain the extracted US disc files (SLUS_203.18 and DATA).'
}
$quotedElf = '"' + ($elfPath -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
Start-Process -FilePath $runnerPath -ArgumentList $quotedElf -WorkingDirectory $gamePath -WindowStyle Normal
