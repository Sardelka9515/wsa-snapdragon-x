<#
.SYNOPSIS
    Adds a simplewall user rule that lets WSA's Android VM use the host's DNS forwarder.

.DESCRIPTION
    WSA's VM sends DNS queries to the Windows host on port 53, where the Internet
    Connection Sharing service (SharedAccess, in svchost.exe) answers them.
    simplewall blocks that inbound traffic silently, so Android has no DNS:
    Google sign-in fails with "problem communicating with Google servers".

    Rule: allow, inbound, local port 53, remote 172.16.0.0/12 (Hyper-V's private
    VM range - the exact WSA subnet can change between boots), app svchost.exe.

    simplewall rewrites profile.xml when it exits, so close it fully first
    (File > Exit), run this, then start simplewall again.
    Attribute names come from simplewall's src/db.c (dir=1 is inbound; no
    "protocol" attribute means any protocol).
#>
param(
    [string]$ProfilePath = (Join-Path $env:APPDATA 'Henry++\simplewall\profile.xml'),
    [string]$RemoteRange = '172.16.0.0/12'
)

$ErrorActionPreference = 'Stop'

if (Get-Process simplewall -ErrorAction SilentlyContinue) {
    throw 'simplewall is running. Close it with File > Exit (not the window X), then run this again.'
}
if (-not (Test-Path $ProfilePath)) { throw "simplewall profile not found: $ProfilePath" }

$xml = [xml](Get-Content $ProfilePath -Raw)
$rules = $xml.SelectSingleNode('/root/rules_custom')
if (-not $rules) { throw 'No <rules_custom> section in profile.xml - unexpected simplewall profile format.' }
if ($rules.SelectSingleNode("item[@name='WSA DNS']")) { Write-Host 'Rule "WSA DNS" already exists - nothing to do.' -ForegroundColor Green; return }

Copy-Item $ProfilePath "$ProfilePath.before-wsa-dns" -Force

$item = $xml.CreateElement('item')
$attrs = [ordered]@{
    name       = 'WSA DNS'
    rule       = $RemoteRange
    rule_local = '53'
    dir        = '1'
    apps       = '%SystemRoot%\System32\svchost.exe'
    is_enabled = 'true'
}
foreach ($k in $attrs.Keys) { $item.SetAttribute($k, $attrs[$k]) }
[void]$rules.AppendChild($item)
$xml.Save($ProfilePath)

Write-Host "Added rule 'WSA DNS'. Backup: $ProfilePath.before-wsa-dns" -ForegroundColor Green
Write-Host 'Start simplewall again; the rule appears under User rules.'
