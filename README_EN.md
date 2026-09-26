# MIB2 Toolbox — CarPlay AltScreen V2.1

**English** | [简体中文](README.md)

This project is designed for the Audi **MHI2Q** platform and displays the **native CarPlay AltScreen / secondary navigation view** directly on the vehicle's **Virtual Cockpit**. The core display path has been verified in a vehicle. Read the [SD card instructions](SD_CARD_README.txt) before making changes to the head unit.

> [!NOTE]
> **Sister project: MMI Mirror**  
> If you want to mirror the **entire MMI center display** instead of using the native CarPlay secondary display, see:  
> **[MHI2Q-CarPlay-MMI-Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)**

> [!WARNING]
> **⚠️ A note before you start**
>
> The package currently published here is an **early public release based on a vehicle-validated display path**. The complete development version already contains additional features, but because freely shared test builds have previously been repackaged and resold without permission, not every completed feature will be released all at once.
>
> Mature features will be added gradually after cleanup, stability testing, and compatibility verification.
>
> This is not a demonstration build. The current CarPlay AltScreen functionality is already usable in a real vehicle.
>
> This project was originally developed around our own vehicles and day-to-day use cases. The current development and validation scope is focused on **MHI2Q / China-region (CN) firmware**. We currently **do not plan to actively adapt the project for the MHI2 platform or for US / ER and other regional firmware variants**. If your vehicle is outside the currently validated scope, do not assume compatibility and do not bypass the installer's checks to force installation.
>
> **Shared free of charge. Reselling is prohibited.**
>
> You are welcome to learn from, study, and discuss the project, but please do not repackage free testing and development work for profit.

> [!IMPORTANT]
> This project modifies system files on the head unit. Keep the SD card inserted and maintain stable power during installation, start, or recovery operations.  
> **After installation or recovery, fully reboot the head unit / HMI as instructed before judging the result.**
>
> Do not perform installation, update, recovery, or troubleshooting while driving.
>

---

## Vehicle demonstration

<img width="1920" height="1080" alt="CarPlay AltScreen on Virtual Cockpit" src="https://github.com/user-attachments/assets/f582d179-8c8e-41ac-882b-24d623813fca" />

---

## Currently included in the public release

- Native CarPlay AltScreen
- The main CarPlay display remains available and unaffected
- STATUS diagnostics
- Safe installation and recovery
- Protection against interrupted installation / recovery
- Restoration of stock configuration
- Logs and SD-card backups
- Core display path vehicle-validated on **China-region AUG22 firmware**

## Not yet included in the current public release

- Steering-wheel control for CarPlay map zoom
- More complete RGI navigation-data integration
- Dynamic Classic / Sport layout adaptation

These features will be added gradually to later public releases according to stability, compatibility, and cleanup progress. **Adaptation for the MHI2 platform and for US / ER or other regional firmware variants is currently outside the scope of this project.**

### Planned for future releases:

https://github.com/user-attachments/assets/b6506445-d6a5-4765-9d4f-db64be53ae44

https://github.com/user-attachments/assets/53cfcd21-63ea-4e7b-a1f7-68f02353de05

---

### Installation and testing

> [!IMPORTANT]
> **This repository is now an AltScreen overlay only. It no longer contains the complete MIB2 Toolbox installer.**
>
> - The **red software-update menu** is only for installing or repairing the upstream [jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox).
> - **This project itself cannot be installed directly through the red software-update menu.**
> - After the upstream Toolbox is working, load this project through **`MQBCoding → Update Toolbox`** in the green menu.
> - If `Update Toolbox` reports `Script not found` or `/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh` is missing, repair/reinstall the upstream Toolbox first.

### 1. Confirm that the upstream MIB2 Toolbox works

1. Check the head unit's firmware version. This project targets **AUG22** firmware accepted by the installer's checks. Stop if the version differs or cannot be confirmed; do not bypass the checks.
2. Park the vehicle and maintain stable power. Back up the current SD card and stock files, then prepare a writable **FAT32** SD card. If the old card contains `MMI-Cockpit-Carplay`, preserve that entire directory when changing cards because it contains stock backups needed for restoration.
3. If `Green Developer Menu → MQBCoding` already works and **`Update Toolbox` runs normally without a Script not found error**, skip directly to section 2.
4. If the upstream Toolbox is not installed, or the green menu exists but `Update Toolbox` is broken / missing its script, download and extract the latest complete **[jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox)** package. Copy the **contents** of that upstream package to the SD-card root without an extra ZIP-name folder.
5. Insert only that SD card in the head unit. Enter the red menu and select `Software updates/versions → Update → SD card → MQB Coding MIB2 Toolbox`. Wait for the update and all automatic reboots to finish; keep the card inserted and power stable.
6. After reboot, open `Green Developer Menu → MQBCoding` and confirm that **`Update Toolbox` runs normally**. If the upstream package is not recognized, check FAT32 format, root layout, and the upstream instructions. Stop if it is still rejected.

### 2. Merge this project overlay into the upstream Toolbox SD card

1. Keep the **complete upstream MIB2 Toolbox SD-card contents**. Do not delete its `metainfo2.txt`, `Toolbox/final/`, `Toolbox/GEM/mqb-main.esd`, `Toolbox/scripts/update_toolbox.sh`, or other upstream files.
2. Download and extract this repository. These are compiled vehicle overlay files; **you do not need to copy source code or build directories**. Merge this repository's **`Toolbox/` directory into the existing `Toolbox/` directory on the SD card**:
   - Replace same-name files with this project's versions.
   - Keep all upstream-only files.
   - When upgrading from an older project build, delete the old `logo.rgba` and `watermark.rgba` from `Toolbox/carplay_alt_screen/mirror_display/release/` on the card; the new binary does not use them.
   - **Do not wipe the upstream Toolbox first, and do not treat this repository as a standalone red-menu update package.**
3. You may also copy `SD_CARD_README.txt` and `SHA256SUMS-SD.txt` to the SD-card root. If changing cards, also preserve the complete `MMI-Cockpit-Carplay` stock-backup directory.
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
├─ SD_CARD_README.txt
└─ SHA256SUMS-SD.txt
```

5. Do not add an extra `SD卡/Toolbox/` or `MHI2Q-CarPlay-AltScreen/Toolbox/` directory level.
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

5. If you still see:

```text
Script not found:
/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh
```

the **upstream Toolbox base installation is still broken**. Do not continue with this project's `INSTALL`; return to section 1 and repair the upstream Toolbox first.

### 4. Install and start the secondary display

In the `MMI-Cockpit-Carplay` menu, follow this order and let each action finish before continuing:

1. **Disconnect the iPhone / CarPlay** so navigation video is not playing during installation.
2. Select `INSTALL`. Wait until it finishes. After `INSTALL=PASS` and `reboot_required=YES`, **fully reboot the head unit**. If it reports `FAIL`, record the message and stop.
3. After reboot, select `START`. Wait for `START=PASS` and `reboot_required=YES`, then **fully reboot the head unit again**.
4. After the second reboot, connect the iPhone, enter CarPlay, and start navigation. Check whether the Virtual Cockpit shows the secondary display and updates with navigation. Once valid secondary-display video arrives, the startup logo appears centered at 80% of its previous size for about two seconds. During operation, the original “Free and open source, resale prohibited” watermark from the sister project drifts and bounces within the visible video area at the reference program's frame-based rate. The startup logo is not the vehicle boot logo.

### 5. Check status and troubleshoot

- With CarPlay navigation running, open `STATUS`. `PHYSICAL_ROUTE_READY=SOFTWARE_CHAIN_COMPLETE` means the script observed the video decoding, display path, Context 80, and other required software conditions; you must still **visually confirm the image on the Virtual Cockpit**. `PHYSICAL_ROUTE_READY=NO` means the required conditions are not all present; check the missing items shown in the output.
- If the `MMI-Cockpit-Carplay` menu is missing, first confirm that the upstream green menu works, the SD-card directory layout is correct, and `MQBCoding → Update Toolbox` completed successfully.
- If `Update Toolbox` itself reports `Script not found`, that is an upstream Toolbox base-installation problem rather than an AltScreen installer problem. Repair the upstream Toolbox through the red software-update menu first.
- If the SD card is not detected, check FAT32, root layout, and read/write status. If `STATUS` is not ready, confirm that CarPlay is connected and navigation is producing video before collecting logs; do not repeatedly force `START`.
- `STORE LOGS + RESTORE` tries to collect diagnostics and **then immediately restores the stock configuration**. It is not a logs-only action. If you want to keep the AltScreen runtime installed and active, do not select it.

### 6. Restore the stock configuration

1. Insert the SD card that retains the `MMI-Cockpit-Carplay` stock-backup directory. Select `RESTORE ORIGINAL`, or `STORE LOGS + RESTORE` if you want to collect logs before restoring.
2. Wait for `RESTORE=PASS` and `reboot_required=YES`, then fully reboot the head unit. Restore removes this project's HMI JAR and restores the related stock configuration.
3. If installation or restore was interrupted, runtime operation remains disabled. Keep the original backup card, run `RESTORE ORIGINAL` again, confirm that restoration succeeds, and only then consider running `INSTALL` again. Do not run `START` while restoration is incomplete.

See the [SD card instructions](SD_CARD_README.txt) for additional runtime notes. Changing head-unit system files can cause a blank screen or require recovery.

### Licensing, authors, and third-party files

This project is developed by [yuedizhibo](https://github.com/yuedizhibo) and [Lanye-z](https://github.com/Lanye-z). The repository-root [PolyForm Noncommercial 1.0.0 license](LICENSE) applies only to original material that the relevant rights holders are entitled to publish under those terms: non-commercial use, modification, and redistribution are permitted, while commercial use requires separate permission from the relevant rights holders. This repository provides runtime binaries, installation scripts, and documentation; it does not publish the C/C++ source used to build the QNX binary. Because commercial use is restricted, the license is not open source under the OSI definition.

The runtime watermark pixels come from [Lanye-z’s MMI Mirror project](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror), retaining the original 196×32 dimensions and approximately 60% maximum opacity. Third-party files retain their existing licenses. Preserve the upstream MIB2 Toolbox [MIT license](LICENSE.TOOLBOX-MIT) and the mirror runtime's [separate license](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR).

Research and implementation references:

- [LIVI](https://github.com/f-io/LIVI): research reference for CarPlay main-display and instrument-cluster secondary-display protocol behavior.
- [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi): reference for MHI2Q CarPlay navigation guidance, HMI, and instrument-cluster interaction.
- [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox): upstream project for the SD-card toolchain, engineering menu, and scripts.

---

# Version status

Current recommended version:

~~~text
main
└── AUG22 / V2.1
    └── Vehicle validated on China-region firmware
~~~

The current public release prioritizes stable installation, normal use, and reliable recovery. Additional features will be introduced in stages.

---

# Public-release notice

The repository currently publishes the installable runtime package and related documentation. It does not mean that every feature from the complete development version has been released at once.

> **Shared free of charge. Reselling is prohibited.**
