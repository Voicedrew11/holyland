#requires -Version 5.1
<#
.SYNOPSIS
Runs the verified US game with fixed updates and independent presentation.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$Executable,
    [ValidateRange(0, 1000)][int]$RenderFps = 120,
    [switch]$OriginalFrames
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$settings = @{
    PS2X_KFIV_FIXED_FRAME = '1'
    PS2X_FRAME_INTERPOLATION = $(if ($OriginalFrames) { '0' } else { '1' })
    PS2X_PRESENT_FPS = [string]$RenderFps
    PS2X_INTERPOLATION_VERIFY = '0'
    PS2X_VU1_LIFT = '0'
}
if ([string]::IsNullOrWhiteSpace($env:PS2X_GS_BACKEND)) {
    $settings.PS2X_GS_BACKEND = 'vulkan'
}
$previous = @{}
try {
    foreach ($name in $settings.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process')
    }
    & (Join-Path $PSScriptRoot 'Run-KFIV.ps1') -GameDirectory $GameDirectory -Executable $Executable
}
finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
