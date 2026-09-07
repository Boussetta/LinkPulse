<#
.SYNOPSIS
    Builds an unsigned MSIX package for local sideload testing of LinkPulse.

.DESCRIPTION
    Stages linkpulse-tray.exe, Package.appxmanifest and Assets\ into a
    layout directory, generates resources.pri, and packs it into an MSIX
    using the Windows 10/11 SDK tools (makepri.exe, makeappx.exe). Run this
    from a "Developer PowerShell for VS" or any shell where those tools are
    on PATH.

    This produces an UNSIGNED package. To install it locally you must either:
      - sign it with a locally-trusted test certificate and
        Add-AppxPackage, or
      - enable Developer Mode and use Add-AppxPackage -Register against the
        staged layout directory directly (no signing needed for that path).

    This script does not build the binary -- run the msvc-release CMake
    preset first.

.EXAMPLE
    cmake --preset msvc
    cmake --build --preset msvc-release
    .\packaging\build-msix.ps1
#>

[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\.."),
    [string]$ExePath = "$RepoRoot\out\windows\linkpulse-tray.exe",
    [string]$StagingDir = "$RepoRoot\build\msix-staging",
    [string]$OutputMsix = "$RepoRoot\build\LinkPulse.msix"
)

$ErrorActionPreference = "Stop"

foreach ($tool in "makepri.exe", "makeappx.exe") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found on PATH. Run this from a Developer PowerShell for Visual Studio, or add the Windows SDK bin directory to PATH."
    }
}

if (-not (Test-Path $ExePath)) {
    throw "$ExePath not found. Build it first: cmake --build --preset msvc-release"
}

if (Test-Path -LiteralPath $StagingDir) {
    $resolvedRepo = (Resolve-Path -LiteralPath $RepoRoot).Path
    $resolvedStaging = (Resolve-Path -LiteralPath $StagingDir).Path
    if (-not $resolvedStaging.StartsWith($resolvedRepo, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to delete staging dir outside RepoRoot: $resolvedStaging" }
    Remove-Item -LiteralPath $StagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $StagingDir | Out-Null
New-Item -ItemType Directory -Path "$StagingDir\Assets" | Out-Null

Copy-Item $ExePath "$StagingDir\linkpulse-tray.exe"
Copy-Item "$PSScriptRoot\Package.appxmanifest" "$StagingDir\AppxManifest.xml"
Copy-Item "$PSScriptRoot\Assets\*.png" "$StagingDir\Assets\"

$priConfig = "$StagingDir\priconfig.xml"
& makepri.exe createconfig /cf $priConfig /dq en-US /o
& makepri.exe new /pr $StagingDir /cf $priConfig /o

if (Test-Path $OutputMsix) {
    Remove-Item $OutputMsix -Force
}
& makeappx.exe pack /d $StagingDir /p $OutputMsix /o

Write-Host "Packed (unsigned): $OutputMsix"
Write-Host "To sideload without signing: Add-AppxPackage -Register `"$StagingDir\AppxManifest.xml`""
