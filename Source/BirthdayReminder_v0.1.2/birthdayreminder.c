/* birthdayreminder.c - BirthdayReminder v0.1.2（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 生日数据表 / 3 运行期状态 / 4 日志 / 5 上下文
 *       / 6 时间读取 / 7 提示窗 / 8 生日计算 / 9 工作线程 / 10 生命周期
 *
 * 功能：每天检测游戏日期，当天有 NPC 生日时在游戏窗口右上角持续
 * 显示“今日生日：XXX + 喜爱礼物”，无人隐藏，跨日刷新，睡觉/跳时
 * （时间大幅前进）隐藏当天提醒。
 *
 * 原理：游戏时间 [[exe+clock]+0x208]+0x3270（QWORD 累计秒），
 * 1 天 = 86400 秒，4 季 × 28 天 = 112 天/年，day0 = 春 1。
 * 时钟全局由 addrsig 运行时解析，MISS 时停用。
 *
 * 编译（在 BirthdayReminder_v0.1.2 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o birthdayreminder.dll
 *     birthdayreminder.c ..\shared\addrsig.c ..\shared\modkit.c
 *     -lgdi32 -luser32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "addrsig.h"
#include "modkit.h"

/* ==================================================================
 * 1 版本档案
 * ================================================================== */
#define TIME_OFF        0x3270
#define SECONDS_PER_DAY 86400
#define DAYS_PER_SEASON 28
#define SEASON_COUNT    4
#define SLEEP_DELTA_SEC 600   /* 时间前进超过 10 游戏分钟视为睡觉/跳时 */

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
 * 2 生日数据表（0=春 1=夏 2=秋 3=冬，day = 1..28）
 * ================================================================== */
typedef struct {
    const WCHAR* name;
    const WCHAR* fav;
    int season;
    int day;
} Birthday;

static const Birthday BIRTHDAYS[] = {
    { L"帷",     L"铝箔纸烤大蒜、樱桃萝卜、乌龙茶套组、黄宝石", 1, 17 },
    { L"木助",   L"芋头、麦烧酎、地瓜烧酎、牛肉干", 0, 18 },
    { L"四郎治", L"玛格丽特披萨、薯条、豆腐汉堡排、波斯菊", 1,  7 },
    { L"林",     L"大头菜、玫瑰果酱、金矿石、蓝宝石", 0,  4 },
    { L"驹子",   L"焗烤马铃薯、暖呼呼火锅、向日葵、青金石", 2,  1 },
    { L"茶梅",   L"柿子蜜饯、古典巧克力蛋糕、巧克力香蕉、琥珀", 3, 20 },
    { L"六角",   L"梅子茶泡饭、姜、松露、梅酒", 3, 15 },
    { L"今野",   L"豆皮乌龙面、野生小动物的肉、日本酒、金矿石", 1, 11 },
    { L"名护",   L"家乡味定食、酸梅干、葡萄酒、碧玺", 2, 26 },
    { L"洋",     L"儿童咖喱、草莓蛋糕、牛奶寒天冻、郁金香", 2, 12 },
    { L"裕太",   L"炖牛肉、夏日圣品西瓜、菠菜、独角仙", 2,  8 },
    { L"堇怜",   L"烤鱼、盐烤香鱼、香鱼、鲑鱼", 0, 21 },
    { L"莲实",   L"茶香冰淇淋、焙茶套组、玫瑰、紫水晶", 3,  3 },
    { L"琪娜娜", L"自创拉面、木槿、啤酒、粉水晶", 1, 24 },
};
#define BIRTHDAY_COUNT (sizeof(BIRTHDAYS) / sizeof(BIRTHDAYS[0]))

/* ==================================================================
 * 3 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t base;
    uintptr_t clock;        /* addrsig 解析的时钟全局地址 */
    char      self_dir[MAX_PATH];
    char      log_path[MAX_PATH];
    WCHAR     font_path[MAX_PATH];
    LogKit    log;
    GameProfile profile;
    /* HUD */
    HWND  hud_hwnd;
    WCHAR display[512];
} BirthdayContext;

static BirthdayContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);
static void set_display_text(const WCHAR* text);
static int cm_to_px(double cm);
static void load_game_font(void);
static void unload_game_font(void);

/* ==================================================================
 * 4 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "br", TRUE);
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
 * 5 上下文
 * ================================================================== */
static void ctx_init(HINSTANCE hinst)
{
    GetModuleFileNameA(hinst, g_ctx.self_dir, MAX_PATH);
    {
        char* slash = strrchr(g_ctx.self_dir, '\\');
        if (slash) *(slash + 1) = 0;
    }
    lstrcpynA(g_ctx.log_path, g_ctx.self_dir, MAX_PATH);
    lstrcatA(g_ctx.log_path, "birthdayreminder.log");
}

static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "br";
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

/* 加载游戏同款字体（FR_PRIVATE 只对当前进程生效，不装系统） */
static void load_game_font(void)
{
    GetModuleFileNameW(NULL, g_ctx.font_path, MAX_PATH);
    {
        WCHAR* slash = wcsrchr(g_ctx.font_path, L'\\');
        if (slash) *(slash + 1) = 0;
    }
    lstrcatW(g_ctx.font_path, L"ResourceHanRoundedTC-Regular.ttf");
    if (AddFontResourceExW(g_ctx.font_path, FR_PRIVATE, 0) > 0)
        log_direct("[br] game font loaded: %ls", g_ctx.font_path);
    else
        log_direct("[br] font load failed (fallback font will be used)");
}

static void unload_game_font(void)
{
    if (g_ctx.font_path[0])
        RemoveFontResourceExW(g_ctx.font_path, FR_PRIVATE, 0);
}

/* ==================================================================
 * 6 时间读取
 * ================================================================== */
static int read_ptr(uintptr_t addr, uintptr_t* out)
{
    uintptr_t v = 0;
    if (!mk_read(&v, (const void*)addr, 8)) return 0;
    if (v < 0x10000) return 0;
    *out = v;
    return 1;
}

static int64_t read_game_seconds(void)
{
    uintptr_t g = 0, d = 0;
    int64_t sec = -1;
    if (!g_ctx.clock) return -1;
    if (!read_ptr(g_ctx.clock, &g)) return -1;
    if (!read_ptr(g + 0x208, &d)) return -1;
    if (!mk_read(&sec, (const void*)(d + TIME_OFF), 8)) return -1;
    return sec;
}

/* ==================================================================
 * 7 提示窗
 * ================================================================== */
/* 按系统 DPI 把厘米换算成像素 */
static int cm_to_px(double cm)
{
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    return (int)(cm * dpi / 2.54);
}

static void set_display_text(const WCHAR* text)
{
    lstrcpynW(g_ctx.display, text, 511);
    g_ctx.display[511] = 0;
    if (g_ctx.hud_hwnd) {
        if (text[0])
            ShowWindow(g_ctx.hud_hwnd, SW_SHOWNOACTIVATE);
        else
            ShowWindow(g_ctx.hud_hwnd, SW_HIDE);
        InvalidateRect(g_ctx.hud_hwnd, NULL, TRUE);
    }
}

static LRESULT CALLBACK hud_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HFONT font, old;
        RECT mr = { 0, 0, 0, 0 };
        int fav_x, ly = 6, lh = 44, line_no = 0;
        WCHAR* text = g_ctx.display;

        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        font = CreateFontW(30, 0, 0, 0, FW_BOLD, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           0, L"Resource Han Rounded TC");
        old = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, L"喜爱礼物：", -1, &mr, DT_CALCRECT | DT_NOPREFIX);
        fav_x = 12 + (mr.right - mr.left);
        while (*text) {
            WCHAR* eol = wcschr(text, L'\n');
            int len = eol ? (int)(eol - text) : (int)wcslen(text);
            WCHAR tmp[512];
            RECT lr;
            int lx;
            if (len > 0 && text[len - 1] == L'\r') len--;
            if (len >= 512) len = 511;
            wcsncpy(tmp, text, len);
            tmp[len] = 0;
            line_no++;
            lx = (line_no >= 3) ? fav_x : 12;  /* 第 3 行起对齐第一个礼物 */
            lr.left = lx;
            lr.top = ly;
            lr.right = 448;
            lr.bottom = ly + lh;
            DrawTextW(hdc, tmp, -1, &lr, DT_NOPREFIX | DT_SINGLELINE);
            ly += lh;
            if (!eol) break;
            text = eol + 1;
        }
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

/* 右上角 HUD 位置：游戏窗口右上角右移 1cm、下移 0.5cm，避开原版 HUD */
static void hud_position(HWND game, RECT* wr, int* px, int* py)
{
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int dx = cm_to_px(1.0), dy = cm_to_px(0.5);
    *px = sx - 420 + dx;
    *py = 8 + 13 * 30 + dy;
    if (game && wr && GetWindowRect(game, wr))
        *py = wr->top + 8 + 13 * 30 + dy;
}

static DWORD WINAPI hud_thread(LPVOID param)
{
    WNDCLASSA wc;
    HWND game;
    RECT wr;
    int hx, hy;
    MSG msg;

    (void)param;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = hud_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "BirthdayReminderHud";
    if (!RegisterClassA(&wc)) return 0;
    game = FindWindowA("Honogurashinoniwa", NULL);
    hud_position(game, &wr, &hx, &hy);
    g_ctx.hud_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "BirthdayReminderHud", "BR", WS_POPUP, hx, hy, 420, 280,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_ctx.hud_hwnd) return 0;
    log_direct("[br] hud pos: screen=%dx%d game.right=%d hx=%d hy=%d",
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
               game ? wr.right : -1, hx, hy);
    SetLayeredWindowAttributes(g_ctx.hud_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    SetTimer(g_ctx.hud_hwnd, 1, 250, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ==================================================================
 * 8 生日计算与显示
 * ================================================================== */
static void refresh_birthday(int64_t sec)
{
    int64_t day;
    int season, doy;
    WCHAR buf[512];
    int n, found = 0, i;
    const WCHAR* fav = NULL;

    if (sec < 0) {
        set_display_text(L"");
        return;
    }
    day = sec / SECONDS_PER_DAY;
    if (day < 0) {
        set_display_text(L"");
        return;
    }
    season = (int)((day / DAYS_PER_SEASON) % SEASON_COUNT);
    doy = (int)(day % DAYS_PER_SEASON) + 1;

    n = swprintf(buf, 512, L"今日生日：");
    for (i = 0; i < (int)BIRTHDAY_COUNT; i++) {
        if (BIRTHDAYS[i].season != season || BIRTHDAYS[i].day != doy)
            continue;
        n += swprintf(buf + n, 512 - n, found ? L" %s" : L"%s",
                      BIRTHDAYS[i].name);
        if (!fav) fav = BIRTHDAYS[i].fav;
        found = 1;
    }
    if (!found) {
        set_display_text(L"");
        return;
    }
    n += swprintf(buf + n, 512 - n, L"\r\n喜爱礼物：");
    if (fav) {
        const WCHAR* p = fav;
        int first = 1;
        while (p && *p) {
            const WCHAR* sep = wcschr(p, L'、');
            int len = sep ? (int)(sep - p) : (int)wcslen(p);
            if (len > 0) {
                WCHAR tmp[64];
                if (len >= 64) len = 63;
                wcsncpy(tmp, p, len);
                tmp[len] = 0;
                n += swprintf(buf + n, 512 - n, first ? L"%ls" : L"\r\n%ls",
                              tmp);
                first = 0;
            }
            if (!sep) break;
            p = sep + 1;
        }
    } else {
        n += swprintf(buf + n, 512 - n, L"—");
    }
    log_direct("[br] day=%lld season=%d doy=%d -> %ls", (long long)day,
               season, doy, buf);
    set_display_text(buf);
}

/* ==================================================================
 * 9 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    int64_t last_day = -1;
    int64_t last_sec = -1;
    int follow_tick = 0;

    (void)param;
    Sleep(600);
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    load_profile();
    log_direct("[br] BirthdayReminder v0.1.2 loaded; base=%p",
               (void*)g_ctx.base);
    if (!g_ctx.clock) {
        log_direct("[br] FATAL: clock sig MISS, mod disabled");
        return 0;
    }

    while (1) {
        int64_t sec, day;
        Sleep(500);
        /* 每 5 秒跟随游戏窗口位置（全屏切换/分辨率变化后纠正偏移） */
        if ((++follow_tick % 10) == 0 && g_ctx.hud_hwnd) {
            HWND game = FindWindowA("Honogurashinoniwa", NULL);
            RECT wr;
            int tx, ty;
            hud_position(game, &wr, &tx, &ty);
            {
                RECT cr;
                if (GetWindowRect(g_ctx.hud_hwnd, &cr) &&
                    (cr.left != tx || cr.top != ty)) {
                    MoveWindow(g_ctx.hud_hwnd, tx, ty, 420, 280, TRUE);
                    log_direct("[br] hud repositioned to (%d, %d)", tx, ty);
                }
            }
        }
        sec = read_game_seconds();
        if (sec < 0) continue;
        /* 睡觉/跳时：时间大幅前进则当天提醒消失 */
        if (last_sec >= 0 && (sec - last_sec) > SLEEP_DELTA_SEC) {
            set_display_text(L"");
            log_direct("[br] sleep/skip detected (delta=%lld), hidden",
                       (long long)(sec - last_sec));
        }
        last_sec = sec;
        day = sec / SECONDS_PER_DAY;
        if (day != last_day) {
            last_day = day;
            refresh_birthday(sec);
        }
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
        HANDLE ht, hw;
        DisableThreadLibraryCalls(hinst);
        SetProcessDPIAware();   /* 统一物理像素坐标，HUD 定位准确 */
        ctx_init(hinst);
        log_init();
        load_game_font();
        ht = CreateThread(NULL, 0, hud_thread, NULL, 0, NULL);
        if (ht) CloseHandle(ht);
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    } else if (reason == DLL_PROCESS_DETACH) {
        unload_game_font();
        log_close();
    }
    return TRUE;
}
