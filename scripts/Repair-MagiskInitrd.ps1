<#
.SYNOPSIS
    Repairs the Magisk boot image (Tools\initrd.img) in WSABuilds' arm64 Magisk
    builds, so WSA boots with root.

.DESCRIPTION
    In WSABuilds' 2407.40000.4.0 arm64 Magisk builds, the boot hook "lspinit" in
    initrd.img has had every CRLF byte pair (0D 0A) turned into LF (0A), as Git
    does to files it treats as text. The file is 3 bytes short and dies with
    "Illegal instruction", so the VM never contacts Windows and WSA gives up
    after 2 minutes (WsaClient crash 0xc0000409).

    This replaces lspinit with the official build from LSPosed/MagiskOnWSALocal
    (pinned commit, SHA-256 checked) and writes a fixed initrd. It also refuses
    to touch an lspinit that doesn't match the known damage.

.PARAMETER Source
    The damaged initrd.img from a WSABuilds Magisk build.

.PARAMETER Output
    Where to write the repaired image. Copy it over C:\WSA\Tools\initrd.img
    while WSA is off (keep the original as a backup).

.EXAMPLE
    .\Repair-MagiskInitrd.ps1 -Source C:\WSA-magisk\Tools\initrd.img -Output .\initrd.magisk-fixed.img
#>
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Output
)

$ErrorActionPreference = 'Stop'
$lspUrl = 'https://raw.githubusercontent.com/LSPosed/MagiskOnWSALocal/b40611271b25e891735e043f588b3688e0111ff8/bin/arm64/lspinit'
$lspSha = '19BB3433B76D870327F3EF85A5401F3DC6ECF1424D780F9447B6FAC628ABA498'

function Read-Cpio([byte[]]$d) {
    $ascii = [Text.Encoding]::ASCII
    $pos = 0; $list = [Collections.Generic.List[object]]::new()
    while ($pos + 110 -le $d.Length) {
        $magic = $ascii.GetString($d, $pos, 6)
        if ($magic -notin '070701', '070702') { throw "Not a newc cpio archive (bad header at offset $pos)" }
        $f = 0..12 | ForEach-Object { [Convert]::ToUInt32($ascii.GetString($d, $pos + 6 + 8 * $_, 8), 16) }
        $nameSize = $f[11]; $size = $f[6]
        $name = $ascii.GetString($d, $pos + 110, $nameSize - 1)
        $pos = ($pos + 110 + $nameSize + 3) -band -bnot 3
        $data = [byte[]]::new($size); [Array]::Copy($d, $pos, $data, 0, $size)
        $pos = ($pos + $size + 3) -band -bnot 3
        if ($name -eq 'TRAILER!!!') { break }
        $list.Add([pscustomobject]@{ Name = $name; Fields = $f; Data = $data })
    }
    $list
}

function Write-Cpio($entries, [string]$path) {
    $ms = [IO.MemoryStream]::new()
    $pad = { while ($ms.Length % 4) { $ms.WriteByte(0) } }
    $ino = 300000
    foreach ($e in @($entries) + [pscustomobject]@{ Name = 'TRAILER!!!'; Fields = (, [uint32]0 * 13); Data = [byte[]]@() }) {
        $f = [uint32[]]$e.Fields.Clone()
        $f[0] = if ($e.Name -eq 'TRAILER!!!') { 0 } else { $ino++ }   # inode
        $f[4] = 1                                                      # nlink
        $f[6] = $e.Data.Length                                         # filesize
        $f[11] = [Text.Encoding]::ASCII.GetByteCount($e.Name) + 1      # namesize
        $f[12] = 0                                                     # check
        $hdr = [Text.Encoding]::ASCII.GetBytes('070701' + (($f | ForEach-Object { '{0:X8}' -f $_ }) -join '') + $e.Name + "`0")
        $ms.Write($hdr, 0, $hdr.Length); & $pad
        $ms.Write($e.Data, 0, $e.Data.Length); & $pad
    }
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
}

function Get-Sha256([byte[]]$b) { -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object { $_.ToString('X2') }) }

$entries = Read-Cpio ([IO.File]::ReadAllBytes((Resolve-Path $Source)))
$lsp = $entries | Where-Object Name -eq 'lspinit'
if (-not $lsp) { throw "$Source has no lspinit - is this a WSABuilds Magisk initrd?" }

[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12   # Windows PowerShell 5.1
$tmp = Join-Path $env:TEMP ('lspinit-' + [guid]::NewGuid().ToString('N'))
Invoke-WebRequest -UseBasicParsing $lspUrl -OutFile $tmp
$good = [IO.File]::ReadAllBytes($tmp); Remove-Item -LiteralPath $tmp
if ((Get-Sha256 $good) -ne $lspSha) { throw 'Downloaded lspinit does not match the expected SHA-256.' }

if ((Get-Sha256 $lsp.Data) -eq $lspSha) { Write-Host 'lspinit in this image is already intact - nothing to repair.' -ForegroundColor Green; return }
# Only replace the file if it is exactly the official one with CRLF -> LF applied
$latin1 = [Text.Encoding]::GetEncoding(28591)
if ($latin1.GetString($good).Replace("`r`n", "`n") -ne $latin1.GetString($lsp.Data)) {
    throw 'lspinit differs from the official build in an unexpected way - not touching it.'
}

$lsp.Data = $good
Write-Cpio $entries $Output
Write-Host "Repaired lspinit ($($good.Length) bytes). Wrote $Output" -ForegroundColor Green
Write-Host 'With WSA off: back up C:\WSA\Tools\initrd.img, copy this file over it, then start WSA.'
