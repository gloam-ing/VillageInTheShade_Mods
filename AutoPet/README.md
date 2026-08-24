# 自动抚摸（AutoPet v0.2.0）

> [English](README_EN.md)

## 功能

- 每天自动抚摸**所有**家畜与宠物（鸡、奶牛、绵羊、鸭、狗、马、牛鬼等）；
- 直接调用游戏原生好感结算逻辑，与手动抚摸效果一致；
- 每只动物每天只抚摸一次，新的一天自动重置；
- 好感倍率可配置：修改 `autopet.txt` 中的 `GainMultiplier`（默认 1），
  保存后实时生效，无需重启游戏。
- 不修改任何游戏文件与指令字节，仅读写数据并调用原生函数。

## 安装

1. 关闭游戏；
2. 将 `AutoPet_v0.2.0` 文件夹放入游戏根目录的 `Mods` 文件夹；
3. 通过 `VillageModLoader.exe` 启动游戏（设为 Steam 启动项）。

> 只适配相同游戏版本（village.exe 18,108,416 字节）。游戏更新后本 Mod 可能失效。

## 卸载

删除 `Mods\AutoPet_v0.2.0` 文件夹即可。

## 日志

`Mods\AutoPet_v0.2.0\autopet.log` 记录每次自动抚摸的动物唯一 ID 与好感变化。

## 配置

`autopet.txt`（与 DLL 同目录）：

```ini
; AutoPet configuration (reloads automatically when changed)
; GainMultiplier: affinity gain multiplier for auto-petting, default 1
GainMultiplier=1
```

作者：gloaming。转载或分享时请注明出处。
