<#
.SYNOPSIS
    Adds (or removes) an "Install to WSA" entry to Explorer's right-click menu
    for .apk, .xapk and .apks files.

.DESCRIPTION
    Copies Install-ToWSA.ps1 to %LOCALAPPDATA%\Android and registers it per user
    under HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\InstallToWSA.
    No admin rights needed. In Windows 11 the entry is under "Show more options"
    (or Shift+F10).

    Requires adb at %LOCALAPPDATA%\Android\platform-tools\adb.exe (Google
    platform-tools) or on PATH, and Developer mode on in WSA Settings.

.EXAMPLE
    .\Register-InstallToWSA.ps1
    .\Register-InstallToWSA.ps1 -Unregister
#>
param([switch]$Unregister)

$ErrorActionPreference = 'Stop'
$exts = '.apk', '.xapk', '.apks'
$dest = Join-Path $env:LOCALAPPDATA 'Android\Install-ToWSA.ps1'

if ($Unregister) {
    foreach ($ext in $exts) {
        Remove-Item "HKCU:\Software\Classes\SystemFileAssociations\$ext\shell\InstallToWSA" -Recurse -ErrorAction SilentlyContinue
    }
    Write-Host 'Removed "Install to WSA" from the right-click menu.' -ForegroundColor Green
    return
}

New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'Install-ToWSA.ps1') $dest -Force

$cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$dest`" `"%1`""
$icon = 'C:\WSA\WsaClient\WsaClient.exe,0'
foreach ($ext in $exts) {
    $key = "HKCU:\Software\Classes\SystemFileAssociations\$ext\shell\InstallToWSA"
    New-Item -Path "$key\command" -Force | Out-Null
    Set-ItemProperty -Path $key -Name '(default)' -Value 'Install to WSA'
    if (Test-Path ($icon -replace ',0$')) { Set-ItemProperty -Path $key -Name 'Icon' -Value $icon }
    Set-ItemProperty -Path "$key\command" -Name '(default)' -Value $cmd
}
Write-Host "Registered 'Install to WSA' for $($exts -join ', ') -> $dest" -ForegroundColor Green
