# MIB2 Toolbox — CarPlay AltScreen V2.1

[English](README_EN.md) | **简体中文**

本项目面向 Audi **MHI2Q** 平台，用于将 **CarPlay 原生 AltScreen / 第二屏导航画面**直接显示至车辆的 **Virtual Cockpit**。核心显示链路已完成实车验证。操作前请先阅读 [SD 卡说明](SD_CARD_README.txt)。

> [!NOTE]
> **姊妹项目：MMI Mirror**  
> 如果你希望显示的是 **MMI 中控完整画面镜像**，而不是 CarPlay 原生第二屏，请前往：  
> **[MHI2Q-CarPlay-MMI-Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)**

> [!WARNING]
> **⚠️ 写在前面**
>
> 当前仓库提供的是基于实车验证显示链路的 **先行公开版本**。完整开发版本已经包含更多功能，但考虑到此前免费测试成果曾被未经允许包装和倒卖，我们不会在第一次公开时一次性发布全部功能。
>
> 后续会在完成整理、稳定性验证和兼容性确认后，逐步将成熟功能更新到公开版本。
>
> 当前版本并非演示代码，现有 CarPlay AltScreen 第二屏功能已经可以正常实车使用。
>
> 本项目最初就是基于我们自己的车辆和日常使用需求进行开发，当前开发与测试范围以 **MHI2Q / 中国区（CN）固件**为主。我们目前**不会针对 MHI2 平台，也不会针对 US / ER 等其他地区固件主动开展适配**。如果你的车辆不在当前已验证范围内，请不要默认其具备兼容性，也不要绕过安装脚本的检查强制安装。
>
> **免费分享，禁止倒卖。**
>
> 可以学习、研究和交流，但请不要把免费的测试与开发成果重新包装后用于牟利。

> [!IMPORTANT]
> 本项目会修改车机系统文件。安装、启动或恢复过程中请保持 SD 卡连接和车机供电稳定。  
> **安装或恢复完成后，请按照页面中的步骤完整重启车机 / HMI，再判断结果。**
>
> 请勿在驾驶过程中进行安装、更新、恢复或故障处理。
>

---

## 实车效果

<img width="1920" height="1080" alt="CarPlay AltScreen on Virtual Cockpit" src="https://github.com/user-attachments/assets/f582d179-8c8e-41ac-882b-24d623813fca" />

---

## 当前公开版本已支持

- CarPlay 原生 AltScreen
- CarPlay 主屏正常使用，不受第二屏影响
- STATUS 状态诊断
- 安全安装与恢复
- 安装 / 恢复中断保护
- 原车配置恢复
- 日志与 SD 卡备份
- 核心显示链路已在 **中国区 AUG22 固件**完成实车验证

## 暂未包含在当前公开版本

- 方向盘滚轮控制 CarPlay 地图缩放
- 更完整的 RGI 导航信息联动
- Classic / Sport 动态布局适配

上述功能会根据稳定性、兼容性和整理进度，逐步更新到后续公开版本。**MHI2 平台以及 US / ER 等其他地区固件的适配目前不在本项目计划内。**

### 未来将引入：

https://github.com/user-attachments/assets/b6506445-d6a5-4765-9d4f-db64be53ae44

https://github.com/user-attachments/assets/53cfcd21-63ea-4e7b-a1f7-68f02353de05

---

### 安装与测试

> [!IMPORTANT]
> **本仓库现在只提供 CarPlay AltScreen 覆盖包，不再包含完整 MIB2 Toolbox 安装器。**
>
> - **红色软件更新菜单**只用于安装 / 修复上游 [jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox)。
> - **本项目本身不能直接通过红色菜单安装。**
> - 本项目应在上游 Toolbox 已正常安装后，通过绿色菜单里的 **`MQBCoding → Update Toolbox`** 写入菜单和脚本。
> - 如果绿色菜单里的 `Update Toolbox` 提示 `Script not found` 或缺少 `/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh`，先重新安装 / 修复上游 Toolbox，再继续本项目。

#### 1. 先确认上游 MIB2 Toolbox 是否正常

1. 先在车机信息页确认固件版本。本项目面向安装脚本能够核验的 **AUG22** 固件；版本不符或无法确认时停止操作，不要绕过检查。
2. 车辆停稳并保持稳定供电。备份正在使用的 SD 卡及原车文件，准备一张可正常读写的 **FAT32** SD 卡。如果旧卡已有 `MMI-Cockpit-Carplay` 目录，换卡时完整保留；其中有原车备份，日后恢复需要这张备份卡。
3. 如果车上已经能够正常进入 `Green Developer Menu → MQBCoding`，并且 **`Update Toolbox` 可以正常执行且不会提示 Script not found**，可直接跳到第 2 节。
4. 如果车上尚未安装上游 Toolbox，或者绿色菜单存在但 `Update Toolbox` 已损坏 / 提示脚本不存在，请前往 **[jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox)** 下载最新完整版本并解压。将**上游包内的文件和目录**复制到 SD 卡根目录，不要再套一层 ZIP 名称文件夹。
5. 车机中只插这一张 SD 卡，进入红色菜单，选择 `Software updates/versions → Update → SD 卡 → MQB Coding MIB2 Toolbox`。等待软件更新和自动重启全部完成，不要提前拔卡或断电。
6. 重启完成后，进入 `Green Developer Menu → MQBCoding`，确认 **`Update Toolbox` 能正常执行**。如果上游安装包不被识别，检查 FAT32、根目录结构和上游说明；仍被拒绝就停止，不要强制刷入。

#### 2. 将本项目覆盖包合并到上游 Toolbox

1. 在电脑上保留**上游完整 MIB2 Toolbox SD 卡内容**，不要删除其 `metainfo2.txt`、`Toolbox/final/`、`Toolbox/GEM/mqb-main.esd`、`Toolbox/scripts/update_toolbox.sh` 或其他上游文件。
2. 下载本仓库 ZIP 并解压。将本仓库的 **`Toolbox/` 目录合并到 SD 卡根目录已有的 `Toolbox/` 目录**：
   - 同名文件：使用本项目版本覆盖；
   - 上游独有文件：全部保留；
   - 从本项目旧版更新时，删除卡上 `Toolbox/carplay_alt_screen/mirror_display/release/` 内旧版的 `logo.rgba` 和 `watermark.rgba`；新版不再使用它们；
   - **不要清空后再复制，也不要把本项目当成一张独立的红菜单安装卡。**
3. 也可以同时复制 `SD_CARD_README.txt` 和 `SHA256SUMS-SD.txt` 到卡根目录。若更换 SD 卡，务必同时完整保留 `MMI-Cockpit-Carplay` 原车备份目录。
4. 正确结构应类似：

```text
SD 卡根目录/
├─ metainfo2.txt                 ← 上游 Toolbox 保留
├─ Toolbox/
│  ├─ final/                     ← 上游 Toolbox 保留
│  ├─ GEM/
│  │  ├─ mqb-main.esd            ← 上游 Toolbox 保留
│  │  └─ mqb-carplayAltScreen.esd← 本项目
│  ├─ scripts/
│  │  ├─ update_toolbox.sh       ← 上游 Toolbox 保留
│  │  └─ ...AltScreen scripts... ← 本项目
│  └─ carplay_alt_screen/        ← 本项目 payload
├─ SD_CARD_README.txt
└─ SHA256SUMS-SD.txt
```

5. 不要出现 `SD卡/Toolbox/`、`MHI2Q-CarPlay-AltScreen/Toolbox/` 之类的额外层级。
6. 在 SD 卡根目录用 Git Bash、Linux 或其他提供 `sha256sum` 的环境运行：

```sh
sha256sum -c SHA256SUMS-SD.txt
```

确认本项目清单中的文件均为 `OK`。该清单只校验本项目覆盖文件，不校验上游完整 Toolbox。

#### 3. 用绿色菜单加载本项目菜单和脚本

1. 将合并后的 SD 卡插回车机。
2. 打开 `TESTMODE → Green Developer Menu → MQBCoding`。
3. 执行 **`Update Toolbox`**。此步骤由上游 Toolbox 的更新脚本负责把 SD 卡中的 `Toolbox/scripts/` 和 `Toolbox/GEM/` 同步到车机。
4. 更新完成后退出并重新打开绿色菜单，进入：

```text
Customization
└─ MMI-Cockpit-Carplay
```

5. 如果此时仍提示：

```text
Script not found:
/eso/hmi/engdefs/scripts/mqb/update_toolbox.sh
```

说明车机上的**上游 Toolbox 基础安装本身仍未修复**。不要继续执行本项目的 `INSTALL`，应返回第 1 节重新修复上游 Toolbox。

#### 4. 安装第二屏并启动

在 `MMI-Cockpit-Carplay` 菜单中按以下顺序操作，每步完成后再进行下一步：

1. **断开 iPhone / CarPlay**，避免安装过程中正在输出导航视频。
2. 选择 `INSTALL`。等待执行结束；看到 `INSTALL=PASS` 且提示 `reboot_required=YES` 后，**完整重启车机**。若出现 `FAIL`，先记录提示并停止后续步骤。
3. 重启完成后选择 `START`。等待 `START=PASS` 和 `reboot_required=YES`，然后**再次完整重启车机**。若失败，不要直接跳到连接手机。
4. 第二次重启后连接 iPhone、进入 CarPlay 并启动导航。观察 Virtual Cockpit 是否出现第二屏画面且能随导航更新。收到有效第二屏视频后，启动 Logo 缩至原大小的 80%、居中显示约 2 秒；运行时参考项目原始的“免费开源，禁止倒卖”水印会按参考程序的逐帧节奏在可见视频区域内漂移、碰边折返。启动 Logo 不是整车开机 Logo。

#### 5. 查看状态和排查

- 在 CarPlay 导航运行时打开 `STATUS`。`PHYSICAL_ROUTE_READY=SOFTWARE_CHAIN_COMPLETE` 表示脚本观察到视频解码、显示链路和 Context 80 等软件条件；仍须**亲眼确认仪表实际显示画面**。`PHYSICAL_ROUTE_READY=NO` 表示条件未齐，按输出中的缺失项检查。
- 若完全看不到 `MMI-Cockpit-Carplay` 菜单，先确认上游绿菜单正常、SD 卡目录层级正确，并确认 `MQBCoding → Update Toolbox` 已成功执行。
- 若 `Update Toolbox` 本身报 `Script not found`，这是上游 Toolbox 基础安装问题，不是本项目第二屏安装脚本的问题；先用上游完整包通过红色软件更新菜单修复 Toolbox。
- 若 SD 卡找不到，核对 FAT32、卡根目录文件和读写状态。若 `STATUS` 不就绪，确认已连接 CarPlay 且导航正在输出，再记录状态及日志；不要反复强制执行 `START`。
- `STORE LOGS + RESTORE` 会尽力保存诊断日志，**随后立即恢复原车配置**；它不是只导出日志的按钮。需要保留第二屏运行时，不要选择它。

#### 6. 恢复原车

1. 插入保留了 `MMI-Cockpit-Carplay` 原车备份目录的 SD 卡，在菜单选择 `RESTORE ORIGINAL`；若要先收集日志再恢复，选择 `STORE LOGS + RESTORE`。
2. 等待 `RESTORE=PASS` 和 `reboot_required=YES`，然后完整重启车机。恢复会删除本项目的 HMI JAR，并还原相关原车配置。
3. 如果安装或恢复中断，运行会保持关闭。保留原备份卡，先重新执行 `RESTORE ORIGINAL`，确认恢复成功后再考虑重新 `INSTALL`；不要在恢复未完成时继续 `START`。

详细运行说明见 [SD 卡说明](SD_CARD_README.txt)。车机修改有黑屏或需要恢复的风险。

### 许可与第三方文件

本项目由 [yuedizhibo](https://github.com/yuedizhibo) 和 [Lanye-z](https://github.com/Lanye-z) 共同开发。仓库根目录的 [PolyForm Noncommercial 1.0.0 许可](LICENSE)仅适用于相应权利人有权按该许可发布的原创部分：允许非商业使用、修改和分发；商业使用须另行取得相关权利人的许可。由于限制商用，本项目属于**源码可见的非商业许可**，不属于 OSI 定义的开源许可。

运行时水印像素取自 [Lanye-z 的 MMI Mirror 项目](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)，保留原始 196×32 尺寸和最高约 60% 的不透明度。仓库中包含第三方文件，其原有授权不因仓库根目录的许可而改变。上游 MIB2 Toolbox 的 [MIT 许可](LICENSE.TOOLBOX-MIT)和镜像运行组件的[独立许可](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR)均须保留；使用或再分发时应分别遵守其条款。

研究与实现参考项目：

- [LIVI](https://github.com/f-io/LIVI)：CarPlay 主屏与仪表第二屏协议行为的研究参考。
- [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi)：MHI2Q 的 CarPlay 导航引导、HMI 与仪表交互参考。
- [MIB2 High Toolbox](https://github.com/jilleb/mib2-toolbox)：SD 卡工具链、工程菜单及脚本的上游项目。

---

# 版本状态

当前推荐版本：

~~~text
main
└── AUG22 / V2.1
    └── 中国区实车验证完成
~~~

当前公开版本以稳定、可安装、可恢复为优先目标；后续功能将分阶段更新。

---

# 开源说明

本项目当前公开的是可安装运行包与相关说明，并不代表完整开发版本的全部功能已经一次性公开。

> **免费分享，禁止倒卖。**
