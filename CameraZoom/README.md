# Village in the Shade - 相机视角 Mod（Camera Zoom Mod v1.1.0）

> [English](README_EN.md)

## 更新日志

### v1.1.0

- 改为 VillageModLoader 注入体系，不再使用 lz4 代理、不再修改原版文件；
- 三档相机倍率可在 `camera_zoom.txt` 中自定义。

## 功能

- 调整相机三档缩放倍率（近 / 中 / 远）；
- 原版倍率为 1.2 / 1.35 / 1.5，本 Mod 默认 0.6 / 0.9 / 1.2；
- 注意：这是"放大倍率"，**值越小镜头越远、看到的范围越大**；
  0.4 太远会导致远景渲染断层，出现断层时把数值调大。

## 安装

1. 关闭游戏；
2. 将整个 `CameraZoom_v1.1.0` 文件夹放入游戏根目录的 `Mods` 文件夹；
3. 按下方"启动方式"运行游戏。

> 只适配相同游戏版本（village.exe 18,108,416 字节）。游戏更新后本 Mod 可能失效。

## 卸载

直接删除游戏根目录 `Mods\CameraZoom_v1.1.0` 文件夹即可。

## 启动方式

本 Mod 通过 `VillageModLoader.exe` 注入。推荐在 Steam 里设置启动项，自动完成注入：

### 方式一：Steam 启动项（推荐）

1. Steam 库 → 右键《Village in the Shade》→ 属性 → 启动选项；
2. 填入下面内容（把路径换成你自己的游戏根目录）：

   ```
   "你的游戏根目录\Mods\VillageModLoader.exe" & %command%
   ```

   例如：

   ```
   "D:\Steam\steamapps\common\Village in the Shade\Mods\VillageModLoader.exe" & %command%
   ```

3. 之后直接从 Steam 启动游戏即可。

### 方式二：手动运行加载器

每次启动游戏前，先双击 `Mods\VillageModLoader.exe`（保持窗口运行），
再从 Steam 启动游戏。

> 忘记运行加载器时，Mod 不会生效。

## 文件

| 文件 | 说明 |
|------|------|
| `camerazoom.dll` | 相机 Mod 本体 |
| `camera_zoom.txt` | 倍率与补丁配置 |

## 警告

- 倍率过小可能导致远景渲染断层，请保持合理数值；
- 转载或分享本 Mod 时请注明出处。

---

作者：gloaming
