# Install an .apk / .xapk / .apks into Windows Subsystem for Android via adb.
# Used by the Explorer right-click entry "Install to WSA".
param(
    [Parameter(Mandatory)][string]$Path,
    [string]$Adb = (Join-Path $env:LOCALAPPDATA 'Android\platform-tools\adb.exe'),
    [string]$Target = '127.0.0.1:58526'
)

# 'Continue' so adb's stderr (e.g. install failures) is captured, not thrown;
# file operations below use -ErrorAction Stop explicitly.
$ErrorActionPreference = 'Continue'
if (-not (Test-Path $Adb)) { $Adb = (Get-Command adb -ErrorAction SilentlyContinue).Source }
$adb = $Adb
$target = $Target

function Done($msg, $ok) {
    Write-Host ''
    if ($ok) { Write-Host $msg -ForegroundColor Green } else { Write-Host $msg -ForegroundColor Red }
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit ([int](-not $ok))
}

try {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    Write-Host "Installing $($file.Name) to WSA..." -ForegroundColor Cyan
    if (-not $adb) { Done 'adb not found. Install Google platform-tools to %LOCALAPPDATA%\Android\platform-tools or add adb to PATH.' $false }

    # Start WSA if it isn't running, then wait until Android has finished booting
    if (-not (Get-Process vmmemWSA -ErrorAction SilentlyContinue)) {
        Write-Host 'Starting WSA...'
        Start-Process WsaClient -ArgumentList '/launch', 'wsa://system'
    }
    $deadline = (Get-Date).AddSeconds(120)
    do {
        & $adb connect $target 2>&1 | Out-Null
        $booted = (& $adb -s $target shell getprop sys.boot_completed 2>$null) -match '1'
        if (-not $booted) { Start-Sleep -Seconds 3 }
    } until ($booted -or (Get-Date) -gt $deadline)
    if (-not $booted) {
        $state = (& $adb devices 2>&1 | Select-String '58526').Line
        if ($state -match 'unauthorized') { Done 'WSA is asking for permission: click "Allow" on the USB debugging prompt, then try again.' $false }
        Done 'Could not connect to WSA. Make sure Developer mode is on in WSA Settings > Advanced settings.' $false
    }

    switch ($file.Extension.ToLower()) {
        '.apk' {
            $out = & $adb -s $target install -r $file.FullName 2>&1
        }
        { $_ -in '.xapk', '.apks' } {
            $tmp = Join-Path $env:TEMP ("wsa-install-" + [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory $tmp -ErrorAction Stop | Out-Null
            try {
                # Expand-Archive rejects non-.zip extensions, so use .NET directly
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                [IO.Compression.ZipFile]::ExtractToDirectory($file.FullName, $tmp)
                $apks = Get-ChildItem $tmp -Recurse -Filter *.apk | Select-Object -ExpandProperty FullName
                if (-not $apks) { Done 'No .apk files found inside the bundle.' $false }
                Write-Host "Bundle contains $($apks.Count) APK(s)."
                $out = & $adb -s $target install-multiple -r @apks 2>&1
                # Game data (OBB), if the bundle includes it
                $obb = Get-ChildItem $tmp -Recurse -Directory -Filter obb | Select-Object -First 1
                if ($obb -and ($out -match 'Success')) {
                    foreach ($pkgDir in Get-ChildItem $obb.FullName -Directory) {
                        Write-Host "Copying game data for $($pkgDir.Name)..."
                        & $adb -s $target push $pkgDir.FullName /sdcard/Android/obb/ 2>&1 | Out-Null
                    }
                }
            } finally {
                Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        default { Done "Unsupported file type: $($file.Extension)" $false }
    }

    # Stringify each line so adb's stderr isn't shown as a PowerShell error record
    $text = (($out | ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" } } | Where-Object { $_ -and $_ -ne 'System.Management.Automation.RemoteException' -and $_.Trim() }) -join "`n").Trim()
    Write-Host $text
    if ($text -match 'Success') { Done 'Installed. The app is in your Start menu.' $true }
    if ($text -match 'NO_MATCHING_ABIS') { Done 'This app has no 64-bit ARM version, so it cannot run on Snapdragon X.' $false }
    if ($text -match 'apkm|encrypted') { Done 'Encrypted bundle (.apkm) - use the APKMirror Installer app instead.' $false }
    Done 'Install failed - see the message above.' $false
}
catch {
    Done "Error: $($_.Exception.Message)" $false
}
