<#
.SYNOPSIS
    Builds the "WSA GL fixes" Magisk module zip.

.DESCRIPTION
    Compiles two stand-ins for WSA's graphics drivers with Zig
    (https://ziglang.org, any recent version; the arm64 Windows build works),
    linked against libc.so / libdl.so pulled from WSA, so no Android NDK is needed:

      libEGL_emulation.so    src\egl_surfaceless.c  adds EGL_KHR_surfaceless_context
      libGLESv2_emulation.so src\gles_wrapper.c     emulates vertex array objects for the
                                                    packages in debug.gles.vaoemu
                                                    (module\system.prop sets the default)

    With -Diagnostics the GLES wrapper also gets the debugging tools (tag gles-seq):
    a call-sequence recorder (debug.gles.trace), a draw filter (debug.gles.skip)
    and a buffer re-upload trigger (debug.gles.reupload). They cost memory and
    CPU, so the everyday build leaves them out.

    WSA's real drivers are pulled from the device and bundled as *_real.so. The
    real GLES driver's soname is renamed (same length) so it doesn't clash with
    the wrapper, which takes over the name libGLESv2_emulation.so.

.EXAMPLE
    .\Build-Module.ps1 -Zig C:\tools\zig\zig.exe
    adb push wsa-egl-surfaceless.zip /data/local/tmp/
    adb shell su -c "magisk --install-module /data/local/tmp/wsa-egl-surfaceless.zip"
#>
param(
    [string]$Zig = 'zig',
    [string]$Adb = (Join-Path $env:LOCALAPPDATA 'Android\platform-tools\adb.exe'),
    [string]$Target = '127.0.0.1:58526',
    [string]$Output = (Join-Path $PSScriptRoot 'wsa-egl-surfaceless.zip'),
    [switch]$Diagnostics
)

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$sysroot = Join-Path $build 'sysroot'
$stage = Join-Path $build 'module'
$egl = Join-Path $stage 'system\vendor\lib64\egl'
$latin1 = [Text.Encoding]::GetEncoding(28591)
foreach ($d in $sysroot, $stage) { if (Test-Path $d) { [IO.Directory]::Delete($d, $true) } }
New-Item -ItemType Directory -Force $sysroot, $egl | Out-Null

function Pull([string]$remote, [string]$local) {
    & $Adb -s $Target pull $remote $local | Out-Null
    if ($LASTEXITCODE) { throw "adb pull $remote failed" }
}

# WSA's real driver: the module's *_real.so copy if the module is active, else the original
function Pull-Driver([string]$name) {
    $real = $name -replace '\.so$', '_real.so'
    $local = Join-Path $egl $real
    & $Adb -s $Target shell "test -f /vendor/lib64/egl/$real" | Out-Null
    $remote = if ($LASTEXITCODE -eq 0) { "/vendor/lib64/egl/$real" } else { "/vendor/lib64/egl/$name" }
    Pull $remote $local
    if ($latin1.GetString([IO.File]::ReadAllBytes($local)) -match 'egl_surfaceless|gles-wrapper|gles-seq') {
        throw "$remote is a wrapper, not WSA's driver"
    }
    $local
}

& $Adb connect $Target | Out-Null
Pull /system/lib64/libc.so (Join-Path $sysroot 'libc.so')
Pull /system/lib64/libdl.so (Join-Path $sysroot 'libdl.so')
Pull-Driver libEGL_emulation.so | Out-Null
$glesReal = Pull-Driver libGLESv2_emulation.so

# Rename the real GLES driver's soname (one occurrence, in .dynstr)
$bytes = [IO.File]::ReadAllBytes($glesReal)
$text = $latin1.GetString($bytes)
$old = "`0libGLESv2_emulation.so`0"; $new = "`0libGLESv2_real_emul.so`0"
$at = $text.IndexOf($old)
if ($at -lt 0 -and $text.Contains($new)) {
    # Already renamed: pulled from an active module's _real.so copy
} elseif ($at -lt 0 -or $text.IndexOf($old, $at + 1) -ge 0) {
    throw 'Expected exactly one soname string in libGLESv2_emulation.so'
} else {
    [Array]::Copy($latin1.GetBytes($new), 0, $bytes, $at, $new.Length)
    [IO.File]::WriteAllBytes($glesReal, $bytes)
}

$common = @('cc', '-target', 'aarch64-linux-gnu', '-shared', '-fPIC', '-O2', '-nostdlib', '-nostdinc',
            '-fno-stack-protector', '-fvisibility=hidden', '-Wl,--hash-style=both')
$libs = @((Join-Path $sysroot 'libdl.so'), (Join-Path $sysroot 'libc.so'))
& $Zig @common '-Wl,-soname,libEGL_emulation_wrapper.so' -o (Join-Path $egl 'libEGL_emulation.so') `
    (Join-Path $PSScriptRoot 'src\egl_surfaceless.c') @libs
if ($LASTEXITCODE) { throw "zig cc (EGL) failed ($LASTEXITCODE)" }
$diag = if ($Diagnostics) { @('-DGLES_DIAG') } else { @() }
& $Zig @common @diag '-Wl,-soname,libGLESv2_emulation.so' -I (Join-Path $PSScriptRoot 'src') `
    -o (Join-Path $egl 'libGLESv2_emulation.so') (Join-Path $PSScriptRoot 'src\gles_wrapper.c') @libs
if ($LASTEXITCODE) { throw "zig cc (GLES) failed ($LASTEXITCODE)" }

Copy-Item (Join-Path $PSScriptRoot 'module\*') $stage -Recurse
# Android's shell needs LF line endings in the scripts
foreach ($f in Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -ne '.so' }) {
    $t = [IO.File]::ReadAllText($f.FullName)
    [IO.File]::WriteAllText($f.FullName, $t.Replace("`r`n", "`n"))
}
if (Test-Path $Output) { Remove-Item -LiteralPath $Output }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $Output)   # entries use '/' separators
Write-Host "Built $Output$(if ($Diagnostics) { ' (with diagnostics)' })" -ForegroundColor Green
