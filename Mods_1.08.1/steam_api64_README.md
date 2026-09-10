# steam_api64 桥接基座（Mods 加载器）

## 这是什么

游戏（Steam 版）必然加载 `steam_api64.dll`。本基座用同名 dll 接管这个
加载点，做四件事：

1. **Mods 加载器**：递归扫描游戏根目录 `Mods\`，`LoadLibrary` 所有 DLL，
   自动加载所有 mod；
2. **Steam 转发**：把游戏实际用到的 10 个 Steamworks 函数原样转发给
   `steam_api64_org.dll`（真 dll），Steam 功能完全不受影响；
3. **tick 分发**：每帧（`SteamAPI_RunCallbacks`）调用各 mod 导出的
   `mod_tick`；
4. **崩溃捕获**：未处理异常写 `mod_crash.log`。

安装本基座后，**所有 mod 都不需要启动器、不需要 Steam 启动项**，
直接从 Steam 启动游戏即可。

## 文件清单

| 文件 | 说明 |
|------|------|
| `steam_api64.dll` | 基座本体（本文件） |
| `steam_api64_org.dll` | 真 Steamworks dll（由原 `steam_api64.dll` 改名而来） |
| `steam_proxy.log` | 基座日志（自动生成） |
| `mod_crash.log` | 崩溃日志（发生未处理异常时自动生成） |

## 安装

1. 关闭游戏；

2. 备份游戏根目录原有的 `steam_api64.dll`，把它**改名**为
   `steam_api64_org.dll`（这就是真 dll）；

3. 把本基座的 `steam_api64.dll` 复制到游戏根目录；

4. 把各 mod 文件夹（如 `Mods\Teleport_v2.1.3\`）放入游戏根目录的
   `Mods` 文件夹；

5. 直接从 Steam 启动游戏。

   **如果你之前使用过我的 mod 安装了 VillageModLoader，**

   **请删除并同步清理 steam 启动项命令，**

   **本启动方式更安全更方便。**

## 其它 Mod 作者看

- 普通 mod：把 DLL 放进 `Mods\` 即可（`DllMain` 加载时运行）；可选导出：
  - `void mod_init(void)`：加载后调用一次，可建线程；
  - `void mod_tick(void)`：游戏主线程每帧调用。
- 想接管 steam_api64 代理的作者：把代理命名为 `steam_api64_proxy.dll`
  放到游戏根目录，本基座会自动链式转发给它（它再转发给
  `steam_api64_org.dll` 或更下一环），**不独占代理名，大家可共存**。

## 回滚 / 卸载基座

删除 `steam_api64.dll`，把 `steam_api64_org.dll` 改回 `steam_api64.dll`
即可完全恢复原版，Steam 功能不受影响。

## 常见问题

- 游戏打不开 / Steam 报错：确认 `steam_api64_org.dll` 存在且是原版改名，
  没有的话游戏无法初始化 Steamworks。
- mod 没生效：确认 mod 文件夹在 `Mods\` 下，且文件夹内有 dll；
  看 `steam_proxy.log` 是否列出该 mod（`loaded: xxx.dll (init=... tick=...)`）。

---

作者：gloaming。转载或分享时请注明出处。
