$ErrorActionPreference = "Stop"

$Target = "D:\Espressif\project\remotecontrol7andEncoder"
$Source = Split-Path -Parent $MyInvocation.MyCommand.Path

if (Test-Path $Target) {
    throw "Cílový adresář už existuje: $Target"
}

Copy-Item -Path $Source -Destination $Target -Recurse
Remove-Item -Path (Join-Path $Target "create_project.ps1") -ErrorAction SilentlyContinue

Write-Host "Projekt vytvořen v $Target"
Write-Host "Dále spusť:"
Write-Host "  cd $Target"
Write-Host "  idf.py set-target esp32c6"
Write-Host "  idf.py build"
