# WSA on Snapdragon X

Notes and scripts for getting Windows Subsystem for Android running on a **Snapdragon X** (Oryon) laptop, which has no 32-bit ARM support. It covers booting, root, networking behind simplewall, and fixing video playback.

**Result:** Android 13 boots with **root (Magisk 30.6)**, Play Store sign-in works, YouTube plays (software decoding).

| | |
|---|---|
| PC | Snapdragon X Plus X1P64100 (10 cores), Adreno X1-85, 16 GB RAM |
| Windows | 11 Home, build 26200 (25H2), ARM64 |
| WSA build | [WSABuilds](https://github.com/MustardChef/WSABuilds) LTS 8: `WSA_2407.40000.4.0_arm64_Release-Nightly-NoGApps-NoAmazon.7z`, plus the repaired `Tools\initrd.img` from the `with-magisk-30.6.30600.-stable-NoGApps-NoAmazon` build for root |
| Android images | [snickler/WSA-Patched](https://github.com/snickler/WSA-Patched) 0.0.2: `wsa-patched-0.0.2.7z` (4 × `.vhdx`) |
| Install folder | `C:\WSA` (WSA runs from it, so it must stay put) |
| Firewall | [simplewall](https://github.com/henrypp/simplewall) 3.9.1, allow-list mode |

## Scripts

| Script | What it does |
|---|---|
| [`scripts/Repair-MagiskInitrd.ps1`](scripts/Repair-MagiskInitrd.ps1) | Fixes "Magisk build never boots": repairs the corrupted boot hook in WSABuilds' Magisk `initrd.img`. |
| [`scripts/Trace-WsaBoot.ps1`](scripts/Trace-WsaBoot.ps1) | Records WSA's own diagnostic events (boot progress, why the VM was stopped). Admin only. |
| [`magisk/service.d/disable-location.sh`](magisk/service.d/disable-location.sh) | Magisk boot script that keeps Android's location off (see the location section). |
| [`scripts/Patch-VendorCcodec.ps1`](scripts/Patch-VendorCcodec.ps1) | Fixes "no video": turns on Android's software decoders in the patched `vendor.vhdx`. Backs up first and supports `-Revert`. |
| [`scripts/Add-SimplewallWsaDns.ps1`](scripts/Add-SimplewallWsaDns.ps1) | Fixes "no internet in Android" behind simplewall: adds an inbound DNS rule for the WSA VM. |
| [`scripts/Install-ToWSA.ps1`](scripts/Install-ToWSA.ps1) | Installs an `.apk` / `.xapk` / `.apks` via adb. Starts WSA if needed and explains common errors. |
| [`scripts/Register-InstallToWSA.ps1`](scripts/Register-InstallToWSA.ps1) | Adds **Install to WSA** to Explorer's right-click menu (per user, no admin). `-Unregister` removes it. |

The scripts run in Windows PowerShell 5.1 or PowerShell 7, for example `powershell -ExecutionPolicy Bypass -File .\scripts\Patch-VendorCcodec.ps1`.

---

## Problem 1: WSA never starts (crash `0xc0000409`)

**Symptom:** "Starting Windows Subsystem for Android…" shows for about 2 minutes, then the window closes. Event Viewer shows a `WsaClient.exe` crash in `ucrtbase.dll` with exception `0xc0000409`, and WSA Settings → Apps shows "0 apps found".

**Cause:** WSA's stock Android images include 32-bit ARM components (the 32-bit app launcher and some media/DRM services). Snapdragon X cores have no 32-bit ARM support in hardware, so Android never finishes booting. See [WSABuilds#710](https://github.com/MustardChef/WSABuilds/issues/710).

**Fix:** replace the four images with snickler's rebuilt ones.

1. Unpack the WSABuilds `NoGApps-NoAmazon` ARM64 build to `C:\WSA`.
2. Copy `system.vhdx`, `vendor.vhdx`, `system_ext.vhdx` and `product.vhdx` from `wsa-patched-0.0.2.7z` over the ones in `C:\WSA`. Keep the originals somewhere as a backup.
3. In an **admin** terminal, run `cd C:\WSA`, then `powershell -ExecutionPolicy Bypass -File .\Install.ps1`.
   Running it already elevated avoids the permanent `Set-ExecutionPolicy Bypass` (CurrentUser) that `Run.bat` applies.

**What did not work:**
- **Stock images**, both the GApps + Magisk build and the plain NoGApps build. The NoGApps test used a fresh data disk: the VM stops within about 20 s, and `WsaClient.exe` crashes repeatedly with `0xc0000409`.
- **Patched images with the Magisk build, as shipped** crash the same way, because the Magisk boot image is corrupted. That one is fixable: see [Root](#root-magisk-build-never-boots).

---

## Problem 2: no internet inside Android ("Couldn't sign in")

**Symptom:** Google sign-in says "There was a problem communicating with Google servers", and simplewall never prompts.

**Cause:** Android's DNS queries go from the VM (for example `172.30.221.210`) to the Windows host on UDP 53, where the Internet Connection Sharing service (`SharedAccess`, in `svchost.exe`) answers them. simplewall blocks this **inbound** traffic, and svchost is set to "silent", so there's no prompt. Normal web traffic from the VM is NAT-forwarded and is *not* blocked; only DNS is.

I found this with simplewall's packet log: Settings → Packets log → "Enable packets logging to a file", then reading `%USERPROFILE%\simplewall.csv`.

**Fix:** a simplewall user rule. Close simplewall fully (File → Exit), run [`Add-SimplewallWsaDns.ps1`](scripts/Add-SimplewallWsaDns.ps1), then start simplewall again. The rule:

- **Allow**, **inbound**, local port **53**
- remote `172.16.0.0/12` (Hyper-V's private VM range; the exact WSA subnet can change between boots)
- app `svchost.exe`

simplewall's command line can't add rules, so the script edits `profile.xml` directly. The attribute names come from simplewall's [`src/db.c`](https://github.com/henrypp/simplewall/blob/master/src/db.c): `dir=1` means inbound, and leaving out `protocol` means any protocol.

---

## Problem 3: no video playback (YouTube stuck at 0:00)

**Symptom:** pages and thumbnails load, but the player spins forever at 0:00. YouTube's log reports `fmt.noneavailable` and `canH264Main.0`.

**Cause:** Android has **no video decoders at all** (`adb shell dumpsys media.player` shows an empty "Decoder infos" list).
- The patched `vendor.vhdx` `build.prop` sets **`debug.stagefright.ccodec=0`**. That disables Codec2, the media system the built-in software decoders (`c2.android.*`) belong to.
- With Codec2 off, Android falls back to the OMX decoders listed in `/vendor/etc/media_codecs_google_video.xml` (`OMX.android.latte.*`). The patched image has **no OMX service** to provide them, because in the stock image that service is 32-bit (see below).
- The patch's own hardware decoders (`c2.goldfish.h264/hevc/vp8/vp9.decoder`) never activate. They need `ro.boot.qemu.hwcodec.*` boot properties plus the Android Emulator's host decoder pipe, and WSA provides neither (logcat: `both vsock and goldfish_pipe paths failed`).

**Fix:** change `debug.stagefright.ccodec=0` → `4` inside `vendor.vhdx` with [`Patch-VendorCcodec.ps1`](scripts/Patch-VendorCcodec.ps1). The new value is the same length, so it's a direct byte patch: no Linux tools needed. ext4 doesn't checksum file data, and VHDX doesn't checksum payload blocks. The string appears twice (the file plus a filesystem journal copy), and both get patched.

```powershell
WsaClient /shutdown          # then wait until vmmemWSA has exited
.\scripts\Patch-VendorCcodec.ps1                 # backs up, patches, verifies build.prop via 7-Zip
.\scripts\Patch-VendorCcodec.ps1 -Revert         # undo
```

After restarting WSA, `dumpsys media.player` lists `c2.android.avc/vp8/vp9.decoder` (H.264 up to High profile 5.2, max 2048×2048) plus the audio decoders.

**Then clear YouTube's storage once** (Android Settings → Apps → YouTube → Storage → Clear storage). YouTube saves its "no decoders" result and keeps refusing to play until it's reset.

**Limits:** software decoding only. 1080p is fine, but it costs CPU and battery, and 4K may stutter. No AV1 or HEVC decoder is listed.

| File | SHA-256 |
|---|---|
| `wsa-patched-0.0.2.7z` | `b1582feef760fdd7cd9897de767a37d5d476a91187f1fb7b50e86786e56e265a` |
| `vendor.vhdx` from that archive (unmodified) | `d7604cb53721650592798f0a0b354b234ee502daebda999868de5f5132bb87ae` |
| `vendor.vhdx` after the patch | `2a8279a2ef3e8d8891746b97fa02ef2c1876ae7f4bce6611632a510e121c1d6e` |

### Why not hardware decoding?

The stock image's hardware decoder service, `/vendor/bin/hw/android.hardware.media.omx@1.0-service`, is a **32-bit ARM binary** (ELF32, machine `0x28`), so it can't run on Snapdragon X. The 64-bit plugin it would load (`lib64/libstagefrighthw.so`, which provides the `OMX.android.latte.*` host decoders) exists in both the stock and patched images. The missing piece is a 64-bit build of the service, which Microsoft never shipped. Transplanting one from another Android 13 arm64 build might work, but I haven't tried it.

---

## Root: Magisk build never boots

**Symptom:** with WSABuilds' Magisk build (`…with-magisk-30.6.30600.-stable…`), WSA behaves exactly like Problem 1, even with snickler's patched images: about 2 minutes of "Starting…", then `WsaClient.exe` crashes with `0xc0000409`. Android never writes a log. The same images boot fine with the stock boot image.

**Cause:** a corrupted file, not Snapdragon X. Magisk on WSA is installed through `Tools\initrd.img`, a small boot image whose `/init` is a hook called **lspinit** (from [MagiskOnWSALocal](https://github.com/LSPosed/MagiskOnWSALocal)). It starts WSA's normal init (`/wsainit`) and swaps in `magiskinit` when Android starts. In this build, every `0D 0A` byte pair in lspinit has been rewritten to `0A`: the CRLF → LF conversion Git applies to text files. lspinit is 3 bytes short (387 005 instead of 387 008) and the code after the first damaged spot is misaligned. It dies with `Illegal instruction` before WSA's init ever runs, so the VM never contacts Windows.

Everything else in the boot image is intact: `wsainit` is identical to the stock `/init`, `magiskinit` matches `lib/arm64-v8a/libmagiskinit.so` in Magisk's own APK, and the `.xz` files pass their checksums.

**How it was found:**
1. [`Trace-WsaBoot.ps1`](scripts/Trace-WsaBoot.ps1) records WSA's diagnostic events (TraceLogging provider `Microsoft.Latte.Client`, GUID `30cb12f5-b71b-5a66-0484-11b8118099da`). On the failed boot it showed `HeartbeatTimeout` with `firstHeartbeatReceived=0` and `virtualMachineStarted=0`: the VM starts, but nothing inside it ever talks to Windows. So the failure is in the boot image, before Android starts.
2. Running the boot image's binaries from a normal (non-root) WSA boot via `adb shell` in `/data/local/tmp`: `magiskinit` runs, but `lspinit` gives `Illegal instruction`. The official lspinit runs fine (it prints `Hello from lspinit…` and exits because it isn't PID 1).
3. A byte comparison with the official file showed the CRLF → LF damage.

**Fix:**

```powershell
.\scripts\Repair-MagiskInitrd.ps1 -Source <Magisk build>\Tools\initrd.img -Output .\initrd.magisk-fixed.img
WsaClient /shutdown
Copy-Item C:\WSA\Tools\initrd.img C:\WSA\Tools\initrd.stock.img     # backup
Copy-Item .\initrd.magisk-fixed.img C:\WSA\Tools\initrd.img
```

The script downloads lspinit from a pinned MagiskOnWSALocal commit, checks its SHA-256, confirms the damaged copy is exactly that file with CRLF → LF applied, and writes a new boot image. You only need the Magisk build's `initrd.img`; the rest of the install can be the NoGApps build with snickler's images, as above.

After starting WSA, `adb shell su -c id` gives `uid=0(root) … context=u:r:magisk:s0`, `magisk -v` gives `30.6:MAGISK:R`, and the Magisk app (`com.topjohnwu.magisk`) is installed. Video, networking and installed apps keep working, and root survives restarts. To go back to no root, copy `initrd.stock.img` back.

**Notes:**
- **Magisk app missing from the Start menu / WSA's Apps list.** It comes preinstalled in the image, and WSA only creates entries for apps installed while it's running. Reinstall it over itself once:
  `adb shell 'cp "$(pm path com.topjohnwu.magisk | cut -d: -f2)" /data/local/tmp/m.apk && pm install -r /data/local/tmp/m.apk'`
- **Don't use the app's Update button.** On WSA, Magisk lives in `Tools\initrd.img` on the Windows side, which the app can't patch. A newer Magisk means rebuilding `initrd.img` with the new `magiskinit`/`magisk` files.
- **Zygisk shows "Disabled"** because it's off by default (no `zygisk` key in the Magisk database). Turn it on in Magisk → Settings → Zygisk and restart WSA if you need it (LSPosed, DenyList, Play Integrity modules). Not tested on this setup yet.
- The image includes one module, `lsp_cust` ("WSA Customization" from the MagiskOnWSALocal developers: VPN dialog, device admin, managed users).

| File | SHA-256 |
|---|---|
| Official `lspinit` (MagiskOnWSALocal `b406112`, `bin/arm64/lspinit`) | `19bb3433b76d870327f3ef85a5401f3dc6ecf1424d780f9447b6fac628aba498` |
| `lspinit` in the WSABuilds Magisk build (damaged) | `321af8cf1e93780fa5e809c4eb74a65b787b6f6e47c34db6d83917bdeac82c77` |
| `Tools\initrd.img` in that build (damaged) | `8c3669884981c1dd5eef56d04274f5586bbe66fe1a6bbb52a3319c570b664685` |
| `initrd.img` written by `Repair-MagiskInitrd.ps1` | `6b026492a6db87f36ce609aaaa7372fd268f0f212b9948574fa73e99c39a26a8` |

---

## Installing APKs

1. In WSA Settings → Advanced settings, turn on **Developer mode**.
2. Get adb from Google's [platform-tools](https://developer.android.com/tools/releases/platform-tools) and unpack it to `%LOCALAPPDATA%\Android\platform-tools`.
3. Run `adb connect 127.0.0.1:58526` and approve the "Allow USB debugging" prompt once.
4. Run `adb install app.apk`. For split bundles (`.xapk`/`.apks`), unzip the bundle and run `adb install-multiple *.apk`.

Or run [`Register-InstallToWSA.ps1`](scripts/Register-InstallToWSA.ps1) once, then right-click an APK and choose **Install to WSA**. In Windows 11 it's under *Show more options*. The script handles bundles and OBB game data, and explains errors such as `INSTALL_FAILED_NO_MATCHING_ABIS`.

**Notes:**
- Only `arm64-v8a` apps run: Android reports no 32-bit ABI on this hardware.
- `.apkm` files from APKMirror are encrypted; use APKMirror's own installer app for those.
- `adb root` isn't available: the build is `user` with `ro.debuggable=0`.

---

## "Google Play services wants to access your location" keeps popping up

**Symptom:** with Windows location turned off, a Windows notification keeps saying "Google Play services wants to access your location. Allow location access for Windows Subsystem for Android in Windows privacy settings." In Android Settings → Apps → Google Play services (or Google) → Permissions → Location, every option is greyed out with "Device requires this permission to operate".

**Cause:** Android's own location switch is separate from Windows' and is **on** by default. When an Android app asks for location, WSA forwards the request to Windows. Windows location is off, so WSA shows the notification instead, and Play services keeps asking. The options are greyed out because Play services and the Google app are privileged system apps: their location permission is locked by the system image and can't be changed in Settings without root.

**Fix:** turn off location for the whole Android system. It applies to every app, including the locked ones, so WSA no longer has anything to ask Windows for.

```powershell
adb -s 127.0.0.1:58526 shell cmd location set-location-enabled false   # true to turn it back on
```

Or in Android Settings → Location, turn off **Use location**.

**WSA turns location back on at every boot**, so this only lasts until WSA restarts. With root, make it permanent with the Magisk boot script [`disable-location.sh`](magisk/service.d/disable-location.sh). It waits for boot to finish, then switches location off:

```powershell
adb -s 127.0.0.1:58526 push magisk\service.d\disable-location.sh /data/local/tmp/
adb -s 127.0.0.1:58526 shell "su -c 'mkdir -p /data/adb/service.d && cp /data/local/tmp/disable-location.sh /data/adb/service.d/ && chmod 755 /data/adb/service.d/disable-location.sh'"
```

To undo, delete `/data/adb/service.d/disable-location.sh` (as root) and restart WSA.
