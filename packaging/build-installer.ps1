[CmdletBinding()]
param(
    [string]$InnoCompiler = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
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

Invoke-Checked "cmake" @("--preset", "msvc")
Invoke-Checked "cmake" @("--build", "--preset", "msvc-release")
Invoke-Checked "ctest" @("--preset", "msvc-release")

if (-not (Test-Path -LiteralPath $InnoCompiler)) {
    throw "Inno Setup compiler not found: $InnoCompiler"
}

Invoke-Checked $InnoCompiler @("packaging\LinkPulse.iss")

$installer = Join-Path $repoRoot "build\installer\LinkPulseSetup.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer was not created: $installer"
}

Write-Host "`nInstaller created:" -ForegroundColor Green
Write-Host $installer -ForegroundColor Green
