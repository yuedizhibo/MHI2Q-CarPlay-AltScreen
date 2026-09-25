# MHI2Q CarPlay AltScreen / MHI2Q CarPlay 第二屏

[中文](#中文) · [English](#english)

## 中文

本仓库提供 Audi MHI2Q / MIB2 High 的 CarPlay 第二屏 SD 卡文件。它尝试把 CarPlay 的独立视频流显示到 Virtual Cockpit。请先阅读 [SD 卡说明](SD_CARD_README.txt)，再进行车机操作。

### 当前状态

- 2026-09-19：V2 显示链路已在实车上点亮仪表，画面能随手机导航更新。
- 仓库根目录中的 SD 卡文件标注为 AUG22 / V2.1（2026-09-25）；这次更新尚未完成上车验证。V2 的实车结果不能视为 V2.1 的验证结果。
- 仅面向安装脚本能够核验的 AUG22 固件；其他固件不应强制安装。

### 下载与安装

1. 下载仓库 ZIP，解压后将仓库根目录的 SD 卡文件和目录复制到 FAT32 SD 卡根目录。`README.md`、主许可 `LICENSE` 和 `.gitattributes` 不参与车机安装，可不复制。卡根目录应直接看到 `metainfo2.txt`、`Toolbox`、`SD_CARD_README.txt` 和 `SHA256SUMS-SD.txt`。
2. 如果旧卡已有 `MMI-Cockpit-Carplay` 备份目录，换卡前完整保留并复制到新卡；恢复原车配置时需要它。
3. 已安装 MIB Toolbox：在 Toolbox 中运行 `Update Toolbox`。尚未安装：通过车机软件更新入口使用本卡的 `metainfo2.txt` 安装菜单和脚本。若车机不接受此更新包，不要强制刷入。
4. 断开 iPhone，依次执行 `INSTALL` → 完整重启 → `START` → 完整重启；然后连接 CarPlay、启动导航，查看仪表与 `STATUS` / 日志。
5. 需要恢复时执行 `RESTORE ORIGINAL`，并确保插入含原车备份的 SD 卡。

车机修改有黑屏或需要恢复的风险。请在车辆静止、供电稳定且已保存原车备份时操作。上车前请核验文件：在 SD 卡根目录运行 `sha256sum -c SHA256SUMS-SD.txt`。该清单只覆盖选定的 38 个运行文件，并非仓库根目录的全部文件。

### 许可与第三方文件

本项目由 [yuedizhibo](https://github.com/yuedizhibo) 和 [Lanye-z](https://github.com/Lanye-z) 共同开发。仓库根目录的 [PolyForm Noncommercial 1.0.0 许可](LICENSE)仅适用于相应权利人有权按该许可发布的原创部分：允许非商业使用、修改和分发；商业使用须另行取得相关权利人的许可。由于限制商用，本项目属于**源码可见的非商业许可**，不属于 OSI 定义的开源许可。

仓库中包含第三方文件，其原有授权不因仓库根目录的许可而改变。上游 MIB2 Toolbox 的 [MIT 许可](LICENSE.TOOLBOX-MIT)和镜像运行组件的[独立许可](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR)均须保留；使用或再分发时应分别遵守其条款。

研究与实现参考项目：

- [LIVI](https://github.com/f-io/LIVI)：CarPlay 主屏与仪表第二屏协议行为的研究参考。
- [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi)：MHI2Q 的 CarPlay 导航引导、HMI 与仪表交互参考。
- [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox)：SD 卡工具链、工程菜单及脚本的上游项目。
## English

This repository provides SD card files for a CarPlay secondary display on Audi MHI2Q / MIB2 High. The project attempts to show a separate CarPlay video stream on the Virtual Cockpit. Read the [SD card instructions](SD_CARD_README.txt) before changing the head unit.

### Status

- 2026-09-19: the V2 display path lit the Virtual Cockpit in a vehicle test and updated with phone navigation.
- The SD card files at the repository root are labeled AUG22 / V2.1 (2026-09-25). This update has **not** been tested in a vehicle. The V2 result does not validate V2.1.
- Use only on AUG22 firmware accepted by the installer's checks. Do not force installation on other firmware.

### Download and installation

1. Download and extract the repository ZIP. Copy the SD card files and directories from the repository root to the root of a FAT32 SD card. `README.md`, the root `LICENSE`, and `.gitattributes` are not needed by the head unit and may be omitted. The card root should contain `metainfo2.txt`, `Toolbox`, `SD_CARD_README.txt`, and `SHA256SUMS-SD.txt`.
2. If the old card contains an `MMI-Cockpit-Carplay` backup directory, preserve it and copy it to the replacement card. Restoring stock configuration requires that backup.
3. If MIB Toolbox is installed, run `Update Toolbox`. Otherwise, use the head unit's software update menu and this card's `metainfo2.txt` to install the menu and scripts. Do not force an update that the head unit rejects.
4. Disconnect the iPhone, then run `INSTALL` → full reboot → `START` → full reboot. Connect CarPlay, start navigation, and check the cockpit plus `STATUS` / logs.
5. To restore the original state, run `RESTORE ORIGINAL` with the SD card containing the original backup inserted.

Changing head unit system files can cause a blank screen or require recovery. Work with the vehicle parked, stable power, and a saved stock backup. Before vehicle use, run `sha256sum -c SHA256SUMS-SD.txt` from the SD card root. The manifest covers 38 selected runtime files, not every file in the repository.

### Licensing, authors, and third-party files

This project is developed by [yuedizhibo](https://github.com/yuedizhibo) and [Lanye-z](https://github.com/Lanye-z). The repository-root [PolyForm Noncommercial 1.0.0 license](LICENSE) applies only to original material that the relevant rights holders can license under those terms. It permits noncommercial use, modification, and distribution; commercial use requires separate permission from the relevant rights holders. Because commercial use is restricted, this is **source-available noncommercial software**, not OSI-defined open source.

Third-party files retain their existing licenses. Preserve the upstream MIB2 Toolbox [MIT license](LICENSE.TOOLBOX-MIT) and the mirror runtime's [separate license](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR), and follow each license when using or redistributing those files.

Research and implementation references:

- [LIVI](https://github.com/f-io/LIVI): reference for CarPlay main and instrument-cluster secondary-display protocol behavior.
- [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi): reference for CarPlay route guidance, HMI integration, and cockpit interaction on MHI2Q.
- [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox): upstream SD card tooling, engineering menu, and scripts.
