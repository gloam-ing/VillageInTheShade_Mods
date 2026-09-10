/* minehelper.c - MineHelper v1.1.1（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文 / 5 配置
 *       / 6 补丁 / 7 矿洞对象 / 8 HUD / 9 工作线程 / 10 生命周期
 *
 * 机制：矿洞层对象 = [[exe+clock]+0x208]+0x34E0，
 *   层数 = [obj+0x34]，剩余石头 = [obj+0x3C]。
 *   游戏出洞判定：剩余石头 == 0（砸完最后一块）。
 *   区间模式：每层随机 b，trigger = 石头总数 - b + 1，剩余降到
 *   trigger 时把立即数与剩余对齐，玩家砸下一块即归零。
 *   FirstHole 模式（F9 切换）：直接 NOP 掉“剩余 != 0 不出洞”的 jne。
 *   +0x38 检查的 jne 始终 NOP，避免该标志误阻止出洞。
 *   离开矿洞必须恢复原始字节，避免影响通用破坏/收割结算路径。
 *
 * 编译（在 MineHelper_v1.1.1 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o minehelper.dll
 *     minehelper.c ..\shared\addrsig.c ..\shared\modkit.c -lgdi32 -luser32
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
#define TIME_OFF     0x3270    /* 时钟字段（秒） */
#define LEVEL_OFF    0x34E0    /* 矿洞层对象链偏移 */
#define F_FLOOR      0x34      /* 层数 */
#define F_REMAIN     0x3C      /* 剩余石头 */
#define CMP_IMM_OFF  3         /* cmp dword [rdi+0x3C], imm8 的立即数偏移 */
#define MAX_REMAIN   300       /* 剩余石头合理上限 */

#define SIG_CLOCK  "clock_0x10CD990"
#define SIG_JNE1   "mine_jne1_0x19EE16"
#define SIG_JNE2   "mine_jne2_0x19EE20"
#define SIG_CMP    "mine_cmp_0x19EE12"

static const AddrSig kSigs[] = {
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
    { SIG_JNE1, ADDRSIG_FUNC,
      "06ba01000000488bceff1090837f3c000f8500000000807f38000f853f030000",
      "0101010101010101010101010101010101010000000001010101010101010101", -16 },
    { SIG_JNE2, ADDRSIG_FUNC,
      "1090837f3c000f8500000000807f38000f8500000000498b8550020000488b58",
      "0101010101010101000000000101010101010000000001010101010101010101", -16 },
    { SIG_CMP, ADDRSIG_FUNC,
      "750e488b06ba01000000488bceff1090837f3c000f8500000000807f38000f85",
      "0101010101010101010101010101010101010101010100000000010101010101", -16 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_CLOCK, 0, 0, 0 },
    { SIG_JNE1,  0, 0, 0 },
    { SIG_JNE2,  0, 0, 0 },
    { SIG_CMP,   0, 0, 0 },
};

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    uintptr_t     clock;
    uintptr_t     jne1;       /* “剩余 != 0 不出洞”跳转 */
    uintptr_t     jne2;       /* +0x38 检查跳转 */
    uintptr_t     cmp_x;      /* cmp 立即数字节地址 */
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    char          cfg_path[MAX_PATH];
    LogKit        log;
    GameProfile   profile;
    uint64_t      cfg_mtime;
    /* 补丁状态 */
    unsigned char orig_jne1[6];
    unsigned char orig_jne2[6];
    unsigned char orig_cmp_x;
    volatile LONG patched;
    volatile LONG first_hole;   /* 1 = 第一块出洞；0 = 区间规则（默认） */
    /* HUD */
    HWND          hud_hwnd;
    volatile LONG hud_n;        /* 还需砸的石头数，-1 = 隐藏 */
    volatile LONG hud_text_on;
    WCHAR         hud_text[24];
} MineContext;

static MineContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);
static void place_hud(void);
static void read_config(void);
static void apply_mode(void);
static void restore_originals(void);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "qk", TRUE);
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
    lstrcatA(g_ctx.log_path, "minehelper.log");
    lstrcpynA(g_ctx.cfg_path, g_ctx.self_dir, MAX_PATH);
    lstrcatA(g_ctx.cfg_path, "minehelper.txt");
    g_ctx.hud_n = -1;
}

static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "qk";
    g_ctx.profile.base = g_ctx.base;
    g_ctx.profile.sigs = kSigs;
    g_ctx.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_ctx.profile.res = s_res;
    g_ctx.profile.entries = s_entries;
    g_ctx.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_ctx.profile);
    g_ctx.clock = profile_addr(&g_ctx.profile, SIG_CLOCK);
    g_ctx.jne1 = profile_addr(&g_ctx.profile, SIG_JNE1);
    g_ctx.jne2 = profile_addr(&g_ctx.profile, SIG_JNE2);
    g_ctx.cmp_x = profile_addr(&g_ctx.profile, SIG_CMP);
    if (g_ctx.cmp_x) g_ctx.cmp_x += CMP_IMM_OFF;
    return (g_ctx.clock && g_ctx.jne1 && g_ctx.jne2 && g_ctx.cmp_x) ? TRUE
                                                                    : FALSE;
}

/* ==================================================================
 * 5 配置（热重载）
 * ================================================================== */
static uint64_t get_mtime(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(g_ctx.cfg_path, GetFileExInfoStandard, &fd))
        return 0;
    return ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
           fd.ftLastWriteTime.dwLowDateTime;
}

static void read_config(void)
{
    FILE* f = fopen(g_ctx.cfg_path, "r");
    char line[256];
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (_strnicmp(line, "FirstHole=", 10) == 0)
                g_ctx.first_hole = (atoi(line + 10) != 0);
        }
        fclose(f);
    }
    g_ctx.cfg_mtime = get_mtime();
    log_direct("[qk] config FirstHole=%d", (int)g_ctx.first_hole);
    if (g_ctx.first_hole) {
        lstrcpynW((WCHAR*)g_ctx.hud_text, L"FirstHole ON", 24);
        g_ctx.hud_text_on = 1;
        g_ctx.hud_n = -1;
    } else {
        g_ctx.hud_text_on = 0;
    }
    place_hud();
}

/* ==================================================================
 * 6 补丁
 * ================================================================== */
static void write_bytes(uintptr_t addr, const void* bytes, int n)
{
    mk_write((void*)addr, bytes, (size_t)n);
}

static void nop6(uintptr_t addr)
{
    static const unsigned char nop[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    write_bytes(addr, nop, 6);
}

static void set_cmp_x(unsigned char x)
{
    write_bytes(g_ctx.cmp_x, &x, 1);
}

/* 记录未修改的原始字节（启动时一次） */
static BOOL save_originals(void)
{
    if (!g_ctx.jne1 || !g_ctx.jne2 || !g_ctx.cmp_x) return FALSE;
    if (!mk_read(g_ctx.orig_jne1, (const void*)g_ctx.jne1, 6)) return FALSE;
    if (!mk_read(g_ctx.orig_jne2, (const void*)g_ctx.jne2, 6)) return FALSE;
    if (!mk_read(&g_ctx.orig_cmp_x, (const void*)g_ctx.cmp_x, 1)) return FALSE;
    return TRUE;
}

/* 离开矿洞必须恢复：这些字节影响通用破坏/收割结算路径 */
static void restore_originals(void)
{
    if (!g_ctx.patched) return;
    write_bytes(g_ctx.jne1, g_ctx.orig_jne1, 6);
    write_bytes(g_ctx.jne2, g_ctx.orig_jne2, 6);
    set_cmp_x(g_ctx.orig_cmp_x);
    g_ctx.patched = 0;
}

static void apply_mode(void)
{
    if (g_ctx.first_hole)
        nop6(g_ctx.jne1);              /* 剩余 != 0 也出洞 → 第一块就出 */
    else
        write_bytes(g_ctx.jne1, g_ctx.orig_jne1, 6);  /* 区间规则：恢复原 jne */
    nop6(g_ctx.jne2);                  /* +0x38 检查始终跳过 */
    g_ctx.patched = 1;
}

/* ==================================================================
 * 7 矿洞对象
 * ================================================================== */
static int read_ptr(uintptr_t addr, uintptr_t* out)
{
    uintptr_t v = 0;
    if (!mk_read(&v, (const void*)addr, 8)) return 0;
    if (v < 0x10000) return 0;
    *out = v;
    return 1;
}

static uintptr_t get_level_obj(void)
{
    uintptr_t g = 0, d = 0, lvl = 0;
    if (!g_ctx.clock || !read_ptr(g_ctx.clock, &g)) return 0;
    if (!read_ptr(g + 0x208, &d)) return 0;
    if (!read_ptr(d + LEVEL_OFF, &lvl)) return 0;
    return lvl;
}

/* ==================================================================
 * 8 HUD（红色 24 号；区间模式显示剩余石头数，FirstHole 模式显示状态）
 * ================================================================== */
static void place_hud(void)
{
    int tx = 8, ty = 8;
    if (!g_ctx.hud_hwnd) return;
    if (g_ctx.first_hole) {
        int sx = GetSystemMetrics(SM_CXSCREEN);
        int sy = GetSystemMetrics(SM_CYSCREEN);
        HWND game = FindWindowA("Honogurashinoniwa", NULL);
        RECT wr;
        tx = sx - 180 - 8;
        ty = sy - 64 - 8;
        if (game && GetWindowRect(game, &wr)) {
            tx = wr.right - 180 - 8;
            ty = wr.bottom - 64 - 8;
        }
    }
    SetWindowPos(g_ctx.hud_hwnd, HWND_TOPMOST, tx, ty, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_ctx.hud_hwnd, NULL, TRUE);
}

static LRESULT CALLBACK hud_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HFONT font, old;
        WCHAR buf[32];
        RECT tr = { 8, 4, 170, 60 };
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        font = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           0, L"Microsoft YaHei");
        old = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, RGB(255, 0, 0));
        if (g_ctx.hud_text_on)
            lstrcpynW(buf, (const WCHAR*)g_ctx.hud_text, 32);
        else if (g_ctx.hud_n >= 0)
            swprintf(buf, 32, L"%d", (int)g_ctx.hud_n);
        else
            buf[0] = 0;
        DrawTextW(hdc, buf, -1, &tr, DT_NOPREFIX);
        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI hud_thread(LPVOID param)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    (void)param;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hud_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "QuarryQuickHud";
    if (!RegisterClassA(&wc)) return 0;
    hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "QuarryQuickHud", "QQ", WS_POPUP, 0, 0, 180, 64,
        NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 0;
    g_ctx.hud_hwnd = hwnd;
    place_hud();
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 200, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ==================================================================
 * 9 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    int last_floor = 0;
    int x = -1;
    int f9_prev = 0;

    (void)param;
    Sleep(300);
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    if (!load_profile()) {
        log_direct("[qk] FATAL: addrsig MISS, mod disabled");
        return 0;
    }
    if (!save_originals()) {
        log_direct("[qk] FATAL: 读取原始字节失败, mod disabled");
        return 0;
    }
    read_config();
    log_direct("[qk] MineHelper v1.1.1 loaded; base=%p", (void*)g_ctx.base);

    while (1) {
        uintptr_t obj;
        int rem = -1, floor = 0;
        int f9;

        Sleep(100);
        f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9_prev) {
            g_ctx.first_hole = !g_ctx.first_hole;
            log_direct("[qk] F9 toggle FirstHole=%d", (int)g_ctx.first_hole);
            if (g_ctx.first_hole) {
                lstrcpynW((WCHAR*)g_ctx.hud_text, L"FirstHole ON", 24);
                g_ctx.hud_text_on = 1;
                g_ctx.hud_n = -1;
            } else {
                g_ctx.hud_text_on = 0;
            }
            place_hud();
            if (g_ctx.patched) apply_mode();   /* 矿洞内立即按新模式应用 */
        }
        f9_prev = f9;

        if (get_mtime() != g_ctx.cfg_mtime) {
            read_config();
            if (g_ctx.patched) apply_mode();
        }

        obj = get_level_obj();
        if (!obj) {
            restore_originals();   /* 离开矿洞：恢复通用结算代码 */
            last_floor = 0;
            x = -1;
            g_ctx.hud_n = -1;
            continue;
        }
        if (!mk_read(&rem, (const void*)(obj + F_REMAIN), 4) ||
            rem < 0 || rem > MAX_REMAIN) {
            restore_originals();
            last_floor = 0;
            x = -1;
            g_ctx.hud_n = -1;
            continue;
        }
        mk_read(&floor, (const void*)(obj + F_FLOOR), 4);
        if (floor <= 0 || floor > 9999) {
            restore_originals();
            last_floor = 0;
            x = -1;
            g_ctx.hud_n = -1;
            continue;
        }

        if (g_ctx.first_hole) {
            g_ctx.hud_n = -1;      /* 第一块出洞模式无需数字 HUD */
            if (!g_ctx.patched) apply_mode();
            last_floor = floor;
            continue;
        }

        if (last_floor == 0 || floor != last_floor) {
            last_floor = floor;
            if (rem > 0) {
                int a = rem;
                int b;
                int X;
                unsigned seed = GetTickCount() ^ (unsigned)floor ^
                                (unsigned)(uintptr_t)obj;
                if (a < 10) b = seed % a + 1;          /* [1, a] */
                else if (a < 30) b = seed % 6 + 5;     /* [5, 10] */
                else b = seed % 6 + 10;                /* [10, 15] */
                X = a - b;
                if (X > 127) { X = 127; b = a - X; }
                if (X < 0) X = 0;
                if (!g_ctx.patched) apply_mode();
                set_cmp_x((unsigned char)X);
                x = X;
                log_direct("[qk] floor=%d a=%d b=%d X=%d", floor, a, b, X);
            }
            continue;
        }
        g_ctx.hud_n = (x >= 0) ? rem - x : -1;   /* 还需砸的石头数 */
    }
    return 0;
}

/* ==================================================================
 * 10 生命周期
 * ================================================================== */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE hh, hw;
        DisableThreadLibraryCalls(hinst);
        SetProcessDPIAware();   /* 统一物理像素坐标，HUD 定位准确 */
        ctx_init(hinst);
        log_init();
        hh = CreateThread(NULL, 0, hud_thread, NULL, 0, NULL);
        if (hh) CloseHandle(hh);
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    } else if (reason == DLL_PROCESS_DETACH) {
        log_close();
    }
    return TRUE;
}
