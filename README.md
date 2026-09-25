# MIB2 Toolbox — CarPlay AltScreen V2.1

[English](README_EN.md) | **简体中文**

本项目面向 Audi **MHI2Q** 平台，用于将 **CarPlay 原生 AltScreen / 第二屏导航画面**直接显示至车辆的 **Virtual Cockpit**。当前仓库提供经过实车验证的安装包与使用说明；具体兼容性以安装脚本的固件核验结果为准。操作前请先阅读 [SD 卡说明](SD_CARD_README.txt)。

> [!NOTE]
> **姊妹项目：MMI Mirror**  
> 如果你希望显示的是 **MMI 中控完整画面镜像**，而不是 CarPlay 原生第二屏，请前往：  
> **[MHI2Q-CarPlay-MMI-Mirror](https://github.com/Lanye-z/MHI2Q-CarPlay-MMI-Mirror)**

> [!WARNING]
> **⚠️ 写在前面**
>
> 当前仓库提供的是经过实车验证的 **先行公开版本**。完整开发版本已经包含更多功能，但考虑到此前免费测试成果曾被未经允许包装和倒卖，我们不会在第一次公开时一次性发布全部功能。
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
- 当前 **中国区 AUG22 固件**已完成实车验证

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

#### 1. 准备 SD 卡

1. 先在车机信息页确认固件版本。此包面向安装脚本能够核验的 **AUG22** 固件；若版本不符、无法确认，或车机拒绝更新包，就停止操作，不要强制刷入。当前根目录提供已完成实车验证的 **AUG22** 文件。
2. 车辆停稳，保持稳定供电。备份正在使用的 SD 卡及原车文件，并准备一张可正常读写的 **FAT32** SD 卡。
3. 下载仓库 ZIP 并解压，**将仓库根目录的内容直接复制到 SD 卡根目录**，不要再套一层仓库名或“SD卡”文件夹。卡根目录应直接看到 `metainfo2.txt`、`Toolbox`、`SD_CARD_README.txt`、`SHA256SUMS-SD.txt`。`README.md`、根目录 `LICENSE` 和 `.gitattributes` 不参与车机安装，可不复制。
4. 如果旧卡已有 `MMI-Cockpit-Carplay` 目录，换卡时把该目录完整复制到新卡；它含有原车备份及诊断资料。以后执行恢复时要插入**含原车备份**的卡，不能只用一张新复制的空白卡。
5. 复制完成后，在 SD 卡根目录用 Git Bash、Linux 或其他提供 `sha256sum` 的环境运行 `sha256sum -c SHA256SUMS-SD.txt`，确认清单中的文件均为 `OK`。清单仅覆盖选定的 38 个运行文件，不覆盖仓库全部文件。

#### 2. 安装或更新 MIB Toolbox

1. **车上已有 MIB Toolbox：**插入本卡，在 Toolbox 菜单执行 `Update Toolbox`，更新工程菜单和脚本。更新完成后确认能看到 `MMI-Cockpit-Carplay` 菜单。
2. **车上没有 MIB Toolbox：**通过车机的**软件更新**入口选择本卡的更新包（卡根目录的 `metainfo2.txt`），安装随卡提供的菜单和脚本；完成后进入 Toolbox 工程菜单，确认有 `MMI-Cockpit-Carplay`。不同车机的入口名称可能不同，以车机实际显示为准。
3. 如果软件更新入口不识别卡或拒绝该包，检查 FAT32 格式及根目录结构；仍被拒绝就停止，不要绕过车机或安装脚本的兼容性检查。

#### 3. 安装第二屏并启动

在 `MMI-Cockpit-Carplay` 菜单中按以下顺序操作，每步完成后再进行下一步：

1. **断开 iPhone / CarPlay**，避免安装过程中正在输出导航视频。
2. 选择 `INSTALL`。等待执行结束；看到 `INSTALL=PASS` 且提示 `reboot_required=YES` 后，**完整重启车机**。若出现 `FAIL`，先记录提示并停止后续步骤。
3. 重启完成后选择 `START`。等待 `START=PASS` 和 `reboot_required=YES`，然后**再次完整重启车机**。若失败，不要直接跳到连接手机。
4. 第二次重启后连接 iPhone、进入 CarPlay 并启动导航。观察 Virtual Cockpit 是否出现第二屏画面且能随导航更新。收到有效第二屏视频后，启动 Logo 约显示 2 秒；它不是整车开机 Logo。

#### 4. 查看状态和排查

- 在 CarPlay 导航运行时打开 `STATUS`。`PHYSICAL_ROUTE_READY=SOFTWARE_CHAIN_COMPLETE` 表示脚本观察到视频解码、显示链路和 Context 80 等软件条件；仍须**亲眼确认仪表实际显示画面**。`PHYSICAL_ROUTE_READY=NO` 表示条件未齐，按输出中的缺失项检查。
- 若看不到菜单，先确认 `Update Toolbox` 或软件更新已完成。若 SD 卡找不到，核对 FAT32、卡根目录文件和读写状态。若 `STATUS` 不就绪，确认已连接 CarPlay 且导航正在输出，再记录状态及日志；不要反复强制执行 `START`。
- `STORE LOGS + RESTORE` 会尽力保存诊断日志，**随后立即恢复原车配置**；它不是只导出日志的按钮。需要保留第二屏运行时，不要选择它。

#### 5. 恢复原车

1. 插入保留了 `MMI-Cockpit-Carplay` 原车备份目录的 SD 卡，在菜单选择 `RESTORE ORIGINAL`；若要先收集日志再恢复，选择 `STORE LOGS + RESTORE`。
2. 等待 `RESTORE=PASS` 和 `reboot_required=YES`，然后完整重启车机。恢复会删除本项目的 HMI JAR，并还原相关原车配置。
3. 如果安装或恢复中断，运行会保持关闭。保留原备份卡，先重新执行 `RESTORE ORIGINAL`，确认恢复成功后再考虑重新 `INSTALL`；不要在恢复未完成时继续 `START`。

详细运行说明见 [SD 卡说明](SD_CARD_README.txt)。车机修改有黑屏或需要恢复的风险。
### 许可与第三方文件

本项目由 [yuedizhibo](https://github.com/yuedizhibo) 和 [Lanye-z](https://github.com/Lanye-z) 共同开发。仓库根目录的 [PolyForm Noncommercial 1.0.0 许可](LICENSE)仅适用于相应权利人有权按该许可发布的原创部分：允许非商业使用、修改和分发；商业使用须另行取得相关权利人的许可。由于限制商用，本项目属于**源码可见的非商业许可**，不属于 OSI 定义的开源许可。

仓库中包含第三方文件，其原有授权不因仓库根目录的许可而改变。上游 MIB2 Toolbox 的 [MIT 许可](LICENSE.TOOLBOX-MIT)和镜像运行组件的[独立许可](Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR)均须保留；使用或再分发时应分别遵守其条款。

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
