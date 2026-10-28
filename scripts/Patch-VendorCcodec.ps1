<#
.SYNOPSIS
    Enables Android's software video decoders in a snickler WSA-Patched vendor image.

.DESCRIPTION
    snickler/WSA-Patched 0.0.2 ships a vendor.vhdx whose build.prop sets
    debug.stagefright.ccodec=0. That disables Codec2 (where the c2.android.*
    software decoders live), while the OMX decoders it falls back to have no
    service behind them, so Android ends up with no video decoders at all.

    This flips every occurrence of "debug.stagefright.ccodec=0" to "=4" in place.
    Same length, so it is a plain byte patch: ext4 doesn't checksum file data and
    VHDX doesn't checksum payload blocks. WSA must be shut down first.

.PARAMETER Image
    The vendor image to patch. Default: C:\WSA\vendor.vhdx

.PARAMETER Backup
    Where to copy the unmodified image first (skipped if it already exists).

.PARAMETER Revert
    Restore the backup over the image instead of patching.

.EXAMPLE
    .\Patch-VendorCcodec.ps1
    .\Patch-VendorCcodec.ps1 -Revert
#>
param(
    [string]$Image = 'C:\WSA\vendor.vhdx',
    [string]$Backup = 'C:\WSA-original-images-nogapps\vendor.snickler-0.0.2.vhdx',
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Image)) { throw "Image not found: $Image" }
# The image must not be in use by a running WSA VM
try { ([IO.File]::Open($Image, 'Open', 'ReadWrite', 'None')).Close() }
catch { throw "$Image is in use. Turn WSA off first (WSA Settings > Turn off, or: WsaClient /shutdown) and wait for vmmemWSA to exit." }

if ($Revert) {
    if (-not (Test-Path $Backup)) { throw "Backup not found: $Backup" }
    Copy-Item $Backup $Image -Force
    Write-Host "Restored $Backup -> $Image" -ForegroundColor Green
    return
}

$prefix = [Text.Encoding]::ASCII.GetBytes('debug.stagefright.ccodec=')
$bytes = [IO.File]::ReadAllBytes($Image)

# Find every "debug.stagefright.ccodec=<digit>"
$hits = [System.Collections.Generic.List[long]]::new()
$first = $prefix[0]
for ($i = 0; $i -le $bytes.Length - $prefix.Length - 1; $i++) {
    if ($bytes[$i] -ne $first) { continue }
    $match = $true
    for ($j = 1; $j -lt $prefix.Length; $j++) { if ($bytes[$i + $j] -ne $prefix[$j]) { $match = $false; break } }
    if ($match) { $hits.Add($i) }
}
if ($hits.Count -eq 0) { throw 'No debug.stagefright.ccodec setting found - is this a WSA vendor image?' }

Write-Host ("Found $($hits.Count) occurrence(s): " + (($hits | ForEach-Object { "offset $_ = $([char]$bytes[$_ + $prefix.Length])" }) -join ', '))
$toPatch = @($hits | Where-Object { [char]$bytes[$_ + $prefix.Length] -eq '0' })
if ($toPatch.Count -eq 0) { Write-Host 'Nothing to do - already patched.' -ForegroundColor Green; return }
$bytes = $null  # release memory before writing

if (-not (Test-Path $Backup)) {
    New-Item -ItemType Directory -Force (Split-Path $Backup) | Out-Null
    Copy-Item $Image $Backup
    Write-Host "Backup: $Backup"
}

$fs = [IO.File]::Open($Image, 'Open', 'ReadWrite', 'None')
try {
    foreach ($off in $toPatch) {
        $fs.Position = $off + $prefix.Length
        if ($fs.ReadByte() -ne [byte][char]'0') { throw "Unexpected byte at offset $off" }
        $fs.Position = $off + $prefix.Length
        $fs.WriteByte([byte][char]'4')
        Write-Host "Patched offset $off"
    }
} finally { $fs.Close() }

# Verify by reading build.prop back out of the image (7-Zip reads ext4 inside VHDX)
$7z = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (Test-Path $7z) {
    $chk = Join-Path $env:TEMP ('vendor-check-' + [guid]::NewGuid().ToString('N'))
    & $7z e $Image "-o$chk" -y build.prop | Out-Null
    $line = (Select-String -Path (Join-Path $chk 'build.prop') -Pattern 'stagefright\.ccodec').Line
    Remove-Item $chk -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "build.prop now: $line" -ForegroundColor Green
} else {
    Write-Host 'Done. (Install 7-Zip to have this script verify build.prop.)' -ForegroundColor Green
}
Write-Host 'Start WSA, then clear YouTube''s storage once so it re-checks the decoders.'
