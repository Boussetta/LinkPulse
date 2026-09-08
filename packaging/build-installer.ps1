[CmdletBinding()]
param(
    [string]$InnoCompiler = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    [string]$Version
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $repoRoot

function Invoke-Checked {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    Write-Host "> $Command $($Arguments -join ' ')" -ForegroundColor Cyan
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

if (-not $Version) {
    # Matches the release workflow: default to the nearest vMAJOR.MINOR.PATCH
    # tag reachable from HEAD, so a locally built installer reports the same
    # version as the GitHub release it corresponds to (otherwise the update
    # checker compares against a stale hardcoded "0.1.0" and always thinks a
    # newer release is available).
    $describe = git describe --tags --match "v*" --abbrev=0 2>$null
    if ($LASTEXITCODE -eq 0 -and $describe) {
        $Version = $describe -replace '^v', ''
    } else {
        $Version = "0.1.0"
        Write-Warning "No vMAJOR.MINOR.PATCH tag found reachable from HEAD; defaulting to $Version"
    }
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version '$Version' must be MAJOR.MINOR.PATCH (e.g. 0.1.0)."
}
Write-Host "Building version $Version" -ForegroundColor Cyan

Invoke-Checked "cmake" @("--preset", "msvc", "-DLINKPULSE_VERSION:STRING=$Version")
Invoke-Checked "cmake" @("--build", "--preset", "msvc-release")
Invoke-Checked "ctest" @("--preset", "msvc-release")

if (-not (Test-Path -LiteralPath $InnoCompiler)) {
    throw "Inno Setup compiler not found: $InnoCompiler"
}

Invoke-Checked $InnoCompiler @("/DMyAppVersion=$Version", "packaging\LinkPulse.iss")

$installer = Join-Path $repoRoot "build\installer\LinkPulseSetup.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer was not created: $installer"
}

Write-Host "`nInstaller created:" -ForegroundColor Green
Write-Host $installer -ForegroundColor Green
