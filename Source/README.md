# Village in the Shade — Mods（1.08.1 归档）

> **状态：已停止维护（归档）**
> 本项目最后对齐的游戏版本是 **1.08.1**（`village.exe` 18,105,864 字节，buildid 24969282）。
> 游戏的 1.09 及以后版本**未适配、未验证**。
> 欢迎其他大佬接手、移植或改写。本仓库仅作为源码归档与参考保留。

---

## 一、内容

10 个 mod 的完整 C 源码 + 一个共享工具层。每个 mod 都是**单文件实现**，只依赖 `shared/`。

|     mod      |                     源码                     |
| :----------: | :------------------------------------------: |
|   自动钓鱼   |         `AutoFish_v0.1.5/autofish.c`         |
|  机器自动化  |         `Automate_v0.5.4/automate.c`         |
|   自动抚摸   |          `AutoPet_v0.2.5/autopet.c`          |
|   生日提醒   | `BirthdayReminder_v0.1.2/birthdayreminder.c` |
|   相机缩放   |       `CameraZoom_v1.2.0/camerazoom.c`       |
| 狩猎一击必杀 |      `HuntOneShot_v0.2.0/huntoneshot.c`      |
|   矿洞助手   |       `MineHelper_v1.1.1/minehelper.c`       |
| 7x11自助商店 |   `SelfServiceStore_v0.1.8/selfservice.c`    |
|   地图传送   |         `Teleport_v2.3.0/teleport.c`         |
|   时间冻结   |       `TimeFreeze_v0.1.2/timefreeze.c`       |

|                            共享层                            |        `shared/`        |
| :----------------------------------------------------------: | :---------------------: |
|                      运行时签名地址解析                      | `addrsig.c / addrsig.h` |
| 日志 `LogKit` / 内存 `mk_*` / Hook 事务 `HookTxn` / 版本档案 `GameProfile` |  `modkit.c / modkit.h`  |

## 二、编译

需要 Zig（测试版本 0.14.1）。统一模板：

```bat
zig cc -shared -O2 -fms-extensions -I..\shared -o <Mod>.dll <mod>.c ^
    ..\shared\addrsig.c ..\shared\modkit.c [-lgdi32] -luser32
```

- 带窗口 / HUD / toast 的 mod 需要 `-lgdi32`：`AutoFish`、`TimeFreeze`、`BirthdayReminder`、
  `MineHelper`、`AutoPet`。

## 三、免责声明

- 仅供单机游戏使用，不分发游戏本体或任何游戏资源；请自行备份存档。
- 游戏版本更新后可能导致 mod 静默停用（签名 MISS 即降级），这是预期行为，不是崩溃。



作者：gloaming。转载或分享时请注明出处。
