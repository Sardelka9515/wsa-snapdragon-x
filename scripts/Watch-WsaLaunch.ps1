<#
.SYNOPSIS
    Logs what starts WSA: every new WsaClient/WsaService/vmmemWSA process with
    its command line, its parent chain and the window that was in front.

.DESCRIPTION
    WsaClient's command line shows how it was started: /launch wsa://<pkg> (an
    app shortcut or Install-ToWSA.ps1), /deeplink (a link an Android app has
    claimed), -Embedding or -ToastActivated (COM activation, e.g. clicking a
    WSA notification), /partiallyrunning or /restart (a startup task, or
    WsaClient restarting itself). The parent chain shows who asked for it.

    Doesn't need admin. Uses a WMI creation event polled every second, so a
    parent that exits within a second may already be gone; its PID is still
    logged. Runs until the file named by -StopFile appears or -Hours pass.

.EXAMPLE
    Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', '.\Watch-WsaLaunch.ps1'
    # later: New-Item $env:TEMP\wsa-trace\stop-watch; Get-Content $env:TEMP\wsa-trace\launches.log
#>
param(
    [string]$Output = (Join-Path $env:TEMP 'wsa-trace\launches.log'),
    [string]$StopFile = (Join-Path $env:TEMP 'wsa-trace\stop-watch'),
    [int]$Hours = 72
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force (Split-Path $Output) | Out-Null
if (Test-Path $StopFile) { Remove-Item -LiteralPath $StopFile }

Add-Type -Namespace Win32 -Name Fg -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
'@

function Write-Log([string]$line) { Add-Content -LiteralPath $Output -Value $line -Encoding UTF8 }

function Get-Foreground {
    $fgPid = 0
    [void][Win32.Fg]::GetWindowThreadProcessId([Win32.Fg]::GetForegroundWindow(), [ref]$fgPid)
    $p = Get-Process -Id $fgPid -ErrorAction SilentlyContinue
    if ($p) { "$($p.ProcessName) ($fgPid) '$($p.MainWindowTitle)'" } else { "pid $fgPid" }
}

$query = "SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process' AND " +
         "(TargetInstance.Name LIKE 'Wsa%' OR TargetInstance.Name = 'vmmemWSA')"
Register-CimIndicationEvent -Query $query -SourceIdentifier WsaLaunch | Out-Null
Write-Log "=== watching since $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (pid $PID)"

$deadline = (Get-Date).AddHours($Hours)
try {
    while (-not (Test-Path $StopFile) -and (Get-Date) -lt $deadline) {
        $ev = Wait-Event -SourceIdentifier WsaLaunch -Timeout 5
        if (-not $ev) { continue }
        Remove-Event -EventIdentifier $ev.EventIdentifier
        $p = $ev.SourceEventArgs.NewEvent.TargetInstance
        $lines = @("$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $($p.Name) ($($p.ProcessId))  $($p.CommandLine)",
                   "    foreground: $(Get-Foreground)")
        # Walk up the parents; stop at PID reuse (a "parent" younger than its child)
        $child = $p
        for ($i = 0; $i -lt 5 -and $child.ParentProcessId; $i++) {
            $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($child.ParentProcessId)" -ErrorAction SilentlyContinue
            if (-not $parent -or $parent.CreationDate -gt $child.CreationDate) {
                $lines += "    parent $($child.ParentProcessId): (exited)"
                break
            }
            $lines += "    parent $($parent.ProcessId): $($parent.Name)  $($parent.CommandLine)"
            $child = $parent
        }
        Write-Log ($lines -join "`r`n")
    }
} finally {
    Unregister-Event -SourceIdentifier WsaLaunch -ErrorAction SilentlyContinue
    Write-Log "=== stopped $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
}
