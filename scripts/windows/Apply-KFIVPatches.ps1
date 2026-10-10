#requires -Version 5.1
<#
.SYNOPSIS
Applies the pinned Holyland source patches to an existing PS2Recomp checkout.
.DESCRIPTION
Uses the shared Python helper. It preserves local changes, records completed
patches in Git metadata, and rejects a checkout at another commit. ToolsOnly
includes ps2xRecomp paths only; apply the full series before a Windows build.
.EXAMPLE
.\Apply-KFIVPatches.ps1 -CheckoutPath C:\KFIV-dev\PS2Recomp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CheckoutPath,
    [string]$PatchDirectory = (Join-Path $PSScriptRoot '..\..\patches'),
    [string]$Base = 'c5a9d02',
    [string]$PythonExecutable = 'python',
    [switch]$ToolsOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$helperPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\apply-patches.py')).ProviderPath
$arguments = @($helperPath, $CheckoutPath, '--patch-dir', $PatchDirectory, '--base', $Base)
if ($ToolsOnly) { $arguments += '--tools-only' }
& $PythonExecutable @arguments
if ($LASTEXITCODE -ne 0) { throw "Patch application failed with exit code $LASTEXITCODE." }
