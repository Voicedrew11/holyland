# SPDX-License-Identifier: GPL-3.0-only
# Source setup only. This script never runs a GPU test or changes a driver.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [switch]$Build,
    [string]$BuildDirectory,
    [string]$CMakePath = 'cmake',
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Jobs = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$gitCommand = (Get-Command git -ErrorAction Stop).Source
$ghCommand = (Get-Command gh -ErrorAction Stop).Source
$sourcePath = [IO.Path]::GetFullPath($SourceDirectory)
$patchPath = Join-Path $PSScriptRoot 'parallel-gs-small-transfers.patch'
$patchSha = 'D651CA44EEB7370AB7402D17255399D2C9F37A8CAA80A662FC41F63A559693A6'

function Invoke-Captured {
    param([string]$Program, [string[]]$Arguments, [switch]$AllowFailure)
    # Windows PowerShell 5.1 can promote native stderr into an ErrorRecord.
    # Inspect the exit code instead; retain both streams for actionable errors.
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $Program @Arguments 2>&1)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    $message = ($lines | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "Command failed ($code): $Program $($Arguments -join ' ')`n$message"
    }
    return [PSCustomObject]@{ ExitCode = $code; Text = $message }
}

function Get-GitText {
    param([string]$Directory, [string[]]$Arguments)
    return (Invoke-Captured $gitCommand (@('-C', $Directory) + $Arguments)).Text.Trim()
}

function Assert-Pin {
    param([string]$Directory, [string]$Pin)
    $top = Get-GitText $Directory @('rev-parse', '--show-toplevel')
    $actualTop = [IO.Path]::GetFullPath($top).TrimEnd([char[]]'\/')
    $expectedTop = [IO.Path]::GetFullPath($Directory).TrimEnd([char[]]'\/')
    if (-not [string]::Equals($actualTop, $expectedTop, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Not a standalone checkout or initialized submodule: $Directory. Existing files were preserved."
    }
    $head = Get-GitText $Directory @('rev-parse', 'HEAD')
    if ($head -ne $Pin) {
        throw "Wrong HEAD at $Directory. Expected $Pin, found $head. Use a separate pinned checkout; this script does not reset or switch an existing checkout."
    }
    Write-Host "Verified $Directory at $Pin"
}

function Ensure-Checkout {
    param([string]$Directory, [string]$Repository, [string]$Pin)
    $hasGit = Test-Path -LiteralPath (Join-Path $Directory '.git')
    if (-not $hasGit) {
        if (Test-Path -LiteralPath $Directory) {
            if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
                throw "Destination is not a directory: $Directory"
            }
            if (@(Get-ChildItem -LiteralPath $Directory -Force).Count -ne 0) {
                throw "Destination contains existing files but is not an initialized checkout: $Directory. Existing files were preserved."
            }
        }
        $parent = Split-Path -Parent $Directory
        if (-not (Test-Path -LiteralPath $parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Write-Host "Retrieving $Repository with gh (without recursive submodules)."
        Invoke-Captured $ghCommand @('repo', 'clone', $Repository, $Directory, '--', '--filter=blob:none', '--no-checkout') | Out-Null
        # Only the checkout created above is switched. Never switch existing files.
        Invoke-Captured $gitCommand @('-C', $Directory, 'checkout', '--detach', $Pin) | Out-Null
    }
    Assert-Pin $Directory $Pin
}

function Assert-Gitlink {
    param([string]$Directory, [string]$RelativePath, [string]$Pin)
    $entry = Get-GitText $Directory @('ls-tree', 'HEAD', '--', $RelativePath)
    $expected = '^160000 commit ' + [regex]::Escape($Pin) + '\s+' + [regex]::Escape($RelativePath) + '$'
    if ($entry -notmatch $expected) {
        throw "Unexpected pinned gitlink $RelativePath in $Directory`: $entry"
    }
}

if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) {
    throw "Required source patch is missing: $patchPath"
}
if ((Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash -ne $patchSha) {
    throw "Small-transfer patch hash changed. Review the new patch and update this package's pin before applying it."
}
if ($Build -and [string]::IsNullOrWhiteSpace($BuildDirectory)) {
    throw '-Build requires an explicit -BuildDirectory outside the source package.'
}

$gsPin = 'cc6184af7e0c03da603045ca371ffa5dae9b0655'
$granitePin = '16e7395f6a4858c1783dbf6f521f90b9d5f82ac5'
$volkPin = '47cddf7ed97b94118a08aacb548a411188e016cc'
$headersPin = '11d6898377797e07dbd543aaaa367e4465074597'
Ensure-Checkout $sourcePath 'Arntzen-Software/parallel-gs' $gsPin
Assert-Gitlink $sourcePath 'Granite' $granitePin
Invoke-Captured $gitCommand @('-C', $sourcePath, 'submodule', 'init', '--', 'Granite') | Out-Null
$granitePath = Join-Path $sourcePath 'Granite'
Ensure-Checkout $granitePath 'Themaister/Granite' $granitePin
Assert-Gitlink $granitePath 'third_party/volk' $volkPin
Assert-Gitlink $granitePath 'third_party/khronos/vulkan-headers' $headersPin
Invoke-Captured $gitCommand @('-C', $granitePath, 'submodule', 'init', '--',
    'third_party/volk', 'third_party/khronos/vulkan-headers') | Out-Null
Ensure-Checkout (Join-Path $granitePath 'third_party/volk') 'zeux/volk' $volkPin
Ensure-Checkout (Join-Path $granitePath 'third_party/khronos/vulkan-headers') 'KhronosGroup/Vulkan-Headers' $headersPin

$forward = Invoke-Captured $gitCommand @('-C', $sourcePath, 'apply', '--ignore-space-change', '--check', $patchPath) -AllowFailure
if ($forward.ExitCode -eq 0) {
    Invoke-Captured $gitCommand @('-C', $sourcePath, 'apply', '--ignore-space-change', $patchPath) | Out-Null
    Write-Host "Applied verified small-transfer patch $patchSha"
} else {
    $reverse = Invoke-Captured $gitCommand @('-C', $sourcePath, 'apply', '--ignore-space-change', '--reverse', '--check', $patchPath) -AllowFailure
    if ($reverse.ExitCode -ne 0) {
        throw "Patch neither applies cleanly nor matches an already applied patch. Existing edits were preserved; any missing dependencies retrieved earlier remain in place.`n$($forward.Text)`n$($reverse.Text)"
    }
    Write-Host 'Verified small-transfer patch is already applied; preserving the checkout.'
}

$dirty = Get-GitText $sourcePath @('status', '--short', '--ignore-submodules=all')
if ($dirty) {
    Write-Host "Local changes are retained (the expected transfer patch is one of them):`n$dirty"
}
if ($Build) {
    $cmakeCommand = (Get-Command $CMakePath -ErrorAction Stop).Source
    $buildPath = [IO.Path]::GetFullPath($BuildDirectory)
    if ([string]::Equals($sourcePath.TrimEnd([char[]]'\/'), $buildPath.TrimEnd([char[]]'\/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Use a separate build directory; in-source builds are not supported by this helper.'
    }
    $configure = @('-S', $sourcePath, '-B', $buildPath, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DPARALLEL_GS_STANDALONE=ON',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL')
    foreach ($option in @('GRANITE_AUDIO', 'GRANITE_BULLET', 'GRANITE_FFMPEG', 'GRANITE_FFMPEG_VULKAN',
            'GRANITE_RENDERDOC_CAPTURE', 'GRANITE_TOOLS', 'GRANITE_ISPC_TEXTURE_COMPRESSION',
            'GRANITE_ASTC_ENCODER_COMPRESSION', 'GRANITE_VULKAN_PROFILES', 'GRANITE_VULKAN_POST_MORTEM',
            'GRANITE_INSTALL_TARGETS', 'GRANITE_INSTALL_EXE_TARGETS', 'GRANITE_VULKAN_DXGI_INTEROP')) {
        $configure += "-D$option=OFF"
    }
    Write-Host (Invoke-Captured $cmakeCommand $configure).Text
    Write-Host (Invoke-Captured $cmakeCommand @('--build', $buildPath, '--config', $Configuration,
        '--target', 'parallel-gs', '--parallel', [string]$Jobs)).Text
    Write-Host "Built parallel-gs ($Configuration, MSVC DLL CRT). No GPU dispatch was run."
}
Write-Host "Prepared source: $sourcePath"
