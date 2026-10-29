# WSA on Snapdragon X

How to run Windows Subsystem for Android on **Snapdragon X** (Oryon) PCs, which have no 32-bit ARM support. Stock WSA builds don't boot on them.

**Result:** Android 13 with Google Play, **root (Magisk 30.6)**, working internet behind a firewall, and video playback.

Tested on a Snapdragon X Plus X1P64100 (Adreno X1-85, 16 GB), Windows 11 build 26200, with [WSABuilds](https://github.com/MustardChef/WSABuilds) LTS 8 (`2407.40000.4.0` arm64) and [snickler/WSA-Patched](https://github.com/snickler/WSA-Patched) 0.0.2.

## Setup

**Downloads:**
- From WSABuilds' LTS 8 arm64 release:
  - `WSA_2407.40000.4.0_arm64_Release-Nightly-NoGApps-NoAmazon.7z`
  - `WSA_2407.40000.4.0_arm64_Release-Nightly-with-magisk-30.6.30600.-stable-NoGApps-NoAmazon.7z` (only for root; you need just its boot image)
- From snickler/WSA-Patched 0.0.2: `wsa-patched-0.0.2.7z`. These images include Google Play.
- [7-Zip](https://www.7-zip.org/) and Google's [platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb).

**Steps** (from a clone of this repo, paths as an example):

```powershell
$z = "$env:ProgramFiles\7-Zip\7z.exe"

# 1. Unpack WSA to C:\WSA (WSA runs from this folder, so it has to stay there)
& $z x WSA_2407.40000.4.0_arm64_Release-Nightly-NoGApps-NoAmazon.7z -oC:\
Rename-Item C:\WSA_2407.40000.4.0_arm64 WSA

# 2. Replace the four Android images with snickler's 64-bit-only ones (keep the originals if you like)
& $z x wsa-patched-0.0.2.7z -oC:\WSA -aoa

# 3. Fix video playback
.\scripts\Patch-VendorCcodec.ps1

# 4. Root (optional): repair the Magisk build's boot image and use it
& $z e WSA_2407.40000.4.0_arm64_Release-Nightly-with-magisk-30.6.30600.-stable-NoGApps-NoAmazon.7z -o"$env:TEMP\magisk-initrd" WSA_2407.40000.4.0_arm64\Tools\initrd.img
Copy-Item C:\WSA\Tools\initrd.img C:\WSA\Tools\initrd.stock.img
.\scripts\Repair-MagiskInitrd.ps1 -Source "$env:TEMP\magisk-initrd\initrd.img" -Output C:\WSA\Tools\initrd.img
```

5. Register WSA from an **admin** terminal: `cd C:\WSA`, then `powershell -ExecutionPolicy Bypass -File .\Install.ps1`. Running it already elevated avoids the permanent `Set-ExecutionPolicy Bypass` (CurrentUser) that `Run.bat` applies.
6. If you use **simplewall**: close it with File → Exit, run `.\scripts\Add-SimplewallWsaDns.ps1`, and start it again. Otherwise Android has no internet.
7. In WSA Settings → Advanced settings, turn on **Developer mode**. Unpack platform-tools to `%LOCALAPPDATA%\Android\platform-tools`, run `adb connect 127.0.0.1:58526`, and approve the debugging prompt once.
8. Optional:
   - `.\scripts\Register-InstallToWSA.ps1` adds **Install to WSA** to the right-click menu for `.apk`, `.xapk` and `.apks` files.
   - With root, [stop the location popup](#location-popup) and [register the Magisk app](#root).

To verify: Play Store sign-in works, YouTube plays, and with root `adb shell su -c id` prints `uid=0(root)`.

## Scripts

All scripts run in Windows PowerShell 5.1 or PowerShell 7 (`powershell -ExecutionPolicy Bypass -File <script>`). The ones that change WSA files refuse to run while WSA is using them.

| Script | What it does |
|---|---|
| [`Patch-VendorCcodec.ps1`](scripts/Patch-VendorCcodec.ps1) | Turns on Android's software video decoders in snickler's `vendor.vhdx`. Backs up first; `-Revert` undoes it. |
| [`Repair-MagiskInitrd.ps1`](scripts/Repair-MagiskInitrd.ps1) | Repairs the corrupted boot hook in WSABuilds' Magisk `initrd.img`. |
| [`Add-SimplewallWsaDns.ps1`](scripts/Add-SimplewallWsaDns.ps1) | Adds a simplewall rule that lets the WSA VM use the host's DNS. |
| [`Register-InstallToWSA.ps1`](scripts/Register-InstallToWSA.ps1) | Adds **Install to WSA** to Explorer's right-click menu (per user, no admin). `-Unregister` removes it. |
| [`Install-ToWSA.ps1`](scripts/Install-ToWSA.ps1) | The installer behind that menu entry: starts WSA if needed, installs APKs and bundles (with OBB data), explains common errors. |
| [`Trace-WsaBoot.ps1`](scripts/Trace-WsaBoot.ps1) | Records WSA's own diagnostic events, for debugging boots that fail before Android logs anything. Admin only. |
| [`disable-location.sh`](magisk/service.d/disable-location.sh) | Magisk boot script that keeps Android's location off. |

---

## How it works

### Booting: stock images crash (`0xc0000409`)

On a stock build, "Starting Windows Subsystem for Android…" shows for about 2 minutes, then `WsaClient.exe` crashes with `0xc0000409` and WSA Settings → Apps shows "0 apps found". The stock Android images contain 32-bit ARM components (the 32-bit app runtime and some media/DRM services), and Snapdragon X has no 32-bit ARM support, so Android never finishes booting ([WSABuilds#710](https://github.com/MustardChef/WSABuilds/issues/710)). snickler's images are rebuilt without them. Confirmed with the stock NoGApps images on a fresh data disk: the VM stops within about 20 s.

### Root

The prebuilt Magisk builds fail the same way, even with snickler's images, because of a **corrupted file**, not the CPU.

On WSA, Magisk sits in `Tools\initrd.img`, a small boot image whose `/init` is a hook called **lspinit** (from [MagiskOnWSALocal](https://github.com/LSPosed/MagiskOnWSALocal)). It starts WSA's normal init (`/wsainit`) and swaps in `magiskinit` when Android starts. In the WSABuilds Magisk build, every `0D 0A` byte pair in lspinit has been turned into `0A`, the CRLF → LF conversion Git applies to text files. lspinit is 3 bytes short and dies with `Illegal instruction` before WSA's init runs, so the VM never contacts Windows. The rest of the boot image is intact.

`Repair-MagiskInitrd.ps1` downloads lspinit from a pinned MagiskOnWSALocal commit, checks its SHA-256, confirms the shipped copy is exactly that file with CRLF → LF applied, and writes a fixed `initrd.img`. With it, `su -c id` gives `uid=0(root) … context=u:r:magisk:s0`, and root survives restarts. **To remove root**, copy `initrd.stock.img` back over `C:\WSA\Tools\initrd.img` while WSA is off.

How it was found: a [`Trace-WsaBoot.ps1`](scripts/Trace-WsaBoot.ps1) trace of the failed boot showed `HeartbeatTimeout` with `firstHeartbeatReceived=0`: nothing inside the VM ever talked to Windows. Running the boot image's binaries from a normal boot via `adb shell` then showed `magiskinit` working and `lspinit` crashing, and a byte comparison with the official file showed the damage.

**Magisk on WSA:**
- **The Magisk app is missing from the Start menu** because it comes preinstalled, and WSA only creates entries for apps installed while it runs. Reinstall it over itself once:
  `adb shell 'cp "$(pm path com.topjohnwu.magisk | cut -d: -f2)" /data/local/tmp/m.apk && pm install -r /data/local/tmp/m.apk'`
- **Don't use the app's Update / Direct install.** The app can't patch `Tools\initrd.img` on the Windows side. A newer Magisk means rebuilding `initrd.img`.
- **Zygisk is off by default.** Turn it on in Magisk → Settings → Zygisk and restart WSA if you need it (LSPosed, DenyList, Play Integrity modules). Not tested here yet.
- The image includes one module, `lsp_cust` ("WSA Customization": VPN dialog, device admin, managed users).
- `adb root` still doesn't work (the build is `user`); use `adb shell su -c <command>`.

### Video: YouTube stuck at 0:00

Pages and thumbnails load, but videos never start, and `adb shell dumpsys media.player` lists **no video decoders**:
- snickler's `vendor.vhdx` `build.prop` sets `debug.stagefright.ccodec=0`, which disables Codec2, where the software decoders (`c2.android.*`) live.
- Android then falls back to the OMX decoders (`OMX.android.latte.*`), but there's no OMX service: in the stock image it's 32-bit.
- The image's `c2.goldfish.*` hardware decoders need the Android Emulator's host pipe, which WSA doesn't provide.

`Patch-VendorCcodec.ps1` changes the value to `4`. It's the same length, so the script byte-patches the `.vhdx` directly (ext4 and VHDX don't checksum file data), including the copy in the filesystem journal. Afterwards `c2.android.avc/vp8/vp9.decoder` are available. **Clear YouTube's storage once** (Settings → Apps → YouTube → Storage), because it remembers that no decoders were found.

Decoding is software only: 1080p is fine, 4K may stutter, and there's no AV1 or HEVC. Hardware decoding would need a 64-bit build of `android.hardware.media.omx@1.0-service`, which Microsoft never shipped. The 64-bit plugin it loads (`lib64/libstagefrighthw.so`) is in the image, but transplanting the service from another Android 13 build hasn't been tried.

### Internet: "Couldn't sign in" behind simplewall

Google sign-in fails with "There was a problem communicating with Google servers", and simplewall never prompts. Android sends DNS queries to the Windows host on UDP 53, where Internet Connection Sharing (`SharedAccess`, in `svchost.exe`) answers them. simplewall blocks this **inbound** traffic silently. Other VM traffic is NAT-forwarded and isn't affected.

`Add-SimplewallWsaDns.ps1` adds a user rule: allow, inbound, local port 53, remote `172.16.0.0/12` (Hyper-V's VM range; the WSA subnet changes between boots), app `svchost.exe`. simplewall's command line can't add rules, so the script edits `profile.xml` directly, with attribute names from simplewall's [`src/db.c`](https://github.com/henrypp/simplewall/blob/master/src/db.c). The diagnosis came from simplewall's packet log (Settings → Packets log → log to file).

### Location popup

With Windows location off, a notification keeps saying "Google Play services wants to access your location", and the location permission for Play services and Google is greyed out ("Device requires this permission to operate"). Android's own location switch is on, so WSA forwards apps' location requests to Windows, which refuses. Play services and Google are privileged system apps, so their permission can't be changed individually.

Turn location off for all of Android: `adb shell cmd location set-location-enabled false`, or Settings → Location → **Use location**. **WSA turns it back on at every boot**, so with root, install the Magisk boot script that switches it off after each boot:

```powershell
adb push magisk\service.d\disable-location.sh /data/local/tmp/
adb shell "su -c 'mkdir -p /data/adb/service.d && cp /data/local/tmp/disable-location.sh /data/adb/service.d/ && chmod 755 /data/adb/service.d/disable-location.sh'"
```

To undo, delete `/data/adb/service.d/disable-location.sh` as root.

---

## Limits

- **64-bit apps only.** Android reports only `arm64-v8a`; apps without it fail with `INSTALL_FAILED_NO_MATCHING_ABIS`.
- **60 fps maximum.** Android sees a single 60 Hz display, whatever the monitor's refresh rate. If a game feels sluggish, check its own frame limiter: GTA: San Andreas' limiter held it near 30 fps with uneven pacing until it was turned off.
- **Software video decoding** (see above).
- `.apkm` files from APKMirror are encrypted; use APKMirror's installer app for those.

## Hashes

| File | SHA-256 |
|---|---|
| `wsa-patched-0.0.2.7z` | `b1582feef760fdd7cd9897de767a37d5d476a91187f1fb7b50e86786e56e265a` |
| `vendor.vhdx` from it, unmodified | `d7604cb53721650592798f0a0b354b234ee502daebda999868de5f5132bb87ae` |
| `vendor.vhdx` after `Patch-VendorCcodec.ps1` | `2a8279a2ef3e8d8891746b97fa02ef2c1876ae7f4bce6611632a510e121c1d6e` |
| WSABuilds Magisk `Tools\initrd.img` (damaged) | `8c3669884981c1dd5eef56d04274f5586bbe66fe1a6bbb52a3319c570b664685` |
| `lspinit` in it (damaged) | `321af8cf1e93780fa5e809c4eb74a65b787b6f6e47c34db6d83917bdeac82c77` |
| Official `lspinit` (MagiskOnWSALocal `b406112`) | `19bb3433b76d870327f3ef85a5401f3dc6ecf1424d780f9447b6fac628aba498` |
| `initrd.img` from `Repair-MagiskInitrd.ps1` | `6b026492a6db87f36ce609aaaa7372fd268f0f212b9948574fa73e99c39a26a8` |
