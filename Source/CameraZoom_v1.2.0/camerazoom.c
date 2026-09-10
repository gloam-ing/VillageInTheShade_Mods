/* camerazoom.c - CameraZoom v1.2.0（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文 / 5 配置
 *       / 6 表定位与改写 / 7 工作线程 / 8 生命周期
 *
 * 功能：调整相机三档缩放倍率。
 *   原版 1.2 / 1.35 / 1.5（第 4 个值 2.0，仅前三档可配）
 *   默认目标 0.6 / 0.9 / 1.2（值越小镜头越远）
 *
 * 定位方式：不依赖固定 RVA，直接在 exe 镜像内扫描原版三连浮点
 * （1.2/1.35/1.5）作锚点，并校验紧随其后的第 4 个浮点在 1.5~3.0
 * 之间，命中即原地改写，因此不需要 addrsig 签名表。
 *
 * 配置（camera_zoom.txt，保存后重启生效）：
 *   Enabled = 1/0   Zoom1..Zoom3   DelayMs = 10000   RepeatMs = 0
 *
 * 编译（在 CameraZoom_v1.2.0 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o camerazoom.dll
 *     camerazoom.c ..\shared\addrsig.c ..\shared\modkit.c -luser32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "addrsig.h"
#include "modkit.h"

/* ==================================================================
 * 1 版本档案
 * ================================================================== */
/* 原版三档倍率（扫描锚点）+ 第 4 个值的验证范围 */
static const float ORIG_ZOOM[3] = { 1.2f, 1.35f, 1.5f };
#define ZOOM4_MIN 1.5f
#define ZOOM4_MAX 3.0f
#define ZOOM_TABLE_SPAN 0x3200000u   /* 镜像扫描范围：base .. base+span */

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    char          cfg_path[MAX_PATH];
    LogKit        log;
    /* 配置 */
    int           enabled;
    float         zoom[3];
    int           delay_ms;
    int           repeat_ms;
} CameraZoomContext;

static CameraZoomContext g_ctx;

static void log_init(void);
static void log_direct(const char* fmt, ...);
static void load_config(void);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "camera_zoom", TRUE);
}

static void log_direct(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_write(&g_ctx.log, "[camera_zoom] %s", buf);
}

/* ==================================================================
 * 4 上下文
 * ================================================================== */
static void ctx_init(HINSTANCE hinst)
{
    GetModuleFileNameA(hinst, g_ctx.self_dir, MAX_PATH);
    {
        char* slash = strrchr(g_ctx.self_dir, '\\');
        if (slash) *(slash + 1) = 0;
    }
    lstrcpynA(g_ctx.log_path, g_ctx.self_dir, MAX_PATH);
    lstrcatA(g_ctx.log_path, "camera_zoom.log");
    lstrcpynA(g_ctx.cfg_path, g_ctx.self_dir, MAX_PATH);
    lstrcatA(g_ctx.cfg_path, "camera_zoom.txt");
    g_ctx.enabled = 1;
    g_ctx.zoom[0] = 0.6f;
    g_ctx.zoom[1] = 0.9f;
    g_ctx.zoom[2] = 1.2f;
    g_ctx.delay_ms = 10000;
    g_ctx.repeat_ms = 0;
}

/* ==================================================================
 * 5 配置
 * ================================================================== */
static void trim(char* s)
{
    char* p = s;
    size_t n;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void load_config(void)
{
    FILE* f = fopen(g_ctx.cfg_path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        char key[64], val[192];
        if (!eq) continue;
        *eq = 0;
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = 0;
        strncpy(val, eq + 1, sizeof(val) - 1);
        val[sizeof(val) - 1] = 0;
        trim(key);
        trim(val);
        if (_stricmp(key, "Enabled") == 0) g_ctx.enabled = atoi(val) != 0;
        else if (_stricmp(key, "DelayMs") == 0) g_ctx.delay_ms = atoi(val);
        else if (_stricmp(key, "RepeatMs") == 0) g_ctx.repeat_ms = atoi(val);
        else if (_stricmp(key, "Zoom1") == 0) g_ctx.zoom[0] = (float)atof(val);
        else if (_stricmp(key, "Zoom2") == 0) g_ctx.zoom[1] = (float)atof(val);
        else if (_stricmp(key, "Zoom3") == 0) g_ctx.zoom[2] = (float)atof(val);
    }
    fclose(f);
}

/* ==================================================================
 * 6 表定位与改写
 * ================================================================== */
/* 在 exe 镜像提交区域内找原版三连倍率表，返回 RVA；找不到返回 0 */
static uintptr_t find_table(void)
{
    BYTE pat[12];
    const uintptr_t span = ZOOM_TABLE_SPAN;
    BYTE* addr;
    BYTE* end;

    memcpy(pat, ORIG_ZOOM, sizeof(pat));
    addr = (BYTE*)g_ctx.base;
    end = addr + span;
    while ((uintptr_t)addr < (uintptr_t)end) {
        MEMORY_BASIC_INFORMATION mbi;
        BYTE* p;
        BYTE* pend;

        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 12 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                            PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            p = (BYTE*)mbi.BaseAddress;
            pend = p + mbi.RegionSize;
            while (p + 16 <= pend) {
                size_t chunk = (size_t)(pend - p);
                BYTE buf[0x4000];
                size_t i;
                if (chunk > 0x4000) chunk = 0x4000;
                if (!mk_read(buf, p, chunk)) break;
                for (i = 0; i + 16 <= chunk; i++) {
                    if (memcmp(buf + i, pat, 12) == 0) {
                        float v4;
                        memcpy(&v4, buf + i + 12, 4);
                        if (v4 >= ZOOM4_MIN && v4 <= ZOOM4_MAX) {
                            uintptr_t rva = (uintptr_t)p + i - g_ctx.base;
                            log_direct("table found @ RVA %08X (v4=%.2f)",
                                       (unsigned)rva, v4);
                            return rva;
                        }
                    }
                }
                p += chunk;
            }
        }
        if (mbi.RegionSize == 0) break;
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 0;
}

/* 改写三档倍率；返回 1 成功 */
static int patch_table(uintptr_t rva)
{
    BYTE* t = (BYTE*)(g_ctx.base + rva);
    float cur[3], rb[3];

    if (!mk_read(cur, t, sizeof(cur))) return 0;
    if (memcmp(cur, ORIG_ZOOM, sizeof(cur)) != 0) {
        log_direct("skip: current %.2f/%.2f/%.2f (already patched?)",
                   cur[0], cur[1], cur[2]);
        return 1;
    }
    if (!mk_write(t, g_ctx.zoom, sizeof(g_ctx.zoom))) {
        log_direct("patch write failed @ %08X", (unsigned)rva);
        return 0;
    }
    if (mk_read(rb, t, sizeof(rb)) &&
        memcmp(rb, g_ctx.zoom, sizeof(rb)) == 0) {
        log_direct("PATCHED -> %.2f / %.2f / %.2f (readback ok)",
                   g_ctx.zoom[0], g_ctx.zoom[1], g_ctx.zoom[2]);
        return 1;
    }
    log_direct("PATCH write-back mismatch");
    return 0;
}

/* ==================================================================
 * 7 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    int found = 0;

    (void)param;
    Sleep(300);
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    log_direct("CameraZoom v1.2.0 loaded; base=%p", (void*)g_ctx.base);
    if (!g_ctx.base) return 0;

    load_config();
    log_direct("config: enabled=%d delay=%d repeat=%d zoom=%.2f/%.2f/%.2f",
               g_ctx.enabled, g_ctx.delay_ms, g_ctx.repeat_ms,
               g_ctx.zoom[0], g_ctx.zoom[1], g_ctx.zoom[2]);
    if (!g_ctx.enabled) return 0;
    if (g_ctx.delay_ms < 0) g_ctx.delay_ms = 0;

    for (;;) {
        uintptr_t rva;
        if (g_ctx.delay_ms > 0) {
            Sleep(g_ctx.delay_ms);
            g_ctx.delay_ms = 0;
        }
        rva = find_table();
        if (rva) {
            if (patch_table(rva)) found = 1;
        } else {
            log_direct("table not found (game data not loaded yet?)");
        }
        if (found && g_ctx.repeat_ms <= 0) break;
        if (g_ctx.repeat_ms > 0) Sleep(g_ctx.repeat_ms);
    }
    log_direct("done");
    return 0;
}

/* ==================================================================
 * 8 生命周期
 * ================================================================== */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE hw;
        DisableThreadLibraryCalls(hinst);
        ctx_init(hinst);
        log_init();
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    }
    return TRUE;
}
