MMI Cockpit CarPlay 第二屏 SD 运行包（AUG22 / V2.1）

1. 将本文件所在的 SD卡 文件夹内全部内容复制到另一张 SD 卡的根目录。
   根目录应直接看到 metainfo2.txt、Toolbox、SD_CARD_README.txt 和 SHA256SUMS-SD.txt。
2. 如果原卡上已有 MMI-Cockpit-Carplay 的 state/backup/logs，请保留这些车机备份文件；
   换新卡之前应先复制原卡上的 MMI-Cockpit-Carplay 目录，否则 RESTORE 无法找到原备份。
3. 车上已经安装 MIB Toolbox：先在 Toolbox 菜单执行 Update Toolbox，更新脚本和工程菜单。
   车上尚未安装 MIB Toolbox：通过车机软件更新入口使用本卡的 metainfo2.txt 安装精简菜单与脚本。
4. 在 MMI-Cockpit-Carplay 菜单执行：断开 iPhone → INSTALL → 完整重启 → START →
   完整重启 → 连接 CarPlay → 开导航。也可通过 SSH 直接执行本卡 Toolbox/scripts 中
   的同名脚本。若固件/更新菜单不接受 SWDL 包，请勿强制刷入。
5. 仅支持脚本可核验的 AUG22 固件。Logo 在收到有效第二屏视频后出现约 2 秒；
   运行时显示“免费开源，禁止倒卖”动态文字水印，在可见区域内漂移并在边界折返。
   Logo 仅用于第二屏启动画面，不是整车开机 Logo。
6. 恢复使用 RESTORE ORIGINAL；将删除本项目的 HMI JAR，并恢复原车相关配置。
7. 运行标记保存在车机 /mnt/app/root/carplay-altscreen/state；冷启动不依赖 SD 卡。
   SD 卡保存原车备份和有上限的诊断日志；RESTORE ORIGINAL 仍需插入含原备份的 SD 卡。
   如安装或恢复中断，运行会保持关闭；重新执行 RESTORE ORIGINAL，再 INSTALL。
8. 临时运行文件只写在 /tmp 根目录，不在 /tmp 下建文件夹。
   配置替换时在 /mnt/system 同目录短暂暂存，以保证原子替换；下次安装或恢复会清理断电残留。
本卡只保留车机运行所需文件，不包含源码、编译目录和其他 Toolbox 工具附件。

本卡 AUG22 / V2.1 文件已完成实车验证。使用前请备份 SD 卡，并先用 SHA256SUMS-SD.txt 核验文件。
