# WSA on Snapdragon X

Notes and scripts for getting Windows Subsystem for Android running on a **Snapdragon X** (Oryon) laptop, which has no 32-bit ARM support. It covers booting, networking behind simplewall, and fixing video playback.

**Result:** Android 13 boots, Play Store sign-in works, YouTube plays (software decoding). No root.

| | |
|---|---|
| PC | Snapdragon X Plus X1P64100 (10 cores), Adreno X1-85, 16 GB RAM |
| Windows | 11 Home, build 26200 (25H2), ARM64 |
| WSA build | [WSABuilds](https://github.com/MustardChef/WSABuilds) LTS 8: `WSA_2407.40000.4.0_arm64_Release-Nightly-NoGApps-NoAmazon.7z` |
| Android images | [snickler/WSA-Patched](https://github.com/snickler/WSA-Patched) 0.0.2: `wsa-patched-0.0.2.7z` (4 × `.vhdx`) |
| Install folder | `C:\WSA` (WSA runs from it, so it must stay put) |
| Firewall | [simplewall](https://github.com/henrypp/simplewall) 3.9.1, allow-list mode |

## Scripts

| Script | What it does |
|---|---|
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
- **Patched images with the Magisk build** crash the same way, because the Magisk boot hook (initrd) also fails on Snapdragon X. Root isn't currently achievable on this hardware.

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
