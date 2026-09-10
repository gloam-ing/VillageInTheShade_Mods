/* autopet.c - AutoPet v0.2.5（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文
 *       / 5 配置 / 6 动物链表与事件 / 7 抚摸结算 / 8 提示窗
 *       / 9 工作线程 / 10 生命周期
 *
 * 功能：每天自动抚摸所有家畜/宠物（不限房屋等级）。
 *
 * 原理（纯数据操作，不修改指令字节）：
 *   动物链表：[[exe+clock]+0x208]+0x3430（头），+0x3438 为数量；
 *     节点 { next, prev, animal }。
 *   动物对象：+0x00 虚表（vt_pet），+0xE0 UID，+0x300 好感（上限 2000），
 *     +0x2A0 名字指针；+0x320 为游戏"今日已进食/处理"标记，mod 不写。
 *   好感结算：aff += 5 × GainMultiplier，钳制 [0,2000]，写回 +0x300。
 *   caress 事件必须走游戏原生 map（纯写字段游戏不识别）：
 *     map = animal+0x18，key = "caress"；
 *     find = map_find，emplace = map_emplace，事件值写 [node+0x18]。
 *   神社好感技能查询在 1.08.1 未定位到替代函数，固定使用基础值 5。
 *
 * 编译（在 AutoPet_v0.2.5 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o autopet.dll
 *     autopet.c ..\shared\addrsig.c ..\shared\modkit.c -lgdi32 -luser32
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
#define NAME_OFF   0x2A0      /* 动物名字（UTF-8 字符串指针） */
#define AFF_OFF    0x300      /* 好感（0..2000） */
#define AFF_MAX    2000
#define EV_MAP_OFF 0x18       /* 事件流 map 对象（动物对象内偏移） */
#define KEY_CARESS 0x737365726163ULL   /* "caress" 8 字节键 */
#define BASE_GAIN  5          /* 基础好感加成 */

#define SIG_CLOCK    "clock_0x10CD990"
#define SIG_VT_PET   "vt_pet_0xE028C8"
#define SIG_MAP_FIND "map_find_0x150B10"
#define SIG_MAP_EMP  "map_emplace_0x150950"

static const AddrSig kSigs[] = {
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
    { SIG_VT_PET, ADDRSIG_RIPGLOBAL,
      "431033ff4889bb40020000488d8b48020000e80000000090488d0500000000488903488d0500000000488943104889bb90020000488d05",
      "01010101010101010101010101010101010101000000000101010100000000010101010101000000000101010101010101010101010101", 27 },
    { SIG_MAP_FIND, ADDRSIG_FUNC,
      "890641c6460800498bc64883c430415f415e415c5f5e5d5bc3488bf848b8ffffffffffffff0748394610750e488d0d40",
      "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 85 },
    { SIG_MAP_EMP, ADDRSIG_FUNC,
      "890641c6460800498bc64883c430415f415e415c5f5e5d5bc3488bf848b8ffffffffffffff0748394610750e488d0d00",
      "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 85 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_CLOCK,    0, 0, 0 },
    { SIG_VT_PET,   0, 0, 0 },
    { SIG_MAP_FIND, 0, 0, 0 },
    { SIG_MAP_EMP,  0, 0, 0 },
};

typedef uintptr_t (__fastcall *EvMapFn)(uintptr_t, uintptr_t, uintptr_t);

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    uintptr_t     clock;
    uintptr_t     vft;
    uintptr_t     ev_find;
    uintptr_t     ev_emp;
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    char          cfg_path[MAX_PATH];
    LogKit        log;
    GameProfile   profile;
    volatile LONG mult;
    HWND          toast_hwnd;
    WCHAR         toast_text[128];
} AutoPetContext;

static AutoPetContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);
static void show_toast_2s(const WCHAR* text);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "ap", TRUE);
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
    lstrcatA(g_ctx.log_path, "autopet.log");
    lstrcpynA(g_ctx.cfg_path, g_ctx.self_dir, MAX_PATH);
    lstrcatA(g_ctx.cfg_path, "autopet.txt");
    g_ctx.mult = 1;
}

static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "ap";
    g_ctx.profile.base = g_ctx.base;
    g_ctx.profile.sigs = kSigs;
    g_ctx.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_ctx.profile.res = s_res;
    g_ctx.profile.entries = s_entries;
    g_ctx.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_ctx.profile);
    g_ctx.clock = profile_addr(&g_ctx.profile, SIG_CLOCK);
    g_ctx.vft = profile_addr(&g_ctx.profile, SIG_VT_PET);
    g_ctx.ev_find = profile_addr(&g_ctx.profile, SIG_MAP_FIND);
    g_ctx.ev_emp = profile_addr(&g_ctx.profile, SIG_MAP_EMP);
    return (g_ctx.clock && g_ctx.vft && g_ctx.ev_find && g_ctx.ev_emp) ? TRUE
                                                                       : FALSE;
}

/* ==================================================================
 * 5 配置（实时生效）：GainMultiplier=1
 * ================================================================== */
static void load_config(void)
{
    HANDLE f = CreateFileA(g_ctx.cfg_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[2048];
    DWORD rd = 0;
    if (f == INVALID_HANDLE_VALUE) return;
    if (ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) {
        char* p;
        buf[rd] = 0;
        p = strstr(buf, "GainMultiplier");
        if (p) {
            p = strchr(p, '=');
            if (p) {
                int v = atoi(p + 1);
                if (v >= 1 && v <= 100) {
                    int old = InterlockedExchange(&g_ctx.mult, v);
                    if (old != v) log_direct("[ap] GainMultiplier=%d", v);
                }
            }
        }
    }
    CloseHandle(f);
}

/* ==================================================================
 * 6 动物链表与事件 map
 * ================================================================== */
static uint64_t rd64(uintptr_t a)
{
    uint64_t v = 0;
    if (!mk_read(&v, (const void*)a, 8)) return 0;
    return v;
}

static uint32_t rd32(uintptr_t a)
{
    uint32_t v = 0;
    if (!mk_read(&v, (const void*)a, 4)) return 0;
    return v;
}

static void wr32(uintptr_t a, uint32_t v)
{
    mk_write((void*)a, &v, 4);
}

static uintptr_t get_manager(void)
{
    uintptr_t g, mgr;
    if (!g_ctx.clock) return 0;
    g = rd64(g_ctx.clock);
    if (g < 0x10000) return 0;
    mgr = rd64(g + 0x208);
    if (mgr < 0x10000) return 0;
    return mgr;
}

/* 读动物名字（UTF-8）到 buf */
static int read_name(uintptr_t animal, char* buf, int cap)
{
    uintptr_t p = (uintptr_t)rd64(animal + NAME_OFF);
    int i;
    if (p < 0x10000) {
        buf[0] = 0;
        return 0;
    }
    if (!mk_read(buf, (const void*)p, (size_t)(cap - 1))) {
        buf[0] = 0;
        return 0;
    }
    buf[cap - 1] = 0;
    for (i = 0; i < cap - 1; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) break;
        if (c < 0x20 || c == 0x7F) {
            buf[0] = 0;
            return 0;
        }
    }
    buf[i] = 0;
    return (int)strlen(buf);
}

/* 读 caress 事件（游戏原生 map find）；返回事件值（0 = 未抚摸） */
static int ev_read_caress(uintptr_t animal)
{
    EvMapFn find;
    uint64_t key = KEY_CARESS;
    unsigned char out[16] = { 0 };
    uintptr_t node = 0;

    if (!g_ctx.ev_find) return 0;
    find = (EvMapFn)g_ctx.ev_find;
    __try {
        find(animal + EV_MAP_OFF, (uintptr_t)&out, (uintptr_t)&key);
    } __except (1) {
        return 0;
    }
    memcpy(&node, out, 8);
    if (!node) return 0;
    return (int)rd32(node + 0x18);
}

/* 写 caress 事件 = 1（游戏原生 map emplace）；返回 1 成功 */
static int ev_write_caress(uintptr_t animal)
{
    EvMapFn emp;
    uint64_t key = KEY_CARESS;
    unsigned char out[16] = { 0 };
    uintptr_t node = 0;
    uint32_t one = 1;

    if (!g_ctx.ev_emp) return 0;
    emp = (EvMapFn)g_ctx.ev_emp;
    __try {
        emp(animal + EV_MAP_OFF, (uintptr_t)&out, (uintptr_t)&key);
    } __except (1) {
        return 0;
    }
    memcpy(&node, out, 8);
    if (!node) return 0;
    return mk_write((void*)(node + 0x18), &one, 4) ? 1 : 0;
}

/* ==================================================================
 * 7 抚摸结算
 * ================================================================== */
/* 对单只动物执行抚摸；返回 1 表示本次实际抚摸 */
static int pet_animal(uintptr_t animal)
{
    uintptr_t vft = rd64(animal);
    uint32_t uid = rd32(animal + 0xE0);
    int32_t aff;
    int gain;
    int64_t nv;
    char name[64];

    if (vft != g_ctx.vft || uid == 0) return 0;   /* 跳过哨兵 */
    if (ev_read_caress(animal) != 0) return 0;    /* caress 已写 */

    aff = (int32_t)rd32(animal + AFF_OFF);
    gain = BASE_GAIN * (int)g_ctx.mult;
    if (gain <= 0) gain = BASE_GAIN;
    nv = (int64_t)aff + gain;
    if (nv > AFF_MAX) nv = AFF_MAX;
    if (nv < 0) nv = 0;
    wr32(animal + AFF_OFF, (uint32_t)nv);
    ev_write_caress(animal);

    if (read_name(animal, name, sizeof(name)) > 0)
        log_direct("[ap] pet name=%s uid=%u aff=%d -> %lld", name, uid, aff,
                   (long long)nv);
    else
        log_direct("[ap] pet uid=%u aff=%d -> %lld", uid, aff, (long long)nv);
    return 1;
}

/* ==================================================================
 * 8 提示窗（2 秒后消失）
 * ================================================================== */
static void show_toast_2s(const WCHAR* text)
{
    lstrcpynW(g_ctx.toast_text, text, 127);
    g_ctx.toast_text[127] = 0;
    if (g_ctx.toast_hwnd) {
        KillTimer(g_ctx.toast_hwnd, 1);
        ShowWindow(g_ctx.toast_hwnd, SW_SHOWNOACTIVATE);
        SetTimer(g_ctx.toast_hwnd, 1, 2000, NULL);
        InvalidateRect(g_ctx.toast_hwnd, NULL, TRUE);
    }
}

static LRESULT CALLBACK toast_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        HFONT font, old;
        RECT tr = { 10, 8, 480, 72 };
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        font = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                           0, L"Microsoft YaHei");
        old = (HFONT)SelectObject(hdc, font);
        SetTextColor(hdc, RGB(80, 255, 120));
        DrawTextW(hdc, g_ctx.toast_text, -1, &tr,
                  DT_NOPREFIX | DT_LEFT | DT_VCENTER);
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

static BOOL CALLBACK find_game_wnd(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc) && rc.right - rc.left > 200) {
            HWND* out = (HWND*)lp;
            *out = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static DWORD WINAPI toast_thread(LPVOID param)
{
    WNDCLASSA wc;
    HWND game = NULL;
    RECT wr;
    int tx, ty;
    MSG msg;

    (void)param;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = toast_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AutoPetToast";
    if (!RegisterClassA(&wc)) return 0;
    EnumWindows(find_game_wnd, (LPARAM)&game);
    tx = GetSystemMetrics(SM_CXSCREEN) - 500;
    ty = GetSystemMetrics(SM_CYSCREEN) - 80;
    if (game && GetWindowRect(game, &wr)) {
        tx = wr.right - 500;
        ty = wr.bottom - 80;
    }
    g_ctx.toast_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "AutoPetToast", "AP", WS_POPUP, tx, ty, 492, 64,
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
 * 9 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    int none_pet = 0;

    (void)param;
    Sleep(800);
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    log_direct("[ap] AutoPet v0.2.5 loaded; base=%p", (void*)g_ctx.base);
    if (!load_profile()) {
        log_direct("[ap] FATAL: addrsig MISS, mod disabled");
        return 0;
    }

    while (1) {
        uintptr_t mgr, head, node;
        uint64_t cnt;
        int petted = 0;
        uint64_t i;

        Sleep(2000);
        load_config();
        mgr = get_manager();
        if (!mgr) {
            none_pet = 0;
            continue;
        }
        head = rd64(mgr + 0x3430);
        cnt = rd64(mgr + 0x3438);
        if (!head || cnt == 0 || cnt > 200) {
            none_pet = 0;
            continue;
        }

        node = rd64(head);
        for (i = 0; i < cnt && node && node != head; i++) {
            uintptr_t animal = rd64(node + 0x10);
            uintptr_t next;
            if (animal) petted += pet_animal(animal);
            next = rd64(node);
            if (!next || next == head) break;
            node = next;
        }
        if (petted) {
            WCHAR txt[128];
            none_pet = 0;
            wsprintfW(txt, L"\u5df2\u81ea\u52a8\u629a\u6478 %d \u53ea\u52a8\u7269",
                      petted);
            show_toast_2s(txt);
        } else if (!none_pet) {
            none_pet = 1;
            log_direct("[ap] scan done: no pending animals (all petted today?)");
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
        ctx_init(hinst);
        log_init();
        ht = CreateThread(NULL, 0, toast_thread, NULL, 0, NULL);
        if (ht) CloseHandle(ht);
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    } else if (reason == DLL_PROCESS_DETACH) {
        log_close();
    }
    return TRUE;
}
