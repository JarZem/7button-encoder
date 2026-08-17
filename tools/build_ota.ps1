param(
    [string]$BuildDir = "build",
    [string]$DistDir = "dist"
)

$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $projectRoot

idf.py build
if ($LASTEXITCODE -ne 0) {
    throw "idf.py build failed with exit code $LASTEXITCODE"
}

$appBin = Join-Path $projectRoot "$BuildDir\remotecontrol7andEncoder.bin"
if (!(Test-Path -LiteralPath $appBin)) {
    throw "Application BIN not found: $appBin"
}

$partitionFile = Join-Path $projectRoot "partitions_ota_16mb.csv"
$otaLine = Get-Content -LiteralPath $partitionFile |
    Where-Object { $_ -match '^\s*ota_0\s*,' } |
    Select-Object -First 1
if (-not $otaLine) {
    throw "ota_0 partition not found in $partitionFile"
}

$parts = $otaLine -split ',' | ForEach-Object { $_.Trim() }
$slotSizeText = $parts[4]
$slotSize = if ($slotSizeText.StartsWith("0x")) {
    [Convert]::ToInt64($slotSizeText.Substring(2), 16)
} else {
    [Convert]::ToInt64($slotSizeText, 10)
}

$binSize = (Get-Item -LiteralPath $appBin).Length
if ($binSize -gt $slotSize) {
    throw "Application BIN size $binSize does not fit OTA slot $slotSize"
}

$descriptionPath = Join-Path $projectRoot "$BuildDir\project_description.json"
$version = "unknown"
if (Test-Path -LiteralPath $descriptionPath) {
    $description = Get-Content -LiteralPath $descriptionPath -Raw | ConvertFrom-Json
    if ($description.project_version) {
        $version = $description.project_version
    }
}

$distPath = Join-Path $projectRoot $DistDir
New-Item -ItemType Directory -Force -Path $distPath | Out-Null

$plainOut = Join-Path $distPath "remotecontrol7andEncoder.bin"
$versionOut = Join-Path $distPath "remotecontrol7andEncoder-$version.bin"
Copy-Item -LiteralPath $appBin -Destination $plainOut -Force
Copy-Item -LiteralPath $appBin -Destination $versionOut -Force

$hash = Get-FileHash -LiteralPath $plainOut -Algorithm SHA256
$shaPath = "$plainOut.sha256"
"$($hash.Hash.ToLower())  remotecontrol7andEncoder.bin" | Set-Content -LiteralPath $shaPath -NoNewline

Write-Host "OTA firmware: $plainOut"
Write-Host "Versioned copy: $versionOut"
Write-Host "SHA-256: $shaPath"
Write-Host "Size: $binSize bytes"
Write-Host "OTA slot: $slotSize bytes"
Write-Host "Free in slot: $($slotSize - $binSize) bytes"
Write-Host "Copy this file to Home Assistant: /config/www/firmware/remotecontrol7andEncoder.bin"
