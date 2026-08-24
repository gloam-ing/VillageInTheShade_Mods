# Village in the Shade 静谧田园 Mod 合集

《静谧田园 / Village in the Shade》个人 Mod 集合，基于
`VillageModLoader`（dll 注入框架）运行。

> 本仓库只发布编译好的二进制（dll / exe），**不公开源码**。
> 适用游戏版本：`1.06`（Steam build `24771274`，village.exe 18,108,416 字节）。
> 游戏更新后各 Mod 可能失效。

## Mod 列表

| Mod | 版本 | 功能 |
| --- | --- | --- |
| VillageModLoader | v1.0.0 | 注入框架：启动游戏时自动加载 `Mods\*\*.dll` |
| Teleport | v2.1.0 | 传送菜单（键盘 + 手柄两个版本），解锁记录 |
| CameraZoom | v1.1.0 | 相机三档缩放调节 |
| ObjectMove | v0.8.x | 移动物体（Alt+左键拿起 / 左键放下，Move It 式），开发中 |
| MineHelper | v1.0.0 | 下矿助手（层数 / 石头计数 / 极速下矿） |
| AutoPet | v0.2.0 | 每日自动抚摸家畜与宠物，好感倍率 |
| BirthdayReminder | v0.1.0 | 当天生日村民 + 喜爱礼物提醒 |
| HuntOneShot | v0.1.1 | 狩猎动物一击必杀 |
| TimeFreeze | v0.1.0 | 深夜时间冻结 |

## 安装

1. 关闭游戏；
2. 把要用的 Mod 文件夹（如 `Teleport`）放进游戏根目录的 `Mods` 文件夹；
   `VillageModLoader` 文件夹里的 `VillageModLoader_v1.0.0.exe` 也要放进
   `Mods` 目录；
3. 启动方式（二选一）：
   - **Steam 启动项（推荐）**：游戏属性 → 启动选项填入
     `"游戏根目录\Mods\VillageModLoader_v1.0.0.exe" & %command%`
     （把路径换成你自己的），之后直接从 Steam 启动；
   - **手动**：先双击 `VillageModLoader_v1.0.0.exe`（保持窗口运行），
     再从 Steam 启动游戏。

## 配置

各 Mod 根目录下的 `*.txt` 为配置/记录文件，一般支持热加载
（Teleport 键位、CameraZoom 倍率、MineHelper 开关等）。

## 使用提醒

- **过剧情时请勿使用传送 Mod**，严禁强行解锁未解锁的传送点；
  因此产生的 bug 或存档损坏恕不负责。
- 下矿助手与 CameraZoom 等会修改游戏内存，退出游戏后全部还原。
- 建议定期备份存档。

## 授权

见 [LICENSE](LICENSE)。转载或分享时请注明出处（作者 gloaming）。
