<#
.SYNOPSIS
    Records WSA's diagnostic events so failed boots can be debugged. Run it in
    an admin terminal.

.DESCRIPTION
    WsaClient and WsaService log to the TraceLogging provider
    "Microsoft.Latte.Client" (GUID 30cb12f5-b71b-5a66-0484-11b8118099da): VM
    start/stop, boot progress and the reason a boot was abandoned (for example
    BootProgressReport ... shutdownReason=HeartbeatTimeout). Android writes no
    logcat when it fails early in boot, so this is the only record of it. The
    VM's kernel log is only forwarded once the VM has connected to Windows.

    Decode the result with: tracerpt <etl> -o out.xml -of XML
    (Get-WinEvent can't decode TraceLogging fields.)

    Starts an ETW session for that provider, then waits until the file named by
    -StopFile appears (or -Minutes pass, or Enter is pressed) and stops it.

.EXAMPLE
    .\Trace-WsaBoot.ps1
#>
param(
    [string]$Output = (Join-Path $env:TEMP 'wsa-trace\wsaboot.etl'),
    [string]$StopFile = (Join-Path $env:TEMP 'wsa-trace\stop'),
    [int]$Minutes = 30
)

$ErrorActionPreference = 'Stop'
$session = 'WsaBootTrace'
$provider = '{30cb12f5-b71b-5a66-0484-11b8118099da}'   # Microsoft.Latte.Client

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators')
if (-not $admin) { throw 'Run this from an admin terminal.' }

New-Item -ItemType Directory -Force (Split-Path $Output) | Out-Null
if (Test-Path $StopFile) { Remove-Item -LiteralPath $StopFile }
logman stop $session -ets 2>&1 | Out-Null   # leftover from an earlier run
logman create trace $session -p $provider 0xffffffffffffffff 0xff -o $Output -mode Sequential -max 512 -ets | Out-Null
if ($LASTEXITCODE) { throw "logman failed to start the trace ($LASTEXITCODE)" }
Write-Host "Recording WSA events to $Output" -ForegroundColor Green
Write-Host "Stops when $StopFile appears, after $Minutes minutes, or when you press Enter."

$deadline = (Get-Date).AddMinutes($Minutes)
try {
    while (-not (Test-Path $StopFile) -and (Get-Date) -lt $deadline) {
        if ([Console]::KeyAvailable -and [Console]::ReadKey($true).Key -eq 'Enter') { break }
        Start-Sleep -Milliseconds 500
    }
} finally {
    logman stop $session -ets | Out-Null
    # Let the non-admin side read it
    icacls $Output /grant "${env:USERNAME}:R" | Out-Null
    Write-Host "Stopped. Trace saved to $Output" -ForegroundColor Green
}
