<#
.SYNOPSIS
    Sets Bully: Anniversary Edition's frame-rate cap (stock 30) through the WSA GL fixes module.

.DESCRIPTION
    Sets debug.bully.fps, which the module's GLES wrapper applies to the running
    game within about half a second, and writes the same value into the module's
    system.prop so it survives WSA restarts. Needs root (Magisk) and the module.

    Parts of Bully are tuned for 30 fps: on PC, cutscenes and at least one
    snowball mission misbehave at 60. Switch back to 30 if something breaks.

.EXAMPLE
    .\Set-BullyFps.ps1 60
    .\Set-BullyFps.ps1 30
#>
param(
    [Parameter(Mandatory)][ValidateRange(10, 240)][int]$Fps,
    [string]$Adb = (Join-Path $env:LOCALAPPDATA 'Android\platform-tools\adb.exe'),
    [string]$Target = '127.0.0.1:58526'
)

$ErrorActionPreference = 'Stop'
& $Adb connect $Target | Out-Null
$prop = '/data/adb/modules/wsa_egl_surfaceless/system.prop'
& $Adb -s $Target shell "setprop debug.bully.fps $Fps"
& $Adb -s $Target shell "su -c 'sed -i s/^debug.bully.fps=.*/debug.bully.fps=$Fps/ $prop && grep ^debug.bully.fps= $prop'"
if ($LASTEXITCODE) { throw "Could not update $prop - is the WSA GL fixes module installed?" }
Write-Host "Bully frame-rate cap: $Fps fps (applies to a running game within a second, and after WSA restarts)" -ForegroundColor Green
