param(
    [Parameter(Mandatory = $true)]
    [string]$ArtifactVersion
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BundleDir = Join-Path $RepoRoot "example/build/windows/x64/runner/Release"
$DistDir = Join-Path $RepoRoot "dist"
$ArtifactName = "usb_sync_example-windows-x64-$ArtifactVersion.zip"
$ArtifactPath = Join-Path $DistDir $ArtifactName

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
if (Test-Path $ArtifactPath) {
    Remove-Item $ArtifactPath -Force
}

Compress-Archive -Path (Join-Path $BundleDir "*") -DestinationPath $ArtifactPath
Write-Output $ArtifactPath
