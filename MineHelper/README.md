# Village in the Shade - 下矿助手（Mine Helper v1.0.0）



## 功能

- 让你能够在高层石头非常多的情况下也能保证最多敲15块就找到矿洞；
- `FirstHole=1`：配置支持，第一块石头就出洞，默认关闭，谨慎开启。

> 可能会出现双洞，但是两个洞都能正常下，不影响继续下矿

## 安装

1. 关闭游戏；
2. 将 `MineHelper_v1.0.0` 文件夹放入游戏根目录的 `Mods` 文件夹；
3. 按下方"启动方式"运行游戏。

> 只适配相同游戏版本（village.exe 18,108,416 字节）。游戏更新后本 Mod 可能失效。

## 卸载

直接删除 `MineHelper_v1.0.0` 文件夹即可。

## 启动方式

本 Mod 通过 `VillageModLoader.exe` 注入。推荐在 Steam 里设置启动项：

1. Steam 库 → 右键《Village in the Shade》→ 属性 → 启动选项；
2. 填入（路径换成你自己的游戏根目录）：

   ```
   "你的游戏根目录\Mods\VillageModLoader.exe" & %command%
   ```

3. 之后从 Steam 直接启动游戏即可。

也可手动：每次启动游戏前先双击 `Mods\VillageModLoader.exe`，再从 Steam 启动。

## 配置（minehelper.txt）

| 配置 | 说明 |
|------|------|
| `FirstHole` | 1 = 第一块石头出洞；0 = 默认 |

修改保存后立即生效（下一层起）。

## 文件

| 文件 | 说明 |
|------|------|
| `minehelper.dll` | Mod 本体 |
| `minehelper.txt` | 配置 |
| `minehelper.log` | 运行日志 |

## 警告

- 少数情况下可能出现双洞，不影响使用；
- 转载或分享本 Mod 时请注明出处。

---

作者：gloaming
