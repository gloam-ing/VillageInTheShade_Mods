# Village in the Shade 图鉴补成就工具（ZukanAchievementUnlock）

适用于 Steam 版 Village in the Shade v1.08.1。

## 作用

把存档里的图鉴条目补齐（共 2856 条物品记录），但**刻意保留"木材"一条不
点亮**。进游戏砍树获得木材，让游戏原生"新增图鉴条目"路径触发全收集判定，
从而补发对应的 Steam 成就。

直接改存档把图鉴全部点亮不会触发成就（成就是事件驱动，游戏读档时不扫描
存档补发），必须留一条靠正常游戏流程点亮。

## 使用

1. 双击 `ZukanAchievementUnlock.exe`（无需安装 Python 或任何依赖）。
2. 把存档文件拖进窗口后按回车。
   存档位置示例：
   `%APPDATA%\Nippon Ichi Software, Inc\Honogurashinoniwa\<SteamID>\save.001`
3. 工具自动备份原档（同目录 `.zukan_ach_时间戳`）→ 写入 → 重解析验证。
4. 进游戏读取该存档，砍树获得木材。
5. 图鉴木材条目点亮的同时，Steam 成就弹出。

## 文件说明

| 文件 | 用途 |
|---|---|
| `ZukanAchievementUnlock.exe` | 主程序，双击即用，所有依赖内置 |
| `zukan_achievement.py` | 源码（可选，需 Python + `pip install lz4`） |
| `zukan_achievement.bat` | 源码快捷启动（可选，会自动装 Python/lz4） |
| `item_ids.bin` | 物品 dataID 表（zlib，源码运行所需） |

## 注意

- 成就为账号级一次性；已解锁过该成就的账号不会重复触发。
- 存档自动备份，可随时用备份恢复；不同时使用其它改档工具。
- 未签名 EXE 首次运行可能被 SmartScreen/杀毒软件提示，属正常现象。
- 仅支持当前存档格式（v1.08.1）；游戏更新后存档格式变化则不保证可用。

## 原理与格式

- 存档 = `YKCMP_V1` 容器（LZ4 block）内嵌 NIS SER 树。
- 图鉴数据 = SER map `encyclopediaReleaseMap_`（key=8 字节物品 dataID，
  value=1 字节解锁标记）。
- SER 格式逆向参考：github.com/maosasagawa/village-in-the-shade-save-editor。

---

作者：gloaming。转载或分享时请注明出处。
