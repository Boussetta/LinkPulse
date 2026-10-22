[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Path,
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

# Authenticode-signs each file with the PFX certificate supplied through the
# WINDOWS_SIGNING_CERT_PFX_BASE64 and WINDOWS_SIGNING_CERT_PASSWORD environment
# variables (repository secrets in the release workflow). Without them the files
# are left unsigned and a warning is emitted, so forks and local builds still work;
# LinkPulse's in-app updater refuses to run unsigned installers.

$ErrorActionPreference = "Stop"

function Write-SigningWarning {
    param([string]$Message)
    if ($env:GITHUB_ACTIONS -eq "true") {
        Write-Host "::warning title=Unsigned release::$Message"
    } else {
        Write-Warning $Message
    }
}

$certificate = $env:WINDOWS_SIGNING_CERT_PFX_BASE64
if ([string]::IsNullOrWhiteSpace($certificate)) {
    Write-SigningWarning ("WINDOWS_SIGNING_CERT_PFX_BASE64 is not set; leaving " +
        "$($Path -join ', ') unsigned. The in-app updater will open the release page " +
        "instead of running this installer.")
    exit 0
}

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        # Prefer the newest SDK version directory that ships an x64 signtool.
        $candidate = Get-ChildItem -LiteralPath $kitsRoot -Directory |
            Where-Object { $_.Name -match '^\d+(\.\d+){3}$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
        if ($candidate) {
            return $candidate
        }
        $legacy = Join-Path $kitsRoot "x64\signtool.exe"
        if (Test-Path -LiteralPath $legacy) {
            return $legacy
        }
    }
    throw "signtool.exe not found; install the Windows 10/11 SDK."
}

$signTool = Find-SignTool
Write-Host "Using signtool: $signTool" -ForegroundColor Cyan

$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$pfxPath = Join-Path $tempRoot ("linkpulse-signing-" + [guid]::NewGuid().ToString("N") + ".pfx")
try {
    [System.IO.File]::WriteAllBytes($pfxPath, [System.Convert]::FromBase64String($certificate))
    foreach ($file in $Path) {
        if (-not (Test-Path -LiteralPath $file)) {
            throw "File to sign not found: $file"
        }
        $arguments = @("sign", "/fd", "SHA256", "/f", $pfxPath)
        if (-not [string]::IsNullOrEmpty($env:WINDOWS_SIGNING_CERT_PASSWORD)) {
            $arguments += @("/p", $env:WINDOWS_SIGNING_CERT_PASSWORD)
        }
        $arguments += @("/tr", $TimestampUrl, "/td", "SHA256", $file)
        & $signTool @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "signtool sign failed for $file with exit code $LASTEXITCODE"
        }
        & $signTool verify /pa $file
        if ($LASTEXITCODE -ne 0) {
            throw "signtool verify failed for $file with exit code $LASTEXITCODE"
        }
        Write-Host "Signed $file" -ForegroundColor Green
    }
} finally {
    if (Test-Path -LiteralPath $pfxPath) {
        Remove-Item -LiteralPath $pfxPath -Force
    }
}
