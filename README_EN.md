# MIB2 Toolbox — CarPlay AltScreen V3.7Fix6

**English** | [简体中文](README.md)

This project is designed for the Audi **MHI2Q** platform and displays the **native CarPlay AltScreen / secondary navigation view** directly on the vehicle's **Virtual Cockpit**. The core display path has been verified in a vehicle. Read this document in full before making changes to the head unit.

**V3.7 update: full RGI navigation-data integration is now available (built on [Luka's mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi)), color conversion for the secondary display now runs on the GPU, cutting whole-system CPU usage from about 80% to about 20%, the runtime watermark has been removed, and starting with V3.7 the whole project is open source under GPL-3.0.**

**Wireless CarPlay: tested with the 君用 (Junyong) M2Pro wireless adapter on its latest beta firmware, which fully supports the cluster display, RGI, steering-wheel zoom and CarPlay projection.**

**V3.7Fix6 update: fixes the missing secondary display on the cluster once the LTE data link is up. The head unit then switches to per-uplink firewall configurations such as `pf.ecm0.conf`, and the installer now writes the 6031 pass rule into those as well.**

**Current startup and display optimization: the stock-map readiness probe is reduced from 500 ms to 200 ms; a normal `stream_ready` start reuses the already validated ready-frame progress instead of waiting for two frames again; CarPlay backing opacity is capped at 70/100 so the stock map remains visible underneath.**

**V3.7Fix5 update: the secondary-display data port is fixed at 6031 and written into the head unit's firewall configuration, fixing the black cluster at cold start and cluster dropouts when LTE changes IP; at cold start the secondary display waits until the stock cluster map has loaded; fixes RGI lane guidance disappearing mid-route; the startup video now fills the cluster 1:1; the green menu is trimmed to three entries, and boot logs are saved to the SD card automatically.**

> [!CAUTION]
> **Disclaimer**
>
> - Every change this project makes is written to, and affects, the head unit's system files. Installing, updating, starting and restoring all modify stock programs, configuration and firewall rules, and can affect how the head unit works.
> - This project is provided free of charge and "as is", without warranty of any kind (see sections 15 and 16 of the GPL-3.0).
> - **By using this project you accept these risks and take full responsibility for the consequences. The authors and contributors accept no liability for any problem arising from using, installing, restoring or modifying this project, including but not limited to head-unit faults, malfunctions, failure to boot, data loss, voided warranty, or any other direct or indirect damage.**
> - If you do not agree to these terms, do not install or use this project.

> [!CAUTION]
> **Public statement: we strongly condemn VWODIS (一车网) for stripping the authors' names and license from this project and reposting it to draw traffic**
>
> VWODIS (一车网, QQ: **296520384**) has published this project on its website as "【Software】Audi MHI2Q-CARPLAY-Altscreen Native Second-Screen Navigation Activation Software V3.7" ([en.vwodis.com/20912.html](https://en.vwodis.com/20912.html)), with login, paid-points and membership download gates. The listing:
>
> - does not name the original authors or link to this GitHub repository;
> - does not include the GPL-3.0 license or any way to obtain the source code;
> - takes its description from this project's README.
>
> GPL-3.0 allows charging for copies, but anyone who redistributes this project must keep the authors' copyright and license notices and provide the complete source code (sections 4 and 6). Redistributing in violation of the license automatically terminates all rights granted under it (section 8). When the author contacted them, they admitted posting it, then replied: "We haven't kept the source closed, and it isn't encrypted", "It's just to attract traffic", "What does it have to do with you?" and "Then just don't publish it."
>
> **We strongly condemn taking open-source authors' work to draw traffic for themselves while erasing the authors' names and license, and we demand that they correct the listing or take it down immediately.**
>
> **This project is published free of charge only on GitHub ([yuedizhibo/MHI2Q-CarPlay-AltScreen](https://github.com/yuedizhibo/MHI2Q-CarPlay-AltScreen)). Any paid, points-based or membership download channel has nothing to do with the authors.** Files from third-party channels may be modified or mixed with old versions; download only from this repository's Releases.

<details>
<summary>Chat with the site operator (in Chinese, click to expand)</summary>

<img src="docs/vwodis-chat.jpg" width="380" alt="Chat with VWODIS (一车网)">

</details>

> [!NOTE]
> **Sister project: MMI Mirror**  
> If you want to mirror the **entire MMI center display** instead of using the native CarPlay secondary display, see:  
> **[MHI2Q-CarPlay-MMI-Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)**

> [!TIP]
> **Community**  
> Join the community to discuss installation and use, report problems, and share vehicle test results:  
> - Telegram: [https://t.me/+xZ2pabi2nmk1MDI9](https://t.me/+xZ2pabi2nmk1MDI9)  
> - QQ group: **823297190**

> [!WARNING]
> **⚠️ A note before you start**
>
> Because freely shared test builds were previously repackaged and resold without permission, earlier versions of this project published only the runtime package and released features in stages. **Starting with V3.7, the project is fully open source**: every feature and all source code are published in this repository.
>
> The whole project is licensed under the **[GNU GPL v3.0](LICENSE)**: anyone may use, modify, and redistribute it, and anyone who redistributes binaries or modified versions must also provide the complete source under GPL-3.0. See "Licensing, authors, and third-party files" below.
>
> This is not a demonstration build. The current CarPlay AltScreen functionality is already usable in a real vehicle.
>
> This project was originally developed around our own vehicles and day-to-day use cases. **China-region (CN) AUG22 firmware has been tested in a vehicle and works normally.** Some European-region (ER) AUG22 firmware has been reported working in vehicles by users (for example `MHI2Q_ER_AUG22_P5092` / `P5152`, MU 1329). Other ER versions and firmware for US and other regions may have unknown bugs and are **not guaranteed to work 100%**; assess the risk yourself and keep your stock backup. The MHI2 platform is not supported. Do not bypass the installer's firmware checks to force installation.
>
> **Shared free of charge.** The source and installation packages are available for free on GitHub; do not pay for them.
>
> You are welcome to learn from, study, and discuss the project. If someone provides you with this project or a modified version of it, the GPL-3.0 entitles you to request the complete source from them.

> [!IMPORTANT]
> This project modifies system files on the head unit. Keep the SD card inserted and maintain stable power during installation, start, or recovery operations.  
> **After installation or recovery, fully reboot the head unit / HMI as instructed before judging the result.**
>
> Do not perform installation, update, recovery, or troubleshooting while driving.

---

## Vehicle demonstration

<img width="1920" height="1080" alt="CarPlay AltScreen on Virtual Cockpit" src="https://github.com/user-attachments/assets/f582d179-8c8e-41ac-882b-24d623813fca" />

---

## Currently included in the public release

- Native CarPlay AltScreen
- The main CarPlay display remains available and unaffected
- Wireless CarPlay: the 君用 (Junyong) M2Pro wireless adapter on its latest beta firmware has been tested to support the cluster display, RGI, wheel zoom and CarPlay projection
- Full RGI navigation-data integration (built on [Luka's mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi))
  - Maneuver arrows and lane guidance on the cluster
  - Can be enabled or left out at install time (`INSTALL WITH RGI` / `INSTALL NO RGI`)
- Classic / Sport dynamic layout adaptation
- Global centering
- Left steering-wheel scroll-wheel zoom
- Cluster map shows the speed-limit sign and compass, without an ETA overlay
- GPU color conversion: whole-system CPU usage cut from about 80% to about 20%
- At cold start, the secondary display waits for the stock cluster map to be ready
- Fixed-port firewall rule for the secondary display; no dropouts when LTE changes IP
- Safe installation and recovery
- Protection against interrupted installation / recovery
- Restoration of stock configuration
- Boot logs saved to the SD card automatically; stock backups kept on the SD card

## Compatibility

| Item | Status |
|---|---|
| Platform | **MHI2Q** only; MHI2 is not supported |
| Firmware version | **AUG22**; the installer checks the head unit's firmware version and refuses anything other than AUG22 |
| China-region (CN) firmware | ✅ Vehicle-tested and works normally |
| Some European-region (ER) firmware | ✅ Reported working in vehicles by users (for example `MHI2Q_ER_AUG22_P5092` / `P5152`, MU 1329) |
| Other ER versions, US and other regional firmware | ⚠️ May have unknown bugs; not guaranteed to work 100% |
| Wireless CarPlay adapter | ✅ 君用 (Junyong) **M2Pro** on its latest beta firmware: tested with full support for the cluster display, RGI, wheel zoom and CarPlay projection |
| iPhone iOS version | ✅ **iOS 26** recommended; ⚠️ below iOS 18, Baidu Maps may not support the cluster display and Amap (Gaode) may show the map with the wrong aspect ratio |

> [!TIP]
> This project was adapted and tested mainly on **iOS 26**. If Baidu Maps does not appear on the cluster, or Amap (Gaode) looks stretched or squashed, update the iPhone to iOS 18 or later (iOS 26 recommended) before troubleshooting further.

---

## How it works and architecture

This project no longer relies on the early Window58 readback route. It connects directly to CarPlay's **private type111** secondary-display video stream: the stock AirPlay / OMX decoding path is kept, frames are safely read from the stock renderer and linearized into standard NV12, handed to a separate display process, and finally shown on the cluster through GLES / displayable3 / Java Context80. Full RGI receives navigation data through iAP2 RouteGuidance, and the Java HMI forwards it to a separate maneuver renderer.

### Architecture diagram

```mermaid
flowchart TB
    iPhone["iPhone CarPlay"]

    subgraph DIO["dio_manager"]
        OMX["Stock OMX decoder + renderer"]
        ALT["libcarplay_altscreen.so<br/>secondary display · viewArea<br/>wheel zoom → changeMapZoomLevel"]
        RGIM["libcarplay_rgi_meta.so<br/>iAP2 RouteGuidance"]
    end

    SHM[("/carplay111_decoded<br/>NV12 shared memory")]

    subgraph HMI["Java HMI"]
        BUS["CarplayBus<br/>TCP 19810"]
        RG["RouteGuidance"]
        RS["RendererServer<br/>TCP 19800"]
        CSC["ClusterStateController<br/>Context80"]
        WZ["WheelZoomBridge"]
    end

    subgraph SIDE["Sidecars"]
        MIR["carplay-alt111-mirror-display<br/>secondary display · GPU color conversion · startup logo"]
        MR["maneuver_render<br/>maneuver arrows · lane guidance"]
    end

    VC["Virtual Cockpit"]

    iPhone -- "type111 secondary video" --> OMX
    iPhone -- "iAP2 navigation data" --> RGIM
    ALT -. "safe frame readback" .-> OMX
    OMX --> SHM --> MIR
    RGIM -- "TCP" --> BUS --> RG
    RG --> RS -- "TCP" --> MR
    WZ -- "wheel event queue" --> ALT
    MIR -- "displayable3 · map" --> VC
    MR -- "displayable 98 · arrows" --> VC
    CSC -- "switches Context80" --> VC
```

### Secondary-display path

```text
iPhone CarPlay
  ↓
private type111 secondary-display video stream
  ↓
Stock AirPlay / OMX decoding
  ↓
QNX Screen readback + linearization → standard NV12 (/carplay111_decoded)
  ↓
Separate display process (carplay-alt111-mirror-display)
  ↓
GPU shader converts NV12 → RGBA
  ↓
GLES / displayable3 (1440×542 source shown 1:1 on the 1440×455 cluster plane)
  ↓
Java/HMI Context80
  ↓
Virtual Cockpit
```

- The main CarPlay display (Main110) stays on the stock path and is not part of this display path.
- No extra decoder is introduced; the stock decoding path that already works on MHI2Q is reused to keep new variables to a minimum.
- The FULL / SMALL view areas switch dynamically within the same CarPlay session through the standard `updateViewArea`; the Classic / Sport layout follows the head unit's HMI state.
- Java/HMI is the only owner of Context80; the display process does not change the cluster Context directly.
- Secondary-display data uses fixed TCP port 6031: the installer writes a pass rule, ahead of the carplay0 block rule, into the head unit's `pf.conf` and into the per-uplink `pf.ecm0.conf`, `pf.ppp0.conf`, and `pf.mlan0.conf`, so it loads at boot with the stock firewall configuration and no `pfctl` runs while the secondary display is being set up. When the head unit reloads the firewall configuration because LTE changed IP, or switches to `pf.ecm0.conf` and the like once the LTE data link is up, the rule is loaded again with it. The rule goes into all of these files or into none; if it is not written or the port is taken, the hook falls back to a random port plus a runtime pass rule.

### Cold start: waiting for the stock cluster map

At cold start, the Java HMI checks every 200 ms, on the stock navigation thread, whether the cluster map (MapKombi) has been initialized; a completed map probe wakes the wait immediately. Once it has, it notifies `libcarplay_rgi_meta.so` over the local bus (TCP 19810, command `0x0119`), which hands the notice to `libcarplay_altscreen.so`. Until then the hook does not ask the iPhone for the secondary-display picture (`showUI` / `forceKeyFrame`), does not switch view areas, and the display process does not start, so both the startup video and the secondary display appear only after the stock map is ready. The main CarPlay display, the secondary-display capability exchange, and the iPhone setting up the secondary video stream are not delayed.

This waits for the stock cluster map object to finish initializing, not for a GPS fix or an active route. Readiness is kept in memory only; hiding the stock map later does not close the gate again. There is no timeout: if the stock cluster map never comes up, the secondary display keeps waiting.

When the display process is started normally from the `stream_ready` marker, it validates the producer PID, generation, cookie, sequence and current NV12 frame, then reuses the marker's already proven two-frame readiness progress instead of waiting for two more frames. An abnormal restart, a producer identity change or a failed validation still requires two fresh stable frames from the same session. The CarPlay backing layer is capped at opacity 70/100, leaving about 30% of the stock map visible beneath the card.

### Performance: GPU color conversion

In earlier versions, the display process converted each decoded NV12 frame to RGBA on the CPU before uploading it to the GPU, which put a heavy load on the head unit. V3.7 moves the NV12 → RGBA color conversion to the GPU: the display process uploads the Y and UV planes as separate textures and a GLES fragment shader does the conversion, so the CPU no longer touches every pixel and no intermediate RGBA buffer is allocated.

In vehicle testing, **whole-system CPU usage dropped from about 80% to about 20%**.

### RGI navigation-data path

- `libcarplay_rgi_meta.so` receives iAP2 RouteGuidance data inside the stock CarPlay process and passes it to the Java HMI over local TCP port 19810. Both install modes load it and start this local bus: `INSTALL NO RGI` only turns off the navigation-data module, while notices such as stock-map readiness still travel over the bus.
- Maneuver arrows and lane guidance are sent over local TCP port 19800 to the separate `maneuver_render` process, which draws them on displayable 98. The process is supervised by `rgi_supervisor.sh` and restarted a limited number of times if it exits unexpectedly.

### Steering-wheel zoom

The Java HMI captures the left steering-wheel scroll events and writes them to an event queue. `libcarplay_altscreen.so` steps toward the target zoom level by sending standard `changeMapZoomLevel` requests to the iPhone, pacing them by fresh secondary-display frames.

---

## Source layout and building

Starting with V3.7, all source code is in this repository:

| Path | Contents | Output |
|---|---|---|
| `Toolbox/carplay_alt_screen/src/` | Preload hook for the stock CarPlay process: private111 secondary display, frame readback, viewArea, wheel zoom | `universal/libcarplay_altscreen.so` |
| `Toolbox/carplay_alt_screen/mirror_display/` | Secondary-display process (C++ / GLES / displayable3) with GPU color conversion and the embedded startup logo | `mirror_display/release/carplay-alt111-mirror-display` |
| `Toolbox/carplay_alt_screen/rgi_native/` | RGI preload hook: iAP2 RouteGuidance parsing and forwarding, stock-map readiness forwarding | `rgi_meta/libcarplay_rgi_meta.so` |
| `Toolbox/carplay_alt_screen/rgi_renderer/` | Maneuver-arrow / lane-guidance renderer | `rgi_renderer/release/maneuver_render` |
| `Toolbox/carplay_alt_screen/hmi/` | Java HMI hook: Context80, cluster layers, RGI distribution, wheel events, stock-map startup wait; `stubs/` holds compile-only stock API stubs and `vendor/` the baseline JAR | `hmi/carplay_hook-basevideo3.jar` |
| `Toolbox/scripts/`, `Toolbox/GEM/` | Install / start / status / restore / diagnostic scripts and the green menu | Used from the SD card |
| `Tools/`, `BUILD-*.sh` | Build and verification tools | — |

Building requires the QNX 6.5.0 SDP ARM cross toolchain (`arm-unknown-nto-qnx6.5.0eabi-gcc`):

```sh
sh BUILD-UNIVERSAL-QNX.sh      # secondary-display hook → libcarplay_altscreen.so
sh BUILD-MIRROR-QNX.sh         # secondary-display process → carplay-alt111-mirror-display
bash Tools/build_rgi_qnx.sh    # RGI hook + renderer → libcarplay_rgi_meta.so, maneuver_render
```

Build output goes to `dev-build/` or `mirror_display/build/` (not tracked in git) and does not overwrite the vehicle files in `release/`. When you replace vehicle files, also update the matching `BUILD_INFO.txt`, `SHA256SUMS`, and the root `SHA256SUMS-SD.txt`.

---

## Installation and testing

> [!IMPORTANT]
> **This repository is now an AltScreen overlay only. It no longer contains the complete MIB2 Toolbox installer.**
>
> - The **red software-update menu** is only for installing or repairing the upstream [jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox).
> - **This project itself cannot be installed directly through the red software-update menu.**
> - After the upstream Toolbox is working, load this project through **`MQBCoding → Update Toolbox`** in the green menu.
> - If `Update Toolbox` reports `Script not found` or `/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh` is missing, repair/reinstall the upstream Toolbox first.
> - **Upgrading from an older version no longer needs a manual restore**: the installer removes the old version automatically before installing. See section 8.

### 1. Confirm that the upstream MIB2 Toolbox works

1. Check that the head unit's firmware version is **AUG22**. The installer verifies the firmware version and refuses non-AUG22 firmware. Stop if the version differs or cannot be confirmed; do not bypass the checks. For firmware outside the China region, read "Compatibility" above first.
2. Park the vehicle and maintain stable power. Back up the current SD card and stock files, then prepare a writable **FAT32** SD card. If the old card contains `MMI-Cockpit-Carplay`, preserve that entire directory when changing cards because it contains stock backups needed for restoration.
3. If `Green Developer Menu → MQBCoding` already works and **`Update Toolbox` runs normally without a Script not found error**, skip directly to section 2.
4. If the upstream Toolbox is not installed, or the green menu exists but `Update Toolbox` is broken / missing its script, download and extract the latest complete **[jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox)** package. Copy the **contents** of that upstream package to the SD-card root without an extra ZIP-name folder.
5. Insert only that SD card in the head unit. Enter the red menu and select `Software updates/versions → Update → SD card → MQB Coding MIB2 Toolbox`. Wait for the update and all automatic reboots to finish; do not remove the card or cut power early.
6. After reboot, open `Green Developer Menu → MQBCoding` and confirm that **`Update Toolbox` runs normally**. If the upstream package is not recognized, check FAT32 format, root layout, and the upstream instructions. Stop if it is still rejected; do not force-flash it.

### 2. Merge this project overlay into the upstream Toolbox SD card

1. Keep the **complete upstream MIB2 Toolbox SD-card contents**. Do not delete its `metainfo2.txt`, `Toolbox/final/`, `Toolbox/GEM/mqb-main.esd`, `Toolbox/scripts/update_toolbox.sh`, or other upstream files.
2. Download the package from this repository's [Releases](https://github.com/yuedizhibo/MHI2Q-CarPlay-AltScreen/releases) page and extract it. These are compiled vehicle overlay files; **you do not need to copy source code or build directories**. Merge the package's **`Toolbox/` directory into the existing `Toolbox/` directory on the SD card**:
   - Replace same-name files with this project's versions.
   - Keep all upstream-only files.
   - When upgrading from an older project build (after completing the restore in section 8), delete the old `logo.rgba` and `watermark.rgba` from `Toolbox/carplay_alt_screen/mirror_display/release/` on the card; the new binary does not use them.
   - **Do not wipe the upstream Toolbox first, and do not treat this repository as a standalone red-menu update package.**
3. Also copy `SHA256SUMS-SD.txt` from the package root to the SD-card root for the check in the next step. If changing cards, also preserve the complete `MMI-Cockpit-Carplay` stock-backup directory.
4. The resulting layout should look like:

```text
SD card root/
├─ metainfo2.txt                  ← keep from upstream Toolbox
├─ Toolbox/
│  ├─ final/                      ← keep from upstream Toolbox
│  ├─ GEM/
│  │  ├─ mqb-main.esd             ← keep from upstream Toolbox
│  │  └─ mqb-carplayAltScreen.esd ← this project
│  ├─ scripts/
│  │  ├─ update_toolbox.sh        ← keep from upstream Toolbox
│  │  └─ ...AltScreen scripts...  ← this project
│  └─ carplay_alt_screen/         ← this project payload
└─ SHA256SUMS-SD.txt
```

5. Do not add an extra `SD card/Toolbox/` or `MHI2Q-CarPlay-AltScreen/Toolbox/` directory level.
6. From the SD-card root, run:

```sh
sha256sum -c SHA256SUMS-SD.txt
```

with Git Bash, Linux, or another environment that provides `sha256sum`. Confirm that every project file reports `OK`. This manifest validates this project's overlay files, not the complete upstream Toolbox.

### 3. Load this project's menu and scripts through the green menu

1. Reinsert the merged SD card.
2. Open `TESTMODE → Green Developer Menu → MQBCoding`.
3. Run **`Update Toolbox`**. The upstream Toolbox update script copies the SD card's `Toolbox/scripts/` and `Toolbox/GEM/` contents into the head unit.
4. Exit and reopen the green menu, then open:

```text
Customization
└─ MMI-Cockpit-Carplay
```

   Check that the version in the menu header matches the package you downloaded (currently **`Version: V3.7 Fix6`**) and that the menu has only three entries: `INSTALL NO RGI`, `INSTALL WITH RGI`, and `RESTORE ORIGINAL`. If it still shows an older version, the menu and scripts on the head unit are still the old ones, and `INSTALL` would run the old installer; run `Update Toolbox` again and continue only once the version is correct.

5. If you still see:

```text
Script not found:
/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh
```

the **upstream Toolbox base installation is still broken**. Do not continue with this project's `INSTALL`; return to section 1 and repair the upstream Toolbox first.

### 4. Install the secondary display

Installing and enabling are now one step, so **only one reboot is needed**:

1. **Disconnect the iPhone / CarPlay** so navigation video is not playing during installation.
2. In the `MMI-Cockpit-Carplay` menu, select one install mode:
   - `INSTALL WITH RGI` (recommended): secondary-display map + full RGI (maneuver arrows, lane guidance).
   - `INSTALL NO RGI`: secondary-display map only, without the RGI components; useful if you only want the map or are troubleshooting RGI.
3. Wait for the final `RESULT:` line. Do not remove the card, cut power, or reboot before it appears. The installer runs five steps and shows each one live:

   ```text
   [1/5] Checking package and firmware...
   [2/5] Checking for a previous installation...   (removes an old version automatically)
   [3/5] Installing AltScreen + RGI...            (rolls back on any error)
   [4/5] Enabling autostart...
   [5/5] Verifying installation...
   ```

4. After `RESULT: SUCCESS`, **fully reboot the head unit once**, then connect the iPhone, enter CarPlay, start navigation, and check that the Virtual Cockpit shows the secondary display and updates with navigation. At cold start the secondary display appears only after the stock cluster map has loaded; this is expected.
5. If it shows `RESULT: FAILED`, the lines below it say which step failed, what state the head unit is in now (unchanged / rolled back / restored to stock), and what to do next. The full run is in the SD-card log shown after `Log:`.

The install mode is recorded as `INSTALL_RGI_MODE=WITH / NO` in the install log on the SD card (`MMI-Cockpit-Carplay/logs/operations/install_*.log`). To switch modes, simply run INSTALL again with the other mode; the installer removes the current version first.

> [!NOTE]
> When the secondary display starts, it first plays a startup video of about 5 seconds (filling the cluster) and then switches to the CarPlay picture. This is expected. It is not the vehicle boot logo.

### 5. Troubleshooting

- Judge success by the **image actually shown on the Virtual Cockpit**.
- If the `MMI-Cockpit-Carplay` menu is missing, first confirm that the upstream green menu works, the SD-card directory layout is correct, and `MQBCoding → Update Toolbox` completed successfully.
- If `Update Toolbox` itself reports `Script not found`, that is an upstream Toolbox base-installation problem rather than an AltScreen installer problem. Repair the upstream Toolbox through the red software-update menu first.
- If the cluster never shows the secondary display after a cold start, first check that the stock navigation map shows normally on the cluster. This project waits for the stock cluster map to initialize before asking for the secondary display, so if the stock map does not come up, the secondary display will not appear either. In the boot log `hook_tmp.log`, `PHASE=OEM_MAP_READY` means the wait is over; only `PHASE=ALT111_REQUEST_DEFERRED ... reason=oem_map_not_initialized` means it is still waiting for the stock map.
- If Baidu Maps does not appear on the cluster or Amap (Gaode) shows the wrong aspect ratio, first make sure the iPhone runs iOS 18 or later (iOS 26 recommended).
- If the SD card is not detected, check FAT32, root layout, and read/write status. If the secondary display shows no picture, confirm that CarPlay is connected and navigation is producing video, then save the logs and report it; do not reinstall repeatedly.

### 6. Logs

- After installation, each boot's runtime logs are saved automatically to `MMI-Cockpit-Carplay/logs/boots/boot_*/` on the SD card (the card must be inserted at boot). They cover private111 connect / teardown, H.264, Screen readback, decoded SHM, display frame rate, displayable3, Context80, view-area and layout state, firewall rules and network state, and the stock system log.
- Logs from install, restore, and other operations are saved to `MMI-Cockpit-Carplay/logs/operations/` on the SD card.
- Live logs on the head unit are under `/tmp/`, for example `/tmp/altscreen_hook.log` and the RGI renderer's `/tmp/maneuver_render.log`.
- When something goes wrong, save the complete logs before changing any configuration or code. When reporting an issue, attach the whole `MMI-Cockpit-Carplay/logs/` directory from the SD card and include the firmware version and region, the Classic / Sport and FULL / SMALL layout, and how the phone was connected (phone plugged in before the head unit started / after it fully started / quick reconnect).

### 7. Restore the stock configuration

1. Insert the SD card that retains the `MMI-Cockpit-Carplay` stock-backup directory and select `RESTORE ORIGINAL`. Logs from earlier boots are already on the SD card, so there is no need to export them before restoring.
2. Wait for the final `RESULT:` line. `RESULT: SUCCESS` means the stock configuration is back; then fully reboot the head unit. Restore stops the display process, releases the Context80 display demand, removes the startup entries, removes this project's HMI JAR, and restores the HMI files and preload configuration saved before installation.
3. If it shows `RESULT: FAILED`, follow the lines below it; if they say the rollback was incomplete, **do not reboot** and run `RESTORE ORIGINAL` again. A head unit that is already stock shows `RESULT: NOTHING TO RESTORE`.

### 7.1 Rescue restore with a backup from the same model and firmware

Normal `RESTORE ORIGINAL` and the automatic uninstall in INSTALL step 2 check the live files against the stock backup. Differences beyond this project's expected edits are refused. A VIN mismatch now produces a warning that a backup from another vehicle is being used; it does not bypass the normal file checks.

If damaged files prevent normal restore, you can enable rescue mode manually. **Use a complete, clean stock backup from the same head-unit model and the exact same firmware train.** Different regions or trains are not interchangeable, and matching MU numbers alone are insufficient. The script enforces the recorded train; confirm the matching head-unit model before borrowing a backup.

1. Save the current SD card, your own stock backup, and the logs to a separate directory on your computer first. Preserve those original copies.
2. Use the current Toolbox package. Place the **complete matching backup directory** under `MMI-Cockpit-Carplay/backup/` on the SD card, including `original`, `firewall-original`, `universal-hook-original`, other original subdirectories, and checksum files. Do not copy only selected libraries or JSON files.
3. Create an empty **file** named **`MMI-Cockpit-Carplay/RESCUE_RESTORE`** on the SD card. It is not a directory and has no `.txt` extension. Enable file-extension visibility in Windows and verify that it is not `RESCUE_RESTORE.txt`.
4. Insert the SD card, keep power stable, disconnect the phone, and select **`RESTORE ORIGINAL`** in the green menu. The screen shows `RESCUE MODE`. A firmware mismatch, unreadable train, incomplete backup, or checksum failure is refused. **Legacy backups without a recorded firmware train cannot be used for rescue; there is no second override marker.**
5. Rescue permits overwriting differing system binaries and configurations: `dio_manager`, `libairplay.so`, `libNmeBaseClasses.so`, the two CarPlay JSON configurations, `pf.conf`, and per-uplink firewall configurations such as `pf.ecm0.conf`. A separate transaction snapshot captures this head unit's current state before writes so a failed restore can roll back. That snapshot is not a new clean stock backup and does not replace either original backup. Earlier interrupted transactions are retained under `install-transaction/rescue-preserved.*` or `restore-transaction/rescue-preserved.*`; they are not used as rescue payloads.
6. Wait for **`RESULT: RESCUE SUCCESS`**, save the logs, delete **`MMI-Cockpit-Carplay/RESCUE_RESTORE`**, and fully reboot the head unit. On failure, follow the result instructions; the marker's presence does not mean rescue succeeded.

The marker applies to the manual `RESTORE ORIGINAL` only. While the marker is present, every later manual restore runs in rescue mode, so always delete it once the rescue is done. **INSTALL's automatic uninstall always retains normal strict checks, even with this marker present.** Rescue restores only files covered by this project's backup; it does not replace a full firmware reflash or guarantee repair of other damaged system files.

### 8. Upgrade from an older version

Upgrading **no longer needs a manual restore**:

1. Disconnect the iPhone / CarPlay and insert the SD card that retains the `MMI-Cockpit-Carplay` stock-backup directory (the card used for the original install).
2. Merge the new overlay into that SD card as described in section 2, and delete the leftover `logo.rgba` and `watermark.rgba` from the old version. Keep the `MMI-Cockpit-Carplay` directory on the card intact.
3. Run `Update Toolbox` as described in section 3, then run `INSTALL WITH RGI` or `INSTALL NO RGI` as described in section 4. When the installer finds an old version, it first runs a full, verified restore, then installs the new version.
4. After `RESULT: SUCCESS`, fully reboot the head unit once.

If the card holds no stock backup (for example, after switching SD cards), the installer stops at step 2 with a message and does not change the head unit.

Changing head-unit system files can cause a blank screen or require recovery.

---

## Contributing

**V3.7 is fully open source, and everyone is welcome to help maintain the project, fix issues, and add new features.**

- Report problems through Issues, with complete logs, the firmware version and region, the layout, and how the phone was connected. For general discussion, join the [Telegram group](https://t.me/+xZ2pabi2nmk1MDI9) or QQ group **823297190**.
- Submit fixes and new features through Pull Requests; see "Source layout and building" above for how to build. Vehicle test results on US / ER and other regional firmware are also welcome and help widen the validated scope.
- In a pull request, describe the test vehicle, firmware version, test steps, and results.
- Change one layer at a time: do not introduce a new decoder, a new Context, and broad display-structure changes in the same change, or it becomes hard to tell which layer caused a problem.
- Vehicle tests should cover cold start (phone plugged in before the head unit started / after it fully started / quick reconnect) and the four Classic / Sport × FULL / SMALL layouts, and confirm that `RESTORE ORIGINAL` still restores correctly.
- Contributed code is released with this project under GPL-3.0.

---

## Licensing, authors, and third-party files

This project is developed by [yuedizhibo](https://github.com/yuedizhibo) and [Lanye-z](https://github.com/Lanye-z). **V3.7 is fully open source**: this repository publishes all C/C++ source code, runtime binaries, installation scripts, and documentation, and the whole project is released under the **[GNU General Public License v3.0](LICENSE)** (GPL-3.0).

The GPL-3.0 allows anyone to use, study, modify, and redistribute this project, including for commercial purposes. Anyone who redistributes binaries or modified versions must provide the complete corresponding source under GPL-3.0 and keep the existing copyright and license notices.

| Part | Origin | License |
|---|---|---|
| Secondary-display path (`src/`, `mirror_display/`), install / restore / diagnostic scripts, green menu, documentation, etc. | Original to this project | GPL-3.0 |
| Java HMI and full RGI (`hmi/`, `rgi_native/`, `rgi_renderer/`) | Built on [Luka's mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi); renderer merged from [Allemon/mib2-carplay-rgi-altscreen](https://github.com/Allemon/mib2-carplay-rgi-altscreen) | GPL-3.0 |
| Upstream MIB2 Toolbox files | [jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox) | [MIT](LICENSE.TOOLBOX-MIT), GPL-3.0 compatible |
| Mirror runtime component | [Lanye-z's MMI Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror) | [Unlicense](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR), GPL-3.0 compatible |

Other third-party files shipped with the source (such as `stb_image.h` and the Unicode data file) keep their own licenses. Keep the original license notices of third-party files.

---

## Version status

Current recommended version:

~~~text
main
└── AUG22 / V3.7Fix6
    ├── China-region (CN) firmware: vehicle-tested and working
    ├── Some European-region (ER) firmware: reported working by users (P5092 / P5152, MU 1329)
    └── Other regional firmware: may have unknown bugs; not guaranteed to work 100%
~~~

The current public release prioritizes stable installation, normal use, and reliable recovery.

### Changelog

**V3.7Fix6**

- Fixed the secondary display not appearing on the cluster, or cutting out right after it appears, once the LTE data link is up. When the LTE module brings up its data link over ECM, the head unit loads `pf.ecm0.conf` instead of `pf.conf`; PPP dial-up and WLAN client use `pf.ppp0.conf` and `pf.mlan0.conf`. V3.7Fix5 wrote only `pf.conf`, so after the switch the 6031 pass rule was gone and the iPhone's secondary-display data was dropped. The installer now writes the pass rule into these files as well; if any of them cannot take it, none does, and the hook falls back to a random port plus a runtime rule.
- The original contents of these files are backed up to `MMI-Cockpit-Carplay/backup/firewall-variants-original/` on the SD card and restored byte for byte by `RESTORE ORIGINAL`. On head units installed before V3.7Fix6, restore removes the project's rule block from these files. The install and restore transaction snapshots cover these files too, so a failure partway through rolls them back.
- Boot logs gain `firewall_sources.txt.log`, which records whether each firewall configuration carries the 6031 rule and which rule set is active.
- The green menu version label is now `V3.7 Fix6`. The hook, RGI, HMI, and display process are the same as in V3.7Fix5.

**V3.7Fix5**

- The secondary-display data port is fixed at TCP 6031: the installer writes a pass rule into the head unit's firewall configuration `pf.conf`, which loads at boot with the stock configuration, and no `pfctl` runs while the secondary display is being set up. Fixes the black cluster at cold start caused by `pfctl` timeouts, and cluster dropouts when the head unit reloads its firewall configuration after LTE changes IP and the runtime pass rule is lost. Before writing, `pfctl` checks the result: if the original configuration parses but the one with the rule does not, the rule is not written and the hook falls back to a random port plus a runtime rule; the install does not fail.
- At cold start, the secondary display waits until the stock cluster map (MapKombi) has initialized before asking the iPhone for the picture and starting the cluster display; the main CarPlay display and the secondary-display negotiation are not delayed.
- `INSTALL NO RGI` now also starts the local bus between the HMI and the hook, which carries the stock-map readiness notice.
- Fixed RGI lane guidance showing only at the start of navigation and then disappearing: lane data uses its own IDs and the cache is no longer pruned by maneuver index.
- New startup video that fills the 1440×455 cluster plane pixel for pixel instead of being scaled and centered.
- The green menu is trimmed to `INSTALL NO RGI`, `INSTALL WITH RGI`, and `RESTORE ORIGINAL`; `STATUS` and `STORE LOGS + RESTORE` were removed, and after installation each boot's logs are saved to the SD card automatically. The version label is now `V3.7 Fix5`.

**V3.7Fix4**

- Fixed the display process recognizing only the B9 cluster layout; in the Q7 / Q5 FY Sport small view the map position now matches the safe area.
- Stronger secondary-display session lifecycle: stopUI is sent before disconnecting, and a new session is no longer affected by an old session's cleanup during fast reconnects or wired/wireless switches; if a stock command stalls, disconnect cleanup finishes in the background instead of blocking the stock flow.
- Retries for stream recovery, view-area changes, and firewall rules are now spaced and bounded; a failed firewall cleanup is retried in the background so no stale pass rule is left on the head unit.
- RGI threads (presentation, renderer connection, data bus) are rebuilt automatically after an unexpected exit; when the stock navigation service is replaced, the gate is reinstalled and stock navigation data is replayed; a renderer that fails at cold boot is retried after a cooldown instead of being abandoned for the whole boot.
- All log files are size-capped and no longer grow without limit on the RAM disk; timers use a clock that is not affected by system-time jumps.
- The stock backup records the firmware version: install and restore are refused on a firmware mismatch, and restore writes back only files this project actually changed; a VIN mismatch or an unreadable VIN only produces a warning.
- A failed rollback is now reported truthfully instead of as success; after an interrupted install, the next INSTALL or RESTORE recovers automatically; the pre-install check covers all runtime files; failure results give more accurate advice.
- Added a manual rescue restore mode; see section 7.1.
- The green-menu entry name is `MMI-Cockpit-Carplay` again, and the version label is `V3.7Fix4`.

**V3.7Fix3**

- Fixed race conditions between secondary-display connect, exit, and routing tasks: the secondary-display request and receiver-thread creation moved out of the metadata lock, and stopping waits for the thread handle to be published before cleanup; a failed stale routing task no longer clears a new session's pending state.
- When an active secondary-display session gets no new frames for a long time, a bounded number of key-frame requests are sent, and recovery is confirmed only by a new video frame.
- Supports both quick and non-quick firewall block rules; after a helper command times out, the actual rule state is checked instead of reporting an unconfirmed port as open.
- Temporary state is isolated from the sister project MMI Mirror; install and start check whether an external display is in use, and restore keeps the other project's state.
- Added safe-area detection for the Q7 cluster layout; the green-menu version label is now `V3.7Fix3`.

**V3.7Fix2**

- Fixed V3.7Fix1 failing at step 1 with a false `Another INSTALL / RESTORE is still running`; the operation lock now uses process identity, liveness, and child tasks, and correctly handles stale locks and PID reuse.
- Fixed directory locks and file renames failing on QNX where `/tmp` points to `/dev/shmem`; these locks and state files now live on the RAM disk.
- The package and scripts are checked before installing, and a failed check leaves the existing installation untouched; install, uninstall, and restore transactions persist their state and roll back more reliably.
- Changed the cleanup order when the secondary display stops: the video session is stopped and its threads exit first, then the stock audio stop and firewall cleanup run.
- `INSTALL WITH RGI` / `INSTALL NO RGI` now install and enable in one step, so only one reboot is needed; `START` was removed from the menu.
- An old version is detected and removed automatically (full, verified restore) before installing; switching install modes no longer needs a manual restore.
- Install, restore, and `STORE LOGS + RESTORE` progress is shown live instead of all at once at the end.
- The screen shows short English steps and errors only, with full detail in the SD-card log, and always ends with a `RESULT: SUCCESS / FAILED` block that states the head unit's current state and the next step.
- If enabling autostart or the final check fails, the stock configuration is restored automatically.

**V3.7Fix1**

- The green menu now offers `INSTALL WITH RGI` / `INSTALL NO RGI`; `START` keeps the chosen mode, and a failed install rolls back automatically.
- Fixed maneuver arrows and distance progress not updating when the maneuver list is valid but `maneuverCount` is 0; a missing junction type now falls back to the maneuver type.
- Fixed state publishing failing when a cross-filesystem rename in QNX `/tmp` shared memory fails; the Java side now rejects incomplete state snapshots.
- `INSTALL`, `START`, and `STATUS` now verify the HMI JAR against `BUILD_INFO`, so an updated JAR is no longer rejected by stale hard-coded checksums.
- The cluster map ETA overlay is turned off; the speed-limit sign, compass, 30 fps secondary display, GPU color conversion, startup video, and 350 / 1000 m arrow rules are kept.

**V3.7**

- Full RGI navigation-data integration; GPU color conversion for the secondary display; runtime watermark removed; all source code open under GPL-3.0.

---

## Public-release notice

Starting with V3.7, the project is fully open source: every feature, all source code, the installable runtime package, and related documentation are public and released under GPL-3.0.

> **Shared free of charge: this project is available for free on GitHub; do not pay for it.**

---

## Acknowledgements

Thanks to the following projects and authors, whose work this project builds on:

- [Luka](https://github.com/luka-dev)'s [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi): the foundation of the full RGI navigation-data integration and the Java HMI, and reference for MHI2Q CarPlay navigation guidance, HMI, and instrument-cluster interaction.
- [Allemon](https://github.com/Allemon)'s [mib2-carplay-rgi-altscreen](https://github.com/Allemon/mib2-carplay-rgi-altscreen): upstream implementation of the RGI maneuver renderer.
- [LIVI](https://github.com/f-io/LIVI): research reference for CarPlay main-display and instrument-cluster secondary-display protocol behavior.
- [jilleb](https://github.com/jilleb)'s [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox): upstream project for the SD-card toolchain, engineering menu, and scripts.
- Everyone who tested in a vehicle, reported issues, and shared logs.
