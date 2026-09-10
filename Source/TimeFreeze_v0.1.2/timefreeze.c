/* timefreeze.c - TimeFreeze v0.1.2（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文 / 5 时间字段
 *       / 6 提示窗 / 7 工作线程 / 8 生命周期
 *
 * 功能：F5 切换主世界时间冻结；冻结时右下角常驻 "TimeFreeze ON"。
 * 只冻结主世界时钟，不影响角色、动画、物理、脚本等待、显式跳时。
 *
 * 原理（不改任何游戏指令）：
 *   [[village.exe+clock]+0x208]+0x3268（QWORD 累计秒）
 *   [[village.exe+clock]+0x208]+0x3270（QWORD 时钟）
 *   worker 每 100ms 读缓存；冻结时把两个字段写回上次缓存值。
 *   地址由 addrsig 运行时解析，MISS 时 mod 停用并写日志。
 *
 * 编译（在 TimeFreeze_v0.1.2 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o timefreeze.dll
 *     timefreeze.c ..\shared\addrsig.c ..\shared\modkit.c -lgdi32 -luser32
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
#define TIME_A_OFF      0x3268   /* QoL 使用的时钟字段（QWORD 秒数） */
#define TIME_B_OFF      0x3270   /* CT 表使用的时钟字段（QWORD） */
#define NIGHT_END_HOUR  6        /* 深夜 = hour 0-5 */
#define DISPLAY_OFFSET  7        /* hour = (b/3600 + 7) % 24 */

#define SIG_CLOCK  "clock_0x10CD990"

static const AddrSig kSigs[] = {
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_CLOCK, 0, 0, 0 },
};

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    uintptr_t     clock;        /* addrsig 解析的时钟全局地址 */
    volatile LONG paused;
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    LogKit        log;
    GameProfile   profile;
    HWND          toast_hwnd;
    WCHAR         toast_text[32];
} TimeFreezeContext;

static TimeFreezeContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);
static void show_toast_hold(const WCHAR* text);
static void show_toast_1s(const WCHAR* text);
static void hide_toast(void);
static uintptr_t get_clock_base(void);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "tf", TRUE);
}

static void log_close(void)
{
    logkit_close(&g_ctx.log);
}

static void log_direct(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_write(&g_ctx.log, "%s", buf);
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
    lstrcatA(g_ctx.log_path, "timefreeze.log");
}

/* addrsig 运行时解析：时钟全局 */
static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "tf";
    g_ctx.profile.base = g_ctx.base;
    g_ctx.profile.sigs = kSigs;
    g_ctx.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_ctx.profile.res = s_res;
    g_ctx.profile.entries = s_entries;
    g_ctx.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_ctx.profile);
    g_ctx.clock = profile_addr(&g_ctx.profile, SIG_CLOCK);
    return g_ctx.clock != 0;
}

/* ==================================================================
 * 5 时间字段读写
 * ================================================================== */
static int read_ptr(uintptr_t addr, uintptr_t* out)
{
    uintptr_t v = 0;
    if (!mk_read(&v, (const void*)addr, 8)) return 0;
    if (v < 0x10000) return 0;
    *out = v;
    return 1;
}

static uintptr_t get_clock_base(void)
{
    uintptr_t g = 0, d = 0;
    if (!g_ctx.clock) return 0;
    if (!read_ptr(g_ctx.clock, &g)) return 0;
    if (!read_ptr(g + 0x208, &d)) return 0;
    return d;
}

static int read_time(uintptr_t clock, DWORD off, uint64_t* out)
{
    if (!clock) return 0;
    return mk_read(out, (const void*)(clock + off), 8);
}

static void write_time(uintptr_t clock, DWORD off, uint64_t v)
{
    if (!clock) return;
    mk_write((void*)(clock + off), &v, 8);
}

/* ==================================================================
 * 6 提示窗（常驻 / 1 秒 / 隐藏）
 * ================================================================== */
static LRESULT CALLBACK toast_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW(32, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, L"Microsoft YaHei");
        HFONT old = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, RGB(255, 0, 0));
        RECT tr = { 10, 6, 320, 80 };
        DrawTextW(hdc, g_ctx.toast_text, -1, &tr, DT_NOPREFIX);
        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        KillTimer(hwnd, 1);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void set_toast_text(const WCHAR* text)
{
    lstrcpynW(g_ctx.toast_text, text, 31);
    g_ctx.toast_text[31] = 0;
}

static void show_toast_hold(const WCHAR* text)
{
    set_toast_text(text);
    if (g_ctx.toast_hwnd) {
        KillTimer(g_ctx.toast_hwnd, 1);
        ShowWindow(g_ctx.toast_hwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(g_ctx.toast_hwnd, NULL, TRUE);
    }
}

static void show_toast_1s(const WCHAR* text)
{
    set_toast_text(text);
    if (g_ctx.toast_hwnd) {
        KillTimer(g_ctx.toast_hwnd, 1);
        ShowWindow(g_ctx.toast_hwnd, SW_SHOWNOACTIVATE);
        SetTimer(g_ctx.toast_hwnd, 1, 1000, NULL);
        InvalidateRect(g_ctx.toast_hwnd, NULL, TRUE);
    }
}

static void hide_toast(void)
{
    if (g_ctx.toast_hwnd) ShowWindow(g_ctx.toast_hwnd, SW_HIDE);
}

static DWORD WINAPI toast_thread(LPVOID param)
{
    WNDCLASSA wc;
    int sx, sy, tx, ty;
    HWND game;
    RECT wr;
    MSG msg;

    (void)param;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = toast_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "TimeFreezeToast";
    if (!RegisterClassA(&wc)) return 0;
    sx = GetSystemMetrics(SM_CXSCREEN);
    sy = GetSystemMetrics(SM_CYSCREEN);
    tx = sx - 340;
    ty = sy - 84;
    game = FindWindowA("Honogurashinoniwa", NULL);
    if (game && GetWindowRect(game, &wr)) {
        tx = wr.right - 340;
        ty = wr.bottom - 84;
    }
    g_ctx.toast_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "TimeFreezeToast", "TF", WS_POPUP, tx, ty, 332, 76,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_ctx.toast_hwnd) return 0;
    SetLayeredWindowAttributes(g_ctx.toast_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ==================================================================
 * 7 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    int f5_prev = 0;
    uint64_t last_a = 0, last_b = 0;
    int has_last = 0;

    (void)param;
    Sleep(600);
    log_init();
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    load_profile();
    log_direct("[tf] TimeFreeze v0.1.2 loaded; base=%p (CT method)",
               (void*)g_ctx.base);
    if (!g_ctx.clock) {
        log_direct("[tf] FATAL: clock sig MISS, mod disabled");
        log_close();
        return 0;
    }

    while (1) {
        int f5;
        uintptr_t clock;
        uint64_t a = 0, b = 0;

        Sleep(100);
        f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5 && !f5_prev) {
            /* 已冻结时：直接关闭，不做时段判定 */
            if (g_ctx.paused) {
                g_ctx.paused = 0;
                hide_toast();
                log_direct("[tf] F5 TimeFreeze=0 (resumed)");
                f5_prev = f5;
                continue;
            }
            /* 未冻结时：仅深夜允许开启 */
            clock = get_clock_base();
            if (clock && read_time(clock, TIME_B_OFF, &b)) {
                int hour = (int)((b / 3600 + DISPLAY_OFFSET) % 24);
                if (hour < NIGHT_END_HOUR) {
                    g_ctx.paused = 1;
                    show_toast_hold(L"TimeFreeze ON");
                    has_last = 0;   /* 重新缓存基准值 */
                    log_direct("[tf] F5 TimeFreeze=1 (hour=%d b=%llu)",
                               hour, (unsigned long long)b);
                } else {
                    show_toast_1s(L"\u5f53\u524d\u65f6\u6bb5\u4e0d\u53ef\u7528"
                                  L"\uff0c\u8bf7\u6df1\u591c\u518d\u8bd5");
                    log_direct("[tf] F5 ignored: hour=%d b=%llu (night only)",
                               hour, (unsigned long long)b);
                }
            } else {
                show_toast_1s(L"\u5f53\u524d\u65f6\u6bb5\u4e0d\u53ef\u7528"
                              L"\uff0c\u8bf7\u6df1\u591c\u518d\u8bd5");
                log_direct("[tf] F5 ignored: hour=-1 b=%llu (night only)",
                           (unsigned long long)b);
            }
        }
        f5_prev = f5;

        clock = get_clock_base();
        if (!clock) continue;
        if (!read_time(clock, TIME_A_OFF, &a)) continue;
        read_time(clock, TIME_B_OFF, &b);

        if (!has_last) {
            last_a = a;
            last_b = b;
            has_last = 1;
            continue;
        }
        if (g_ctx.paused) {
            write_time(clock, TIME_A_OFF, last_a);
            write_time(clock, TIME_B_OFF, last_b);
        } else {
            last_a = a;
            last_b = b;
        }
    }
    return 0;
}

/* ==================================================================
 * 8 生命周期
 * ================================================================== */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE ht, hw;
        DisableThreadLibraryCalls(hinst);
        SetProcessDPIAware();   /* 统一物理像素坐标，HUD 定位准确 */
        ctx_init(hinst);
        ht = CreateThread(NULL, 0, toast_thread, NULL, 0, NULL);
        if (ht) CloseHandle(ht);
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    }
    return TRUE;
}
