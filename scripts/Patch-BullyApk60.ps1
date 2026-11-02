<#
.SYNOPSIS
    Patches a Bully: Anniversary Edition APK to run at 60 fps instead of 30, for
    devices without root.

.DESCRIPTION
    Bully caps itself in Application::ClampFPS: it sleeps until (1/maxFPS) *
    1,000,000 µs have passed since the last frame, with maxFPS = 30. This changes
    that constant to 500,000 (one instruction in lib/arm64-v8a/libGame.so), so
    every frame cap the game sets is doubled: 30 -> 60. The code is found by a
    32-byte pattern and the script refuses if it isn't there exactly once.

    The APK is then re-signed. Android only installs it as an update over the
    existing app if it's signed with the same key as the installed copy:
      - APKs signed with the public AOSP test key (CN=Android, cert SHA-256
        a40da80a…f5dc) can be re-signed with it: -UseAospTestKey downloads it
        from the Android source tree.
      - Play Store installs can't be updated this way (Google's key isn't
        public): the app would have to be uninstalled first (local saves and Play
        asset packs are lost, and Play asset delivery won't serve a re-signed app).

    Needs 7-Zip, a JDK (java on PATH or -Java) and Android build-tools
    (zipalign, apksigner; -BuildTools). Parts of Bully are tuned for 30 fps.

.EXAMPLE
    .\Patch-BullyApk60.ps1 -Apk base.apk -Output bully-60fps.apk -UseAospTestKey -BuildTools <dir> -Java <jdk dir>
    adb install -r bully-60fps.apk
#>
param(
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][string]$Output,
    [string]$BuildTools,
    [string]$Java,
    [string]$Key,                      # PKCS#8 private key (.pk8)
    [string]$Cert,                     # X.509 certificate (.pem)
    [switch]$UseAospTestKey
)

$ErrorActionPreference = 'Stop'
$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path $sevenZip)) { throw '7-Zip not found' }
if ($Java) { $env:JAVA_HOME = $Java; $env:PATH = "$Java\bin;$env:PATH" }
$zipalign = Join-Path $BuildTools 'zipalign.exe'
$apksigner = Join-Path $BuildTools 'apksigner.bat'
foreach ($f in $zipalign, $apksigner) { if (-not (Test-Path $f)) { throw "Not found: $f (pass -BuildTools)" } }

$work = Join-Path ([IO.Path]::GetTempPath()) ('bully60-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
try {
    if ($UseAospTestKey) {
        $base = 'https://android.googlesource.com/platform/build/+/refs/heads/main/target/product/security'
        foreach ($n in 'testkey.pk8', 'testkey.x509.pem') {
            $b64 = (Invoke-WebRequest -UseBasicParsing "$base/$n`?format=TEXT").Content
            if ($b64 -is [byte[]]) { $b64 = [Text.Encoding]::ASCII.GetString($b64) }
            [IO.File]::WriteAllBytes((Join-Path $work $n), [Convert]::FromBase64String($b64.Trim()))
        }
        $Key = Join-Path $work 'testkey.pk8'; $Cert = Join-Path $work 'testkey.x509.pem'
    }
    if (-not $Key -or -not $Cert) { throw 'Pass -Key and -Cert, or -UseAospTestKey' }

    # 1. Patch libGame.so
    $libPath = 'lib/arm64-v8a/libGame.so'
    & $sevenZip x -tzip $Apk "-o$work\stage" $libPath -y | Out-Null
    $lib = Join-Path $work 'stage\lib\arm64-v8a\libGame.so'
    if (-not (Test-Path $lib)) { throw "$libPath not in the APK" }
    $b = [IO.File]::ReadAllBytes($lib)
    # fmov s1,#1.0 / mov w8,#0x2400 / movk w8,#0x4974,lsl#16 (1e6f) / fdiv / fmov s1,w8 / sub / fmul / fcvtzs
    $pat = [byte[]](0x01,0x10,0x2e,0x1e, 0x08,0x80,0x84,0x52, 0x88,0x2e,0xa9,0x72, 0x20,0x18,0x20,0x1e,
                    0x01,0x01,0x27,0x1e, 0x08,0x00,0x16,0xcb, 0x00,0x08,0x21,0x1e, 0x15,0x00,0x38,0x9e)
    $hits = @(); $patched = @()
    for ($i = 0; $i -le $b.Length - $pat.Length; $i += 4) {
        if ($b[$i] -ne 0x01 -or $b[$i + 1] -ne 0x10) { continue }
        $same = $true; $done = $true
        for ($j = 2; $j -lt $pat.Length; $j++) {
            $want = $pat[$j]
            if ($b[$i + $j] -ne $want) { $same = $false }
            if ($j -eq 9) { $want = 0x1e }                # already-patched variant (movk #0x48f4)
            if ($b[$i + $j] -ne $want) { $done = $false }
        }
        if ($same) { $hits += $i } elseif ($done) { $patched += $i }
    }
    if ($patched.Count -eq 1 -and $hits.Count -eq 0) { throw 'libGame.so is already patched' }
    if ($hits.Count -ne 1) { throw "ClampFPS pattern found $($hits.Count) times - unknown game version, not patching" }
    $b[$hits[0] + 9] = 0x1e                                # movk w8, #0x48f4, lsl #16  -> 5e5f
    [IO.File]::WriteAllBytes($lib, $b)
    Write-Host ("Patched ClampFPS at file offset 0x{0:x}: frame period 1e6/maxFPS -> 5e5/maxFPS" -f ($hits[0] + 8))

    # 2. Copy the APK with the library swapped. Every other entry is copied as raw
    #    compressed bytes; the old signing block is dropped and apksigner replaces
    #    the old v1 signature files. (7-Zip can't edit APKs this large.)
    $tmpApk = Join-Path $work 'unsigned.apk'
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Compression;
public static class ApkEntrySwap {
    static uint[] crcTable;
    static uint Crc32(byte[] d) {
        if (crcTable == null) {
            crcTable = new uint[256];
            for (uint n = 0; n < 256; n++) { uint c = n; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTable[n] = c; }
        }
        uint crc = 0xFFFFFFFFu;
        foreach (byte b in d) crc = crcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }
    static uint U32(byte[] b, int o) { return BitConverter.ToUInt32(b, o); }
    static ushort U16(byte[] b, int o) { return BitConverter.ToUInt16(b, o); }
    public static void Replace(string src, string dst, string name, byte[] data) {
        using (var inp = new FileStream(src, FileMode.Open, FileAccess.Read))
        using (var outp = new FileStream(dst, FileMode.Create, FileAccess.Write)) {
            // End of central directory (no zip64: this APK has < 65535 entries and < 4 GB)
            int tail = (int)Math.Min(inp.Length, 65557);
            byte[] t = new byte[tail];
            inp.Seek(inp.Length - tail, SeekOrigin.Begin); inp.Read(t, 0, tail);
            int e = -1;
            for (int i = tail - 22; i >= 0; i--) if (U32(t, i) == 0x06054b50) { e = i; break; }
            if (e < 0) throw new Exception("no end of central directory");
            int count = U16(t, e + 10);
            uint cdSize = U32(t, e + 12), cdOff = U32(t, e + 16);
            byte[] cd = new byte[cdSize];
            inp.Seek(cdOff, SeekOrigin.Begin); inp.Read(cd, 0, (int)cdSize);
            var newCd = new MemoryStream();
            byte[] buf = new byte[1 << 20];
            bool found = false;
            int p = 0;
            for (int k = 0; k < count; k++) {
                if (U32(cd, p) != 0x02014b50) throw new Exception("bad central directory");
                int nlen = U16(cd, p + 28), xlen = U16(cd, p + 30), clen = U16(cd, p + 32);
                int recLen = 46 + nlen + xlen + clen;
                string n = System.Text.Encoding.UTF8.GetString(cd, p + 46, nlen);
                uint csize = U32(cd, p + 20), loff = U32(cd, p + 42);
                if (n == name) { found = true; p += recLen; continue; }
                byte[] lh = new byte[30];
                inp.Seek(loff, SeekOrigin.Begin); inp.Read(lh, 0, 30);
                if (U32(lh, 0) != 0x04034b50) throw new Exception("bad local header for " + n);
                long total = 30 + U16(lh, 26) + U16(lh, 28) + (long)csize;
                uint newOff = (uint)outp.Position;
                inp.Seek(loff, SeekOrigin.Begin);
                for (long left = total; left > 0;) {
                    int r = inp.Read(buf, 0, (int)Math.Min(buf.Length, left));
                    if (r <= 0) throw new Exception("short read in " + n);
                    outp.Write(buf, 0, r); left -= r;
                }
                byte[] rec = new byte[recLen];
                Array.Copy(cd, p, rec, 0, recLen);
                Array.Copy(BitConverter.GetBytes(newOff), 0, rec, 42, 4);
                newCd.Write(rec, 0, recLen);
                p += recLen;
            }
            if (!found) throw new Exception(name + " not found");
            // New entry, deflated
            var comp = new MemoryStream();
            using (var ds = new DeflateStream(comp, CompressionLevel.Optimal, true)) ds.Write(data, 0, data.Length);
            byte[] cdata = comp.ToArray();
            byte[] nameB = System.Text.Encoding.UTF8.GetBytes(name);
            uint crc = Crc32(data), off = (uint)outp.Position;
            var w = new BinaryWriter(outp);
            w.Write(0x04034b50u); w.Write((ushort)20); w.Write((ushort)0); w.Write((ushort)8);
            w.Write((ushort)0); w.Write((ushort)0x21);              // time/date: 1980-01-01
            w.Write(crc); w.Write((uint)cdata.Length); w.Write((uint)data.Length);
            w.Write((ushort)nameB.Length); w.Write((ushort)0); w.Write(nameB); w.Write(cdata);
            var cw = new BinaryWriter(newCd);
            cw.Write(0x02014b50u); cw.Write((ushort)20); cw.Write((ushort)20); cw.Write((ushort)0); cw.Write((ushort)8);
            cw.Write((ushort)0); cw.Write((ushort)0x21); cw.Write(crc); cw.Write((uint)cdata.Length); cw.Write((uint)data.Length);
            cw.Write((ushort)nameB.Length); cw.Write((ushort)0); cw.Write((ushort)0); cw.Write((ushort)0); cw.Write((ushort)0);
            cw.Write(0u); cw.Write(off); cw.Write(nameB);
            w.Flush();
            uint newCdOff = (uint)outp.Position;
            byte[] cdBytes = newCd.ToArray();
            outp.Write(cdBytes, 0, cdBytes.Length);
            w.Write(0x06054b50u); w.Write((ushort)0); w.Write((ushort)0); w.Write((ushort)count); w.Write((ushort)count);
            w.Write((uint)cdBytes.Length); w.Write(newCdOff); w.Write((ushort)0);
            w.Flush();
        }
    }
}
'@
    [ApkEntrySwap]::Replace((Resolve-Path $Apk).Path, $tmpApk, $libPath, [IO.File]::ReadAllBytes($lib))

    # 3. Align and sign
    $aligned = Join-Path $work 'aligned.apk'
    & $zipalign -p -f 4 $tmpApk $aligned
    if ($LASTEXITCODE) { throw 'zipalign failed' }
    & $apksigner sign --key $Key --cert $Cert --out $Output $aligned
    if ($LASTEXITCODE) { throw 'apksigner failed' }
    & $apksigner verify --print-certs $Output | Select-String 'Verified using|certificate SHA-256' | ForEach-Object { $_.Line }
    Write-Host "Wrote $Output" -ForegroundColor Green
} finally {
    if (Test-Path $work) { [IO.Directory]::Delete($work, $true) }
}
