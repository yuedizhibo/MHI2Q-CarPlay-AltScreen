# MIB2 Toolbox — CarPlay AltScreen V2.1

**English** | [简体中文](README.md)

This project is designed for the Audi **MHI2Q** platform and displays the **native CarPlay AltScreen / secondary navigation view** directly on the vehicle's **Virtual Cockpit**. This repository currently provides a vehicle-validated installation package and usage instructions. Actual compatibility is determined by the installer's firmware checks. Read the [Instructions](#installation-and-testing) before making changes to the head unit.

> [!NOTE]
> **Sister project: MMI Mirror**  
> If you want to mirror the **entire MMI center display** instead of using the native CarPlay secondary display, see:  
> **[MHI2Q-CarPlay-MMI-Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)**

> [!WARNING]
> **⚠️ A note before you start**
>
> The package currently published here is a **vehicle-validated early public release**. The complete development version already contains additional features, but because freely shared test builds have previously been repackaged and resold without permission, not every completed feature will be released all at once.
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

---

## Vehicle demonstration

<img width="1920" height="1080" alt="CarPlay AltScreen on Virtual Cockpit" src="https://github.com/user-attachments/assets/f582d179-8c8e-41ac-882b-24d623813fca" />

### In-vehicle operation

https://github.com/user-attachments/assets/b6506445-d6a5-4765-9d4f-db64be53ae44

### Additional demonstration

https://github.com/user-attachments/assets/53cfcd21-63ea-4e7b-a1f7-68f02353de05

---

## Currently included in the public release

- Native CarPlay AltScreen
- The main CarPlay display remains available and unaffected
- Automatic operation after cold boot
- STATUS diagnostics
- Safe installation and recovery
- Protection against interrupted installation / recovery
- Restoration of stock configuration
- Logs and SD-card backups
- Currently vehicle-validated on **China-region AUG22 firmware**

## Not yet included in the current public release

- Steering-wheel control for CarPlay map zoom
- More complete RGI navigation-data integration
- Dynamic Classic / Sport layout adaptation

These features will be added gradually to later public releases according to stability, compatibility, and cleanup progress. **Adaptation for the MHI2 platform and for US / ER or other regional firmware variants is currently outside the scope of this project.**

---

## Installation and testing

### 1. Prepare the SD card

1. Check the firmware version on the head unit first. This package is for **AUG22** firmware that the installer can verify. Stop if the version is different, uncertain, or the head unit rejects the update package; do not force installation. The repository root contains vehicle-validated **AUG22 / V2.1** files.
2. Park the vehicle and maintain stable power. Back up the SD card in use and the stock files. Prepare a working, writable **FAT32** SD card.
3. Download and extract the repository ZIP. **Copy the contents of the repository root directly to the SD card root**; do not add an enclosing repository-name folder. The card root should directly contain `metainfo2.txt`, `Toolbox`, `SD_CARD_README.txt`, and `SHA256SUMS-SD.txt`. `README.md`, `README_EN.md`, the root `LICENSE`, and `.gitattributes` are not needed for installation.
4. If the old card has an `MMI-Cockpit-Carplay` directory, copy that entire directory to the replacement card. It contains stock backups and diagnostic material. A later restore requires the **card with the stock backup**, not a newly copied blank card.
5. After copying, run `sha256sum -c SHA256SUMS-SD.txt` from the card root using Git Bash, Linux, or another environment with `sha256sum`; confirm that listed files report `OK`.

### 2. Install or update MIB Toolbox

1. **MIB Toolbox already installed:** insert this card and run `Update Toolbox` from the Toolbox menu to refresh the engineering menu and scripts. Confirm that the `MMI-Cockpit-Carplay` menu appears.
2. **No MIB Toolbox installed:** use the head unit's **software update** entry to select the package on this card (with `metainfo2.txt` at the card root). Install the supplied menu and scripts, then open the Toolbox engineering menu and confirm `MMI-Cockpit-Carplay` appears.
3. If software update cannot read the card or rejects the package, check FAT32 formatting and the root layout. If it still rejects the package, stop; do not bypass the head unit's or installer's compatibility checks.

### 3. Install and start the secondary display

In the `MMI-Cockpit-Carplay` menu, follow this order and let each action finish before continuing:

1. **Disconnect the iPhone / CarPlay** so navigation video is not playing during installation.
2. Select `INSTALL`. Wait until it finishes. After `INSTALL=PASS` and `reboot_required=YES`, **fully reboot the head unit**. If it reports `FAIL`, record the message and stop.
3. After reboot, select `START`. Wait for `START=PASS` and `reboot_required=YES`, then **fully reboot the head unit again**.
4. After the second reboot, connect the iPhone, enter CarPlay, and start navigation. Check whether the Virtual Cockpit shows the secondary display and updates with navigation. Once valid secondary-display video arrives, the startup logo appears for about two seconds; it is not the vehicle boot logo.

### 4. Check status and troubleshoot

- With CarPlay navigation running, open `STATUS`. `PHYSICAL_ROUTE_READY=SOFTWARE_CHAIN_COMPLETE` means the script observed the required software conditions; you must still **visually confirm the image on the Virtual Cockpit**.
- If the menu is missing, check that `Update Toolbox` or software update completed. If the SD card is not detected, check FAT32, the root layout, and whether it is writable.
- `STORE LOGS + RESTORE` tries to collect diagnostics and **then immediately restores the stock configuration**. It is not a logs-only action.

### 5. Restore the stock configuration

1. Insert the SD card that retains the `MMI-Cockpit-Carplay` stock-backup directory. Select `RESTORE ORIGINAL`, or `STORE LOGS + RESTORE` if you want to collect logs before restoring.
2. Wait for `RESTORE=PASS` and `reboot_required=YES`, then fully reboot the head unit. Restore removes this project's HMI JAR and restores the related stock configuration.
3. If installation or restore was interrupted, keep the original backup card, run `RESTORE ORIGINAL` again, confirm that it succeeds, and only then consider running `INSTALL` again.

See the [SD card instructions](SD_CARD_README.txt) for additional runtime notes. Changing head-unit system files can cause a blank screen or require recovery.

### Licensing, authors, and third-party files

This project is developed by [yuedizhibo](https://github.com/yuedizhibo) and [Lanye-z](https://github.com/Lanye-z). The repository-root [PolyForm Noncommercial 1.0.0 license](LICENSE) applies only to original material that the relevant rights holders can license under those terms. Commercial use requires separate permission.

Third-party files retain their existing licenses. Preserve the upstream MIB2 Toolbox [MIT license](LICENSE.TOOLBOX-MIT) and the mirror runtime's [separate license](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR).

Research and implementation references:

- [LIVI](https://github.com/f-io/LIVI)
- [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi)
- [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox)

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
