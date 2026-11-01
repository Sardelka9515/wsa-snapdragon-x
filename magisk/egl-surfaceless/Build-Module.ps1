<#
.SYNOPSIS
    Builds the "WSA EGL surfaceless contexts" Magisk module zip.

.DESCRIPTION
    Compiles src\egl_surfaceless.c into a stand-in for WSA's EGL driver with Zig
    (https://ziglang.org, any recent version; the arm64 Windows build works) and
    links it against libc.so / libdl.so pulled from WSA, so no Android NDK is
    needed. WSA's real driver is pulled from the device and bundled as
    libEGL_emulation_real.so.

    Run it while the module is NOT active, so the pulled driver is WSA's own
    (the module's installer refuses the wrapper anyway).

.EXAMPLE
    .\Build-Module.ps1 -Zig C:\tools\zig\zig.exe
    adb push wsa-egl-surfaceless.zip /data/local/tmp/
    adb shell su -c "magisk --install-module /data/local/tmp/wsa-egl-surfaceless.zip"
#>
param(
    [string]$Zig = 'zig',
    [string]$Adb = (Join-Path $env:LOCALAPPDATA 'Android\platform-tools\adb.exe'),
    [string]$Target = '127.0.0.1:58526',
    [string]$Output = (Join-Path $PSScriptRoot 'wsa-egl-surfaceless.zip')
)

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$sysroot = Join-Path $build 'sysroot'
$stage = Join-Path $build 'module'
$egl = Join-Path $stage 'system\vendor\lib64\egl'
if (Test-Path $build) { [IO.Directory]::Delete($build, $true) }
New-Item -ItemType Directory -Force $sysroot, $egl | Out-Null

# Libraries to link against, and WSA's real driver to bundle
& $Adb connect $Target | Out-Null
foreach ($f in '/system/lib64/libc.so', '/system/lib64/libdl.so') {
    & $Adb -s $Target pull $f (Join-Path $sysroot (Split-Path $f -Leaf)) | Out-Null
    if ($LASTEXITCODE) { throw "adb pull $f failed" }
}
$real = Join-Path $egl 'libEGL_emulation_real.so'
& $Adb -s $Target pull /vendor/lib64/egl/libEGL_emulation.so $real | Out-Null
if ($LASTEXITCODE) { throw 'adb pull of libEGL_emulation.so failed' }
if ([Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($real)).Contains('egl_surfaceless')) {
    throw 'The device already runs the wrapper. Disable the module, restart WSA, and build again.'
}

& $Zig cc -target aarch64-linux-gnu -shared -fPIC -O2 -nostdlib -nostdinc -fno-stack-protector `
    -fvisibility=hidden '-Wl,-soname,libEGL_emulation_wrapper.so' '-Wl,--hash-style=both' `
    -o (Join-Path $egl 'libEGL_emulation.so') (Join-Path $PSScriptRoot 'src\egl_surfaceless.c') `
    (Join-Path $sysroot 'libdl.so') (Join-Path $sysroot 'libc.so')
if ($LASTEXITCODE) { throw "zig cc failed ($LASTEXITCODE)" }

Copy-Item (Join-Path $PSScriptRoot 'module\*') $stage -Recurse
# Android's shell needs LF line endings in the scripts
foreach ($f in Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -ne '.so' }) {
    $t = [IO.File]::ReadAllText($f.FullName)
    [IO.File]::WriteAllText($f.FullName, $t.Replace("`r`n", "`n"))
}
if (Test-Path $Output) { Remove-Item -LiteralPath $Output }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $Output)   # entries use '/' separators
Write-Host "Built $Output" -ForegroundColor Green
