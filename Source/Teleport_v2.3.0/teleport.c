/*
 * teleport.c - Village in the Shade 地图传送 Mod（Teleport Mod）
 * ------------------------------------------------------------------
 * 版本：2.3.0
 *
 * 功能：
 *   - 按 F6（可配置）打开/关闭“传送地点选择”菜单；手柄 L 键(LB) 等效；
 *   - 按 Alt+F6 在小屋门口站定后手动开始定位（不再自动扫描/跨日自动重扫）；
 *   - 、键（可配置，原 Tab 被游戏占用）在“目的地 / 按键设置”两页间切换；
 *   - ↑↓ 选择目的地，Enter 传送，Esc 关闭（手柄十字键/R 键等效）；
 *   - 手柄：LB 开关菜单，←/→ 选择，↓ 翻页，RB 确认，B 设置页；
 *   - 键位提示统一显示在左上角，中间窗口只显示菜单与目的地（半透明底）；
 *   - 设置页可修改所有按键绑定，自动写回 teleport.txt；
 *   - 目的地名称前显示全局序号；；
 *   - 目的地写在 teleport.txt（Dest1Name/X/Y ...，最多 24 个）。
 *
 * 原理：
 *   - 玩家世界坐标位于 [[玩家对象 + 8] + 560/+564]（float）；
 *   - 传送 = 直接写入该坐标字段。玩家对象主虚表 RVA 0xE121D8
 *     （2026-08-17 游戏更新后版本，RTTI 重算）。
 *
 *
 * 编译：
 *   zig cc -shared -O2 -o teleport.dll teleport.c -lgdi32 -luser32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <tlhelp32.h>

/* 运行时地址签名解析（跨版本自动定位，见 dev_backup\shared\addrsig.h） */
#include "addrsig.h"
#include "modkit.h"

/* ---- XInput 手柄适配（编译时定义 TP_KEYBOARD_ONLY 则生成纯键盘版） ---- */
#ifndef TP_KEYBOARD_ONLY
#define TP_PAD_UP    0x0001
#define TP_PAD_DOWN  0x0002
#define TP_PAD_LEFT  0x0004
#define TP_PAD_RIGHT 0x0008
#define TP_PAD_A     0x1000
#define TP_PAD_LB    0x0100
#define TP_PAD_RB    0x0200
#define TP_PAD_B     0x2000
/* 扳机（Switch 布局 LZ/RZ）映射为自定义高位，与 wButtons 位不冲突 */
#define TP_PAD_LZ    0x00010000
#define TP_PAD_RZ    0x00020000

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} TP_XGAMEPAD;

typedef struct {
    DWORD dwPacketNumber;
    TP_XGAMEPAD Gamepad;
} TP_XSTATE;

typedef DWORD (WINAPI *TP_XIGETSTATE)(DWORD, TP_XSTATE*);
#endif

#define TP_VERSION "2.3.0"
#define TP_VERSION_W L"2.3.0"

/* 玩家实体虚表组（CCom_Unit_Player，4 继承槽位）由运行时签名解析，
 * 不再硬编码 RVA（g_a_player_vt[0..3] 对应旧 0xE10380/0xE10218/
 * 0xE10208/0xE10370，各自在对象内的偏移见 g_a_player_vt_off）。 */
#define PLAYER_POS_OFF 0x3D0    /* sub（碰撞体）+0x3D0 = 世界坐标 x/y（CE 实测） */
#define PLAYER_POS_OFF_OLD 0x230 /* sub +0x230 = 旧版位置字段（相机/渲染仍读） */

/* 传送目的地 */
#define TP_MAX_DEST 24
#define TP_PAGE_SIZE 8
#define SETTINGS_ROWS 8
#define UNLOCK_KEY 0x5A
typedef struct { char name[64]; float x, y; } TPDest;

/* 配置结构（原定义在"配置"区，为收拢运行期状态上移到类型区） */
typedef struct {
    int enabled;
    int teleport;
    int tp_key;
    int hud_key;
    int settings_key;
    int up_key;
    int dn_key;
    int pg_key;
    int nk_key;
    int ok_key;
    int cancel_key;
    int pad_toggle_key;
    int pad_ok_key;
    int pad_up_key;
    int pad_down_key;
    int pad_next_key;
    int pad_settings_key;
    float unlock_radius;
    int diag_stale;   /* 1=传送后扫描仍等于旧坐标的副本并写入日志（排查用） */
    float hud_scale;  /* 0=按屏幕分辨率自动缩放，>0=手动缩放系数（SD 建议 0.67） */
    TPDest dests[TP_MAX_DEST];
    int dest_count;
} Config;

/* ==================================================================
 * 运行期状态：唯一全局 g_ctx（AutoFish 标准）
 * ------------------------------------------------------------------
 * 原 63 个文件作用域全局按用途收进 TpContext；为避免改动全文一千余处
 * 引用点，下面用等名宏把旧名映射到 g_ctx 字段——语义与直接写
 * g_ctx.xxx 完全一致（同一个静态对象的字段），保留旧写法便于对照
 * 历史日志与调试断点。
 * ================================================================== */
typedef struct {
    /* 路径与日志 */
    LogKit        logkit;
    char          log_path[MAX_PATH];
    char          self_dir[MAX_PATH];
    /* 配置与目的地 */
    Config        cfg;
    volatile int  unlocked[TP_MAX_DEST];
    char          unlock_names[128][64];
    int           unlock_n;
    int           dest_order[TP_MAX_DEST];
    int           dest_order_n;
    /* addrsig 解析结果 */
    uintptr_t     base;
    uintptr_t     sub;
    uintptr_t     player;
    uintptr_t     a_clock;
    uintptr_t     a_getctrl;
    uintptr_t     a_player_vt[4];
    uintptr_t     a_sub_vt;
    uintptr_t     a_cam_vt;
    /* 坐标副本与收敛候选 */
    uintptr_t     pa[64];
    float         pa_x[64], pa_y[64];
    int           pa_n;
    volatile int  pa_ready;
    uintptr_t     cand[64];
    float         cand_x[64], cand_y[64];
    int           cand_n;
    unsigned long long last_day;
    /* 变换/物理对象坐标副本缓存 */
    uintptr_t     tf_coord[2];
    int           tf_n;
    uintptr_t     ph_coord[2];
    int           ph_n;
    /* 扫描协调 */
    volatile int  scan_latch;
    volatile int  scan_request;
    volatile int  scan_busy;
    /* 菜单与按键设置 */
    volatile int  menu_open;
    volatile int  menu_sel;
    volatile int  menu_page;
    volatile int  rebind_row;
    volatile int  rebind_armed;
    /* 手柄 / XInput */
    TP_XIGETSTATE tp_xig;
    DWORD         pad_slot;
    volatile DWORD pad_buttons;
    volatile LONG pad_ok_shared;
    BYTE*         xig_export;
    BYTE          key_prev[256];
    DWORD         pad_rebind_prev;
    /* HUD / 状态显示 */
    volatile float ov_px, ov_py;
    volatile int  last_input;
    volatile int  show_pos;
    volatile int  tele_status;
    float         ui_scale;
    HWND          hwnd;
    HWND          menu_hwnd;
    int           hud_live;
    float         hud_prev[64][2];
    int           hud_prev_ok;
    /* 低级键盘钩子 */
    HHOOK         kbd_hook;
    volatile LONG kbd_down[256];
    volatile LONG kbd_latch[256];
    /* 玩家实体定位遗留状态位（主路径不依赖） */
    volatile float hold_x, hold_y;
    volatile int  hold_cnt;
    uintptr_t     ent_pos;
} TpContext;

static TpContext g_ctx;

/* 旧名 → g_ctx 字段（同名映射，既有引用点无需改动） */
#define g_logkit        (g_ctx.logkit)
#define g_log_path      (g_ctx.log_path)
#define g_self_dir      (g_ctx.self_dir)
#define g_cfg           (g_ctx.cfg)
#define g_unlocked      (g_ctx.unlocked)
#define g_unlock_names  (g_ctx.unlock_names)
#define g_unlock_n      (g_ctx.unlock_n)
#define g_dest_order    (g_ctx.dest_order)
#define g_dest_order_n  (g_ctx.dest_order_n)
#define g_base          (g_ctx.base)
#define g_sub           (g_ctx.sub)
#define g_player        (g_ctx.player)
#define g_a_clock       (g_ctx.a_clock)
#define g_a_getctrl     (g_ctx.a_getctrl)
#define g_a_player_vt   (g_ctx.a_player_vt)
#define g_a_sub_vt      (g_ctx.a_sub_vt)
#define g_a_cam_vt      (g_ctx.a_cam_vt)
#define g_pa            (g_ctx.pa)
#define g_pa_x          (g_ctx.pa_x)
#define g_pa_y          (g_ctx.pa_y)
#define g_pa_n          (g_ctx.pa_n)
#define g_pa_ready      (g_ctx.pa_ready)
#define g_cand          (g_ctx.cand)
#define g_cand_x        (g_ctx.cand_x)
#define g_cand_y        (g_ctx.cand_y)
#define g_cand_n        (g_ctx.cand_n)
#define g_last_day      (g_ctx.last_day)
#define g_tf_coord      (g_ctx.tf_coord)
#define g_tf_n          (g_ctx.tf_n)
#define g_ph_coord      (g_ctx.ph_coord)
#define g_ph_n          (g_ctx.ph_n)
#define g_scan_latch    (g_ctx.scan_latch)
#define g_scan_request  (g_ctx.scan_request)
#define g_scan_busy     (g_ctx.scan_busy)
#define g_menu_open     (g_ctx.menu_open)
#define g_menu_sel      (g_ctx.menu_sel)
#define g_menu_page     (g_ctx.menu_page)
#define g_rebind_row    (g_ctx.rebind_row)
#define g_rebind_armed  (g_ctx.rebind_armed)
#define tp_xig          (g_ctx.tp_xig)
#define g_pad_slot      (g_ctx.pad_slot)
#define g_pad_buttons   (g_ctx.pad_buttons)
#define g_pad_ok_shared (g_ctx.pad_ok_shared)
#define g_xig_export    (g_ctx.xig_export)
#define g_key_prev      (g_ctx.key_prev)
#define g_pad_rebind_prev (g_ctx.pad_rebind_prev)
#define g_ov_px         (g_ctx.ov_px)
#define g_ov_py         (g_ctx.ov_py)
#define g_last_input    (g_ctx.last_input)
#define g_show_pos      (g_ctx.show_pos)
#define g_tele_status   (g_ctx.tele_status)
#define g_ui_scale      (g_ctx.ui_scale)
#define g_hwnd          (g_ctx.hwnd)
#define g_menu_hwnd     (g_ctx.menu_hwnd)
#define g_hud_live      (g_ctx.hud_live)
#define g_hud_prev      (g_ctx.hud_prev)
#define g_hud_prev_ok   (g_ctx.hud_prev_ok)
#define g_kbd_hook      (g_ctx.kbd_hook)
#define g_kbd_down      (g_ctx.kbd_down)
#define g_kbd_latch     (g_ctx.kbd_latch)
#define g_hold_x        (g_ctx.hold_x)
#define g_hold_y        (g_ctx.hold_y)
#define g_hold_cnt      (g_ctx.hold_cnt)
#define g_ent_pos       (g_ctx.ent_pos)

/* ------------------------------------------------------------------ */
/* 日志                                                               */
/* ------------------------------------------------------------------ */



/* ---- 日志（shared logkit，句柄一次打开） ---- */

static void log_init(void)
{
    logkit_open(&g_logkit, g_log_path, "tp", TRUE);
}

static void log_direct(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_write(&g_logkit, "%s", buf);
}

/* ------------------------------------------------------------------ */
/* 配置                                                               */
/* ------------------------------------------------------------------ */


/* 名称 <-> 混淆编码（XOR + hex），防止手动改配置提前解锁 */
static void encode_name(const char* name, char* out)
{
    int o = 0;
    for (const char* p = name; *p; p++) {
        unsigned char c = (unsigned char)*p ^ UNLOCK_KEY;
        out[o++] = "0123456789ABCDEF"[c >> 4];
        out[o++] = "0123456789ABCDEF"[c & 15];
    }
    out[o] = 0;
}

static int decode_name(const char* hex, char* out, int maxout)
{
    int n = 0;
    for (; hex[0] && hex[1] && n < maxout - 1; hex += 2) {
        char hi = hex[0], lo = hex[1];
        int hv = (hi >= '0' && hi <= '9') ? hi - '0'
               : ((hi | 32) >= 'a' && (hi | 32) <= 'f') ? (hi | 32) - 'a' + 10 : -1;
        int lv = (lo >= '0' && lo <= '9') ? lo - '0'
               : ((lo | 32) >= 'a' && (lo | 32) <= 'f') ? (lo | 32) - 'a' + 10 : -1;
        if (hv < 0 || lv < 0) return 0;
        out[n++] = (char)(((hv << 4) | lv) ^ UNLOCK_KEY);
    }
    out[n] = 0;
    return 1;
}

static int parse_int(const char* s)
{
    int v = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void parse_dest(const char* key, const char* val)
{
    if (strnicmp(key, "Dest", 4) != 0) return;
    const char* p = key + 4;
    int idx = 0;
    while (*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
    }
    if (idx < 1 || idx > TP_MAX_DEST) return;
    idx--;
    /* 记录该目的地首次出现的顺序（菜单按此顺序显示） */
    int seen = 0;
    for (int k = 0; k < g_dest_order_n; k++) {
        if (g_dest_order[k] == idx) { seen = 1; break; }
    }
    if (!seen && g_dest_order_n < TP_MAX_DEST)
        g_dest_order[g_dest_order_n++] = idx;
    if (strnicmp(p, "Name", 4) == 0) {
        lstrcpynA(g_cfg.dests[idx].name, val, sizeof(g_cfg.dests[idx].name));
        if (idx + 1 > g_cfg.dest_count) g_cfg.dest_count = idx + 1;
    } else if (strnicmp(p, "X", 1) == 0) {
        g_cfg.dests[idx].x = (float)atof(val);
        if (idx + 1 > g_cfg.dest_count) g_cfg.dest_count = idx + 1;
    } else if (strnicmp(p, "Y", 1) == 0) {
        g_cfg.dests[idx].y = (float)atof(val);
        if (idx + 1 > g_cfg.dest_count) g_cfg.dest_count = idx + 1;
    }
}

static void read_config(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_dest_order_n = 0;
    g_cfg.enabled = 1;
    g_cfg.teleport = 1;
    g_cfg.tp_key = 117; /* F6 */
    g_cfg.hud_key = 0x77; /* F7：切换坐标 HUD */
    g_cfg.settings_key = 0xDC;  /* \ 键（中文输入法下是、） */
    g_cfg.up_key = 0x21;      /* PageUp */
    g_cfg.dn_key = 0x22;      /* PageDown */
    g_cfg.pg_key = 0xDB;      /* [ */
    g_cfg.nk_key = 0xDD;      /* ] */
    g_cfg.ok_key = 0x0D;      /* Enter */
    g_cfg.cancel_key = 0x1B;  /* Esc */
    g_cfg.pad_toggle_key   = 0x0100;  /* LB */
    g_cfg.pad_ok_key       = 0x0200;  /* RB */
    g_cfg.pad_up_key       = 0x0004;  /* ← */
    g_cfg.pad_down_key     = 0x0008;  /* → */
    g_cfg.pad_next_key     = 0x0002;  /* ↓ */
    g_cfg.pad_settings_key = 0x1000;  /* A */
    g_cfg.unlock_radius = 300.0f;
    g_cfg.hud_scale = 0.0f;

    char path[MAX_PATH];
    lstrcpyA(path, g_self_dir);
    lstrcatA(path, "teleport.txt");

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[65536];
    DWORD rd = 0;
    if (!ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL) || rd == 0) {
        CloseHandle(f);
        return;
    }
    CloseHandle(f);
    buf[rd] = 0;

    char* line = buf;
    while (line && *line) {
        char* eol = strchr(line, '\n');
        if (eol) *eol = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#' && *p != ';') {
            char* eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                char* key = p;
                char* val = eq + 1;
                while (*key == ' ' || *key == '\t') key++;
                char* ktail = key + lstrlenA(key);
                while (ktail > key && (ktail[-1] == ' ' || ktail[-1] == '\t')) ktail--;
                *ktail = 0;
                while (*val == ' ' || *val == '\t') val++;
                char* vtail = val + lstrlenA(val);
                while (vtail > val && (vtail[-1] == ' ' || vtail[-1] == '\t')) vtail--;
                *vtail = 0;
                if (lstrcmpiA(key, "Enabled") == 0) g_cfg.enabled = parse_int(val);
                else if (lstrcmpiA(key, "Teleport") == 0) g_cfg.teleport = parse_int(val);
                else if (lstrcmpiA(key, "TeleportKey") == 0) g_cfg.tp_key = parse_int(val);
                else if (lstrcmpiA(key, "HudKey") == 0) g_cfg.hud_key = parse_int(val);
                else if (lstrcmpiA(key, "SettingsKey") == 0) g_cfg.settings_key = parse_int(val);
                else if (lstrcmpiA(key, "UpKey") == 0) g_cfg.up_key = parse_int(val);
                else if (lstrcmpiA(key, "DownKey") == 0) g_cfg.dn_key = parse_int(val);
                else if (lstrcmpiA(key, "PrevPageKey") == 0) g_cfg.pg_key = parse_int(val);
                else if (lstrcmpiA(key, "NextPageKey") == 0) g_cfg.nk_key = parse_int(val);
                else if (lstrcmpiA(key, "ConfirmKey") == 0) g_cfg.ok_key = parse_int(val);
                else if (lstrcmpiA(key, "CancelKey") == 0) g_cfg.cancel_key = parse_int(val);
                else if (lstrcmpiA(key, "PadToggleKey") == 0) g_cfg.pad_toggle_key = parse_int(val);
                else if (lstrcmpiA(key, "PadOkKey") == 0) g_cfg.pad_ok_key = parse_int(val);
                else if (lstrcmpiA(key, "PadUpKey") == 0) g_cfg.pad_up_key = parse_int(val);
                else if (lstrcmpiA(key, "PadDownKey") == 0) g_cfg.pad_down_key = parse_int(val);
                else if (lstrcmpiA(key, "PadNextKey") == 0) g_cfg.pad_next_key = parse_int(val);
                else if (lstrcmpiA(key, "PadSettingsKey") == 0) g_cfg.pad_settings_key = parse_int(val);
                else if (lstrcmpiA(key, "UnlockRadius") == 0) g_cfg.unlock_radius = (float)atof(val);
                else if (lstrcmpiA(key, "DiagStalePos") == 0) g_cfg.diag_stale = parse_int(val);
                else if (lstrcmpiA(key, "HudScale") == 0) g_cfg.hud_scale = (float)atof(val);
                else if (lstrcmpiA(key, "Unlock") == 0) {
                    /* 解锁记录按名称去重；旧版只读前 16 条，重复/过时记录
                     * 会把后写入的（如山顶）挤出，导致每次都要重新解锁 */
                    char tmp[64];
                    if (decode_name(val, tmp, 64)) {
                        int dup = 0;
                        for (int k = 0; k < g_unlock_n; k++) {
                            if (lstrcmpA(g_unlock_names[k], tmp) == 0) {
                                dup = 1;
                                break;
                            }
                        }
                        if (!dup && g_unlock_n < 128)
                            lstrcpyA(g_unlock_names[g_unlock_n++], tmp);
                    }
                }
                else parse_dest(key, val);
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    /* 按名字匹配已解锁的传送点 */
    for (int k = 0; k < g_unlock_n; k++) {
        for (int i = 0; i < g_cfg.dest_count; i++) {
            if (lstrcmpA(g_cfg.dests[i].name, g_unlock_names[k]) == 0)
                g_unlocked[i] = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 解锁存档：teleport_unlock.txt 记录到访过的传送点                   */
/* ------------------------------------------------------------------ */

static void game_root_path(char* out, const char* file)
{
    lstrcpyA(out, g_self_dir);
    lstrcatA(out, file);
}

static void save_unlock(int idx)
{
    char path[MAX_PATH];
    game_root_path(path, "teleport.txt");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    char enc[256];
    encode_name(g_cfg.dests[idx].name, enc);
    WriteFile(f, "Unlock=", 7, &w, NULL);
    WriteFile(f, enc, (DWORD)lstrlenA(enc), &w, NULL);
    WriteFile(f, "\r\n", 2, &w, NULL);
    CloseHandle(f);
}

/* 判断一行是否为指定 Key= 配置（大小写不敏感） */
static int key_line_eq(const char* q, size_t qlen, const char* key)
{
    int keylen = lstrlenA(key);
    if (qlen <= (size_t)keylen) return 0;
    for (int i = 0; i < keylen; i++) {
        char a = q[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return q[keylen] == '=';
}

/* 行首是否等于指定前缀（大小写不敏感） */
static int line_starts_with(const char* q, size_t qlen, const char* prefix)
{
    size_t plen = lstrlenA(prefix);
    if (qlen < plen) return 0;
    for (size_t i = 0; i < plen; i++) {
        char a = q[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* 向 teleport.txt 写入 Key=值 配置（改键/切换选项时用）。
 * 与解锁记录不同，按键配置在文件上方维护单行：已有该键则原位替换并删除
 * 重复行；没有则插入到 Teleport= 行之后，避免像旧版那样不断追加导致文件
 * 尾部堆满历史按键记录。 */
static void save_config_int(const char* key, int val)
{
    char path[MAX_PATH];
    game_root_path(path, "teleport.txt");

    HANDLE f = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 0x40000) {
        CloseHandle(f);
        return;
    }
    char* buf = (char*)malloc(size + 1);
    if (!buf) {
        CloseHandle(f);
        return;
    }
    DWORD rd = 0;
    if (!ReadFile(f, buf, size, &rd, NULL) || rd != size) {
        free(buf);
        CloseHandle(f);
        return;
    }
    CloseHandle(f);
    buf[size] = 0;

    char line[64];
    wsprintfA(line, "%s=%d", key, val);

    char* out = (char*)malloc(size + 64);
    if (!out) {
        free(buf);
        return;
    }
    size_t o = 0;
    int replaced = 0, inserted = 0;

    char* p = buf;
    while (p && *p) {
        char* eol = strchr(p, '\n');
        size_t tlen = eol ? (size_t)(eol - p) : lstrlenA(p);
        while (tlen > 0 && p[tlen - 1] == '\r') tlen--; /* 去掉行尾 \r */
        const char* q = p;
        while (q < p + tlen && (*q == ' ' || *q == '\t')) q++;
        size_t qlen = (size_t)(p + tlen - q);

        int is_key = 0;
        if (key_line_eq(q, qlen, key)) {
            if (!replaced) {
                memcpy(out + o, line, lstrlenA(line));
                o += lstrlenA(line);
                replaced = 1;
            }
            is_key = 1; /* 重复的旧行直接删除 */
        }

        if (!is_key) {
            memcpy(out + o, p, tlen);
            o += tlen;
            /* 键不存在时：插到 Teleport= 行之后 */
            if (!inserted && !replaced &&
                line_starts_with(q, qlen, "Teleport=")) {
                memcpy(out + o, line, lstrlenA(line));
                o += lstrlenA(line);
                inserted = 1;
            }
        }
        out[o++] = '\n';
        p = eol ? eol + 1 : NULL;
    }
    if (!replaced && !inserted) {
        memcpy(out + o, line, lstrlenA(line));
        o += lstrlenA(line);
        out[o++] = '\n';
    }

    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, out, (DWORD)o, &w, NULL);
        CloseHandle(f);
    }
    free(out);
    free(buf);
}

/* 虚拟键码 -> 简短英文名称（用于菜单显示） */
static void key_name(int vk, char* out, int max)
{
    static const struct { int vk; const char* n; } named[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" },     { 0x0D, "Enter" },
        { 0x1B, "Esc" },       { 0x20, "Space" },   { 0x21, "PgUp" },
        { 0x22, "PgDn" },      { 0x23, "End" },     { 0x24, "Home" },
        { 0x25, "Left" },      { 0x26, "Up" },      { 0x27, "Right" },
        { 0x28, "Down" },      { 0x2D, "Insert" },  { 0x2E, "Delete" },
        { 0x60, "Num0" },      { 0x61, "Num1" },    { 0x62, "Num2" },
        { 0x63, "Num3" },      { 0x64, "Num4" },    { 0x65, "Num5" },
        { 0x66, "Num6" },      { 0x67, "Num7" },    { 0x68, "Num8" },
        { 0x69, "Num9" },      { 0x6A, "Num*" },    { 0x6B, "Num+" },
        { 0x6D, "Num-" },      { 0x6E, "Num." },    { 0x6F, "Num/" },
        { 0xBA, ";" },         { 0xBB, "=" },       { 0xBC, "," },
        { 0xBD, "-" },         { 0xBE, "." },       { 0xBF, "/" },
        { 0xC0, "`" },         { 0xDB, "[" },       { 0xDC, "、" },
        { 0xDD, "]" },
    };
    if (vk >= 0x70 && vk <= 0x87) {        /* F1-F24 */
        wsprintfA(out, "F%d", vk - 0x70 + 1);
        return;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = (char)vk;
        out[1] = 0;
        return;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (named[i].vk == vk) {
            lstrcpynA(out, named[i].n, max);
            return;
        }
    }
    wsprintfA(out, "Key%d", vk);
}

static void key_name_w(int vk, WCHAR* out, int max)
{
    char a[24];
    key_name(vk, a, sizeof(a));
    MultiByteToWideChar(CP_UTF8, 0, a, -1, out, max);
}

#ifndef TP_KEYBOARD_ONLY
/* 手柄按钮位 -> 名称 */
static void pad_key_name(int b, WCHAR* out, int max)
{
    switch (b) {
    case 0x0001: wcscpy(out, L"↑"); break;
    case 0x0002: wcscpy(out, L"↓"); break;
    case 0x0004: wcscpy(out, L"←"); break;
    case 0x0008: wcscpy(out, L"→"); break;
    case 0x0010: wcscpy(out, L"Start"); break;
    case 0x0020: wcscpy(out, L"Back"); break;
    case 0x0100: wcscpy(out, L"LB"); break;
    case 0x0200: wcscpy(out, L"RB"); break;
    case 0x1000: wcscpy(out, L"A"); break;
    case 0x2000: wcscpy(out, L"B"); break;
    case 0x4000: wcscpy(out, L"X"); break;
    case 0x8000: wcscpy(out, L"Y"); break;
    default: swprintf(out, max, L"键%d", b); break;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* 安全内存读取 / 玩家定位                                             */
/* ------------------------------------------------------------------ */

static BOOL safe_read(void* dst, const void* src, size_t n);
static int is_heap_ptr(uintptr_t v);

/* ------------------------------------------------------------------ */
/* 运行时地址自动解析（签名掩码位移，跨版本免手改）                    */
/* ------------------------------------------------------------------ */


/* 签名表：由 dev_backup\analysis\probe_teleport_sigs.py 从 1.08 exe 生成，
 * 掩码掉全部位移类字节；1.08.1 实测 7/7 唯一命中。 */
static const AddrSig g_addr_sigs[] = {
    { "getctrl_0x4A8BD0", ADDRSIG_FUNC,
      "4053b850100000e800000000482be0488bd9448b491c41ffc14489491c443b49180f8f000000004c8b5128418bd1498b",
      "010101010101010100000000010101010101010101010101010101010101010101010100000000010101010101010101", 0 },
    { "clock_0x10CD960", ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
    { "player_vt_0xE10380", ADDRSIG_RIPGLOBAL,
      "83f801750b488b01ba01000000ff1090488d0500000000488903488d050000000048894310488d",
      "010101010101010101010101010101010101010000000001010101010100000000010101010101", 19 },
    { "player_vt_0xE10218", ADDRSIG_RIPGLOBAL,
      "01750b488b01ba01000000ff1090488d0500000000488903488d050000000048894310488d050000000048894320488d05000000004889",
      "01010101010101010101010101010101010000000001010101010100000000010101010101010000000001010101010101000000000101", 27 },
    { "player_vt_0xE10208", ADDRSIG_RIPGLOBAL,
      "ff1090488d0500000000488903488d050000000048894310488d050000000048894320488d050000000048898348020000488b06488983",
      "01010101010100000000010101010101000000000101010101010100000000010101010101010000000001010101010101010101010101", 27 },
    { "player_vt_0xE10370", ADDRSIG_RIPGLOBAL,
      "0048894310488d050000000048894320488d050000000048898348020000488b06488983300400",
      "010101010101010100000000010101010101010000000001010101010101010101010101010101", 19 },
    { "sub_vt_0xE4D288", ADDRSIG_RIPGLOBAL,
      "488bf2488bf948894c242833ed896908488d0500000000488901488b05000000004883c0014889",
      "010101010101010101010101010101010101010000000001010101010100000000010101010101", 19 },
    { "cam_vt_0xE54C40", ADDRSIG_RIPGLOBAL,
      "1a98ff48890048894008488983b80100004889bbc8010000488d0500000000488903488d050000000048894310488d0500000000488943",
      "01010101010101010101010101010101010101010101010101010100000000010101010101000000000101010101010100000000010101", 27 },
};

static void addr_init(void)
{
    static AddrRes res[sizeof(g_addr_sigs) / sizeof(g_addr_sigs[0])];
    addrsig_resolve(g_addr_sigs, (int)(sizeof(g_addr_sigs) / sizeof(g_addr_sigs[0])),
                    g_base, res, log_direct);
    for (size_t i = 0; i < sizeof(g_addr_sigs) / sizeof(g_addr_sigs[0]); i++) {
        if (!res[i].hit) continue;
        if (strcmp(res[i].name, "getctrl_0x4A8BD0") == 0)
            g_a_getctrl = res[i].addr;
        else if (strcmp(res[i].name, "clock_0x10CD960") == 0)
            g_a_clock = res[i].addr;
        else if (strcmp(res[i].name, "player_vt_0xE10380") == 0)
            g_a_player_vt[0] = res[i].addr;
        else if (strcmp(res[i].name, "player_vt_0xE10218") == 0)
            g_a_player_vt[1] = res[i].addr;
        else if (strcmp(res[i].name, "player_vt_0xE10208") == 0)
            g_a_player_vt[2] = res[i].addr;
        else if (strcmp(res[i].name, "player_vt_0xE10370") == 0)
            g_a_player_vt[3] = res[i].addr;
        else if (strcmp(res[i].name, "sub_vt_0xE4D288") == 0)
            g_a_sub_vt = res[i].addr;
        else if (strcmp(res[i].name, "cam_vt_0xE54C40") == 0)
            g_a_cam_vt = res[i].addr;
    }
}

/* 扫描玩家实体内部找碰撞体子对象：
 * 1) 内嵌：实体内部某偏移直接是 vptr == sub 虚表；
 * 2) 指针：实体内部某偏移是指向碰撞体（vptr == sub 虚表）的堆指针。 */
static uintptr_t find_sub_in_entity(uintptr_t ent)
{
    if (!g_a_sub_vt) return 0;
    for (size_t off = 0; off + 8 <= 0x1000; off += 8) {
        uintptr_t v = 0;
        if (!safe_read(&v, (const void*)(ent + off), 8)) break;
        if (v == g_a_sub_vt)
            return ent + off;
    }
    for (size_t off = 0; off + 8 <= 0x1000; off += 8) {
        uintptr_t p = 0;
        if (!safe_read(&p, (const void*)(ent + off), 8)) break;
        if (!is_heap_ptr(p)) continue;
        uintptr_t vv = 0;
        if (!safe_read(&vv, (const void*)p, 8)) continue;
        if (vv == g_a_sub_vt)
            return p;   /* 碰撞体对象地址 */
    }
    return 0;
}

/* 找玩家实体：vptr == 玩家主虚表（CCom_Unit_Player），取堆地址。 */

/* 找玩家控制器：对象头虚表在 .rdata 且 [obj+0x318] == 玩家实体。
 * 原生链：FUN_14010F9D0(A) -> [[A+0x208]+8] = C -> FUN_1407223F0(C, pos)。 */

/* 找相机对象：vptr == 相机虚表且 [obj+0x318] = 实体、实体+0x230 坐标合理。
 * 玩家画面位置 = [实体+0x230]（FUN_1407245A0 返回它给相机，CE 验证跟随玩家）。 */

/* 农场出门点：玩家 x 恒为 12180（实测），y 因小屋等级而异。
 * x 误差 20、y 误差 100（21225~21425），先设大后面好优化。 */
#define FARM_X 12180.0f
#define FARM_XE 10.0f
#define FARM_Y1 21225.0f
#define FARM_Y2 21425.0f
/* 收敛候选：首次全内存扫描只记录地址+初始值，随后循环只读这些地址，
 * 值仍精确等于初始值的保留（活副本），值已变化的剔除（污染/失效地址）。 */
#define CONVERGE_ROUNDS 30   /* 连续无剔除轮数达到即收敛完成（16ms/轮，约 0.5s，覆盖 30 帧） */

/* Alt+F6 手动定位状态机：
 *   kbd_hook        识别组合 -> g_scan_latch（吞键，不记 F6 状态）
 *   teleport_thread 收口（关菜单）-> g_scan_request
 *   birth_scan_thread 消费 -> 执行单次扫描/收敛 -> g_scan_busy 门禁
 * 固定 F6(0x75)，不随 TeleportKey 改键。 */
#define TP_SCAN_VK 0x75

/* 全内存扫描：记录落在农场出门点范围内的所有坐标对地址及初始值 */
static void scan_birth_candidates(void)
{
    g_cand_n = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* addr = (BYTE*)si.lpMinimumApplicationAddress;
    while ((uintptr_t)addr < (uintptr_t)si.lpMaximumApplicationAddress &&
           g_cand_n < 64) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
            BYTE buf[0x40000];
            size_t pos = 0;
            while (pos < mbi.RegionSize && g_cand_n < 64) {
                size_t chunk = mbi.RegionSize - pos;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!safe_read(buf, (const BYTE*)mbi.BaseAddress + pos, chunk))
                    break;
                for (size_t i = 0; i + 8 <= chunk && g_cand_n < 64; i += 4) {
                    float x, y;
                    memcpy(&x, buf + i, 4);
                    memcpy(&y, buf + i + 4, 4);
                    if (fabsf(x - FARM_X) <= FARM_XE &&
                        y >= FARM_Y1 && y <= FARM_Y2) {
                        uintptr_t at = (uintptr_t)mbi.BaseAddress + pos + i;
                        HMODULE hm = NULL;
                        if (GetModuleHandleExA(
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)at, &hm) && hm)
                            continue;
                        g_cand[g_cand_n] = at;
                        g_cand_x[g_cand_n] = x;
                        g_cand_y[g_cand_n] = y;
                        g_cand_n++;
                    }
                }
                pos += chunk;
            }
        }
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    if (g_cand_n > 0)
        log_direct("[tp] farm-point candidates: %d", g_cand_n);
}

/* 收敛：只读候选地址，值精确等于初始记录的保留，变化的剔除 */
static int converge_birth_copies(void)
{
    int keep = 0;
    for (int i = 0; i < g_cand_n; i++) {
        float x, y;
        if (safe_read(&x, (const void*)g_cand[i], 4) &&
            safe_read(&y, (const void*)(g_cand[i] + 4), 4) &&
            x == g_cand_x[i] && y == g_cand_y[i]) {
            if (keep != i) {
                g_cand[keep] = g_cand[i];
                g_cand_x[keep] = g_cand_x[i];
                g_cand_y[keep] = g_cand_y[i];
            }
            keep++;
        }
    }
    g_cand_n = keep;
    return keep;
}

/* 当前游戏天数：[[时钟全局]+0x208]+0x3270 累计秒 ÷ 86400 */
static unsigned long long read_day(void)
{
    uintptr_t p = 0, q = 0;
    uint64_t t = 0;
    if (!g_a_clock ||
        !safe_read(&p, (const void*)g_a_clock, 8) || !is_heap_ptr(p))
        return 0;
    if (!safe_read(&q, (const void*)(p + 0x208), 8) || !is_heap_ptr(q))
        return 0;
    if (!safe_read(&t, (const void*)(q + 0x3270), 8))
        return 0;
    return (unsigned long long)(t / 86400ULL);
}

/* 角色控制器（A）：复刻狗带回家 TLS 链
 * GS:[0x58] -> *ptr -> [ptr+0x56c8] 索引 -> [ptr+idx*8+0x5a20-8] 管理器
 * -> FUN_1404A8BD0(管理器) = A（FUN_14010F9D0 的 RCX） */

/* 移动检测：全扫坐标副本（碰撞体+0x3D0 / CObject+0x230，带小数），
 * 等玩家移动后重读，坐标变化的那组 = 玩家（NPC 静止不动）。 */

/* 重读快照：返回变化副本数，并把玩家组坐标写入 ox/oy。
 * 玩家移动时只有玩家组变；返回玩家组首地址。 */

/* 玩家坐标定位：全内存扫所有 float 对，统计精确相等的频率。
 * 玩家坐标副本恰好 9 个精确相等（CE 实测，任意存档通用）；
 * 无关重复数据（104 个）和 NPC 组（3-4 个）都被排除。
 * 找到后返回副本地址列表（最多 32 个）。 */
static int find_player_copies(uintptr_t* out, int maxout, float* ox, float* oy)
{
    static float px[64], py[64];
    static int pcnt[64];
    int pn = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* addr = (BYTE*)si.lpMinimumApplicationAddress;
    while ((uintptr_t)addr < (uintptr_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY))) {
            BYTE buf[0x40000];
            size_t pos = 0;
            while (pos < mbi.RegionSize) {
                size_t chunk = mbi.RegionSize - pos;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!safe_read(buf, (const BYTE*)mbi.BaseAddress + pos, chunk))
                    break;
                for (size_t i = 0; i + 8 <= chunk; i += 4) {
                    float x, y;
                    memcpy(&x, buf + i, 4);
                    memcpy(&y, buf + i + 4, 4);
                    if (x <= 1.0f || x >= 1000000.0f ||
                        y <= 1.0f || y >= 1000000.0f)
                        continue;
                    int k;
                    for (k = 0; k < pn; k++) {
                        if (px[k] == x && py[k] == y) {
                            pcnt[k]++;
                            break;
                        }
                    }
                    if (k == pn && pn < 64) {
                        px[pn] = x;
                        py[pn] = y;
                        pcnt[pn] = 1;
                        pn++;
                    }
                }
                pos += chunk;
            }
        }
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    int best = -1, bestn = 0;
    for (int k = 0; k < pn; k++) {
        if (pcnt[k] >= 7 && pcnt[k] <= 12 && pcnt[k] > bestn) {
            bestn = pcnt[k];
            best = k;
        }
    }
    /* 诊断：打印频率最高的 6 个值（含被过滤的） */
    for (int a = 0; a < 6 && a < pn; a++) {
        int bi = -1, bn = 0;
        for (int k = 0; k < pn; k++) {
            if (pcnt[k] > bn) {
                bn = pcnt[k];
                bi = k;
            }
        }
        if (bi < 0) break;
        log_direct("[tp] freq[%d]: (%.2f, %.2f) x%d",
                a, px[bi], py[bi], pcnt[bi]);
        pcnt[bi] = 0;
    }
    if (best >= 0) {
        *ox = px[best];
        *oy = py[best];
        /* 再扫一遍收集该坐标的所有地址 */
        int n = 0;
        SYSTEM_INFO si2;
        GetSystemInfo(&si2);
        BYTE* a2 = (BYTE*)si2.lpMinimumApplicationAddress;
        while ((uintptr_t)a2 < (uintptr_t)si2.lpMaximumApplicationAddress &&
               n < maxout) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(a2, &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
                (mbi.Protect & PAGE_GUARD) == 0 &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY))) {
                BYTE buf[0x40000];
                size_t pos = 0;
                while (pos < mbi.RegionSize && n < maxout) {
                    size_t chunk = mbi.RegionSize - pos;
                    if (chunk > sizeof(buf)) chunk = sizeof(buf);
                    if (!safe_read(buf, (const BYTE*)mbi.BaseAddress + pos, chunk))
                        break;
                    for (size_t i = 0; i + 8 <= chunk && n < maxout; i += 4) {
                        float a, b;
                        memcpy(&a, buf + i, 4);
                        memcpy(&b, buf + i + 4, 4);
                        if (a == px[best] && b == py[best]) {
                            uintptr_t at = (uintptr_t)mbi.BaseAddress + pos + i;
                            HMODULE hm = NULL;
                            if (GetModuleHandleExA(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)at, &hm) && hm)
                                continue;
                            out[n++] = at;
                        }
                    }
                    pos += chunk;
                }
            }
            a2 = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        }
        log_direct("[tp] player copies (%d, %d addrs) @ (%.2f,%.2f)",
                bestn, n, *ox, *oy);
        return n;
    }
    return 0;
}

/* 检测是否进入存档：游戏时间 [[时钟全局]+0x208]+0x3270（gameTime_.second_）。
 * 标题界面为 0；进存档后累计（Automate 实测 Day149=12873600）。 */

static BOOL safe_read(void* dst, const void* src, size_t n);
static int is_heap_ptr(uintptr_t v);

/* g_player 已在文件前部声明 */
#ifndef TP_KEYBOARD_ONLY
/* v2.1.4：手柄状态由独立线程读取（1.08.1 下 XInput 调用可能卡死
 * teleport_thread，导致 F6 轮询停摆、菜单打不开）。 */
static DWORD WINAPI pad_read_thread(LPVOID param);

/* ---- XInput 屏蔽：菜单打开时让游戏拿不到手柄输入 ---- */
#define TP_HOOK_LEN 14

/* 菜单打开时：返回"手柄已连接但无按键"；否则转发真实读取 */

/* 对 XInputGetState 导出做 inline detour：入口跳转到 tp_xig_detour，
 * 原函数头部复制到 trampoline 供绕过 hook 使用。
 * 注意：hook 系统 XInput 曾在启动时导致游戏闪退，当前已停用，
 * 待找到更稳定的拦截方案后再启用。 */

static void tp_load_xinput(void)
{
    static const char* names[] = { "xinput1_4.dll", "xinput1_3.dll",
                                   "xinput9_1_0.dll" };
    for (int i = 0; i < 3; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (!m) continue;
        BYTE* fn = (BYTE*)GetProcAddress(m, "XInputGetState");
        if (fn) {
            tp_xig = (TP_XIGETSTATE)fn;
            g_xig_export = fn;   /* 延迟安装由 teleport 线程执行 */
            log_direct("[tp] xinput: %s", names[i]);
            return;
        }
    }
    log_direct("[tp] xinput: not available (手柄不可用)");
}

/* 游戏启动约 3 秒后安装拦截，避开启动早期 XInput 初始化竞态 */
#endif

#ifndef TP_KEYBOARD_ONLY
#endif

#ifdef TP_SD_SCALE
/* SD 版：按屏幕分辨率计算界面缩放，以 1920x1080 为基准，取宽/高比例
 * 较小者，上限 1.0（高分屏不放大），下限 0.5；HudScale 配置 >0 时手动覆盖。 */
static void ui_scale_init(void)
{
    if (g_cfg.hud_scale > 0.001f) {
        g_ui_scale = g_cfg.hud_scale;
        if (g_ui_scale < 0.4f) g_ui_scale = 0.4f;
        if (g_ui_scale > 2.0f) g_ui_scale = 2.0f;
        log_direct("[tp] hud scale %.2f (manual)", g_ui_scale);
        return;
    }
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float s = 1.0f;
    if (sw > 0 && sh > 0) {
        float a = (float)sw / 1920.0f;
        float b = (float)sh / 1080.0f;
        s = a < b ? a : b;
        if (s > 1.0f) s = 1.0f;
        if (s < 0.5f) s = 0.5f;
    }
    g_ui_scale = s;
    log_direct("[tp] hud scale %.2f (screen %dx%d)", s, sw, sh);
}
#else
/* 桌面版：界面固定原尺寸，不缩放 */
static void ui_scale_init(void)
{
    g_ui_scale = 1.0f;
}
#endif

/* 状态提示时 HUD 窗口移到屏幕顶部居中，状态结束回左上角 */
static void set_hud_pos(void)
{
    if (!g_hwnd) return;
    float s = g_ui_scale;
    int x = (int)(8 * s), y = (int)(8 * s);
    int w = (int)(400 * s), h = (int)(190 * s);
    if (g_tele_status != 0) {
        w = (int)(900 * s);
        h = (int)(200 * s);
        x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        y = (int)(8 * s);
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* 菜单打开时把窗口移到屏幕/游戏窗口中心，关闭时回到左上角 */
static void overlay_recenter(int center)
{
    if (!g_hwnd) return;
    log_direct("[tp] recenter %d", center);
    float s = g_ui_scale;
    /* 提示/HUD 窗口固定在左上角，不抢焦点 */
    SetWindowPos(g_hwnd, HWND_TOPMOST, (int)(8 * s), (int)(8 * s), 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    /* 中间菜单窗口：打开时居中显示，关闭时隐藏 */
    if (g_menu_hwnd) {
        if (center) {
#ifdef TP_SD_SCALE
            int mw = (int)(210 * s), mh = (int)(300 * s);
#else
            int mw = 180, mh = 300;
#endif
            int x = (int)(8 * s), y = (int)(8 * s);
            HWND gw = GetForegroundWindow();
            RECT gr;
            if (gw && GetClientRect(gw, &gr)) {
                POINT p = { 0, 0 };
                ClientToScreen(gw, &p);
                x = p.x + (gr.right - gr.left - mw) / 2;
                y = p.y + (gr.bottom - gr.top - mh) / 2;
            } else {
                x = (GetSystemMetrics(SM_CXSCREEN) - mw) / 2;
                y = (GetSystemMetrics(SM_CYSCREEN) - mh) / 2;
            }
            ShowWindow(g_menu_hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(g_menu_hwnd, HWND_TOPMOST, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g_menu_hwnd, SW_HIDE);
        }
    }
}

static BOOL safe_read(void* dst, const void* src, size_t n)
{
    SIZE_T rd = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &rd) && rd == n;
}

/* 防崩写入：页面可写校验 + SEH 异常包裹 + 读回校验。
 * 返回 1=写且读回一致；2=已写入但读回不一致（游戏并发改写，罕见）；
 * 0=页面不可写/访问违例（跳过，不崩）。 */
static int safe_write(uintptr_t addr, const void* src, size_t n)
{
    if (!addr || n == 0 || n > 16) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return 0;
    int ok = 0;
    __try {
        memcpy((void*)addr, src, n);
        ok = 1;
    } __except (1) {
        ok = 0;
    }
    if (!ok) return 0;
    unsigned char back[16];
    if (safe_read(back, (const void*)addr, n) && memcmp(back, src, n) == 0)
        return 1;
    return 2;
}

static int is_heap_ptr(uintptr_t v)
{
    return (v >= 0x100000000ull && v < 0x7FFF00000000ull);
}

/* ------------------------------------------------------------------ */

/* 玩家对象是否仍有效（轻量校验：子对象指针 + 坐标合理性） */

/* F6 强制重扫：清空 g_player 后全内存扫描重新定位（玩家碰撞体可能
 * 在存档加载后才创建，首次扫描会漏掉）。 */

/* 周期校验玩家对象（玩家可能在换图/读档后被重建）。
 * 注意：全内存扫描很重（一次约 1~2 秒），不能每 3 秒无条件扫——
 * 会让 QoL 镰刀收割等重操作掉帧。只在对象丢失/失效时才全扫。 */

/* 独立线程：按需执行出生点副本定位（Alt+F6 触发单次扫描，不自动重扫）。
 * 已就绪后只做跨日失效检测：day 变化即清空 g_pa 等待玩家手动重扫，
 * 避免把上一日已失效的副本地址写入内存。 */
static DWORD WINAPI birth_scan_thread(LPVOID param)
{
    (void)param;
    Sleep(2000);
    for (;;) {
        if (g_pa_ready && !g_scan_request) {
            /* 已收敛：仅跨日失效检测；day 变化不自动重扫 */
            unsigned long long day = read_day();
            if (day != g_last_day && day != 0) {
                g_last_day = day;
                log_direct("[tp] day changed -> g_pa invalidated, "
                        "press Alt+F6 at farm door");
                g_pa_ready = 0;
                g_pa_n = 0;
                g_cand_n = 0;
                g_tele_status = 1;
                set_hud_pos();
            }
            Sleep(1000);
            continue;
        }
        /* 未就绪：等待 Alt+F6，不自动扫描 */
        if (!g_scan_request) {
            if (g_tele_status != 1) {
                g_tele_status = 1;
                set_hud_pos();
            }
            Sleep(200);
            continue;
        }

        /* ---- 单次手动定位：全内存扫描 + 收敛，失败不自动重试 ---- */
        g_scan_request = 0;
        g_scan_busy = 1;
        int prev_status = g_tele_status;
        g_tele_status = 3;          /* 定位扫描中 */
        set_hud_pos();
        log_direct("[tp] manual scan started (stand still at farm door)");
        g_cand_n = 0;
        scan_birth_candidates();
        int converged = 0;
        if (g_cand_n >= 7) {
            int stable = 0;
            for (;;) {
                int before = g_cand_n;
                converge_birth_copies();
                if (g_cand_n != before) {
                    stable = 0;   /* 有剔除，重新计稳定轮 */
                    if (g_cand_n < 7) break;
                } else {
                    stable++;
                }
                if (stable >= CONVERGE_ROUNDS) {
                    converged = 1;
                    break;
                }
                Sleep(16);   /* 一帧一轮（游戏 60fps），定位期间请勿移动 */
            }
        }
        if (converged) {
            /* 收敛完成：填充 g_pa，覆盖旧副本 */
            g_pa_n = 0;
            for (int i = 0; i < g_cand_n; i++) {
                g_pa[g_pa_n] = g_cand[i];
                g_pa_x[g_pa_n] = g_cand_x[i];
                g_pa_y[g_pa_n] = g_cand_y[i];
                g_pa_n++;
            }
            g_pa_ready = 1;
            g_tele_status = 2;
            set_hud_pos();
            g_last_day = read_day();
            log_direct("[tp] farm-point copies: %d (converged %d rounds) ready=1",
                    g_pa_n, CONVERGE_ROUNDS);
            for (int i = 0; i < g_pa_n; i++)
                log_direct("[tp]   copy x=%p y=%p (%.2f, %.2f)",
                        (void*)g_pa[i], (void*)(g_pa[i] + 4),
                        g_pa_x[i], g_pa_y[i]);
            log_direct("[tp] 已找到玩家坐标");
        } else {
            /* 失败：不在门口范围/候选不足/定位中移动。
             * 不覆盖旧副本、不自动重试，等下一次 Alt+F6。 */
            g_cand_n = 0;
            g_tele_status = g_pa_ready ? prev_status : 1;
            set_hud_pos();
            log_direct("[tp] manual scan failed: not at farm door or moved");
        }
        g_scan_busy = 0;
    }
    return 0;
}

/* 更新 HUD 数值：玩家坐标。
 * 收敛得到的 g_pa 可能混入不随玩家变化的静止副本（值合理但不变），
 * 不能无脑读第一个。跨帧比较副本值变化，识别"随玩家移动同步变化"的
 * 活副本组；站立不动时沿用已确认的活副本；均不可用时退回众数
 * （值一致副本最多的组）。 */
#define HUD_MOVE_EPS 0.5f

static void overlay_update_state(void)
{
    float cx[64], cy[64];
    int cok[64];
    int n = g_pa_n;
    if (n > 64) n = 64;

    for (int i = 0; i < n; i++) {
        cok[i] = 0;
        if (safe_read(&cx[i], (const void*)g_pa[i], 4) &&
            safe_read(&cy[i], (const void*)(g_pa[i] + 4), 4) &&
            cx[i] > 1.0f && cx[i] < 1000000.0f &&
            cy[i] > 1.0f && cy[i] < 1000000.0f)
            cok[i] = 1;
    }

    /* 1) 同步移动组：本次相对上轮坐标变化、且 >=2 份值一致的副本组 */
    int pick = -1, pickc = 0;
    for (int i = 0; i < n; i++) {
        if (!cok[i]) continue;
        int moved = 0;
        if (g_hud_prev_ok &&
            (fabsf(cx[i] - g_hud_prev[i][0]) > HUD_MOVE_EPS ||
             fabsf(cy[i] - g_hud_prev[i][1]) > HUD_MOVE_EPS))
            moved = 1;
        if (!moved) continue;
        int c = 0;
        for (int j = 0; j < n; j++)
            if (cok[j] && fabsf(cx[j] - cx[i]) <= HUD_MOVE_EPS &&
                fabsf(cy[j] - cy[i]) <= HUD_MOVE_EPS)
                c++;
        if (c >= 2 && c > pickc) { pick = i; pickc = c; }
    }

    if (pick >= 0) {
        g_hud_live = pick;
        g_ov_px = cx[pick];
        g_ov_py = cy[pick];
    } else if (g_hud_live >= 0 && g_hud_live < n && cok[g_hud_live]) {
        /* 站立不动：沿用已确认的活副本（当前位置） */
        g_ov_px = cx[g_hud_live];
        g_ov_py = cy[g_hud_live];
    } else {
        /* 兜底：众数 */
        int mc = 0;
        g_ov_px = 0.0f;
        g_ov_py = 0.0f;
        for (int i = 0; i < n; i++) {
            if (!cok[i]) continue;
            int c = 0;
            for (int j = 0; j < n; j++)
                if (cok[j] && fabsf(cx[j] - cx[i]) <= HUD_MOVE_EPS &&
                    fabsf(cy[j] - cy[i]) <= HUD_MOVE_EPS)
                    c++;
            if (c > mc) { mc = c; g_ov_px = cx[i]; g_ov_py = cy[i]; }
        }
    }

    /* 记录本轮快照供下轮移动检测 */
    for (int i = 0; i < n; i++) {
        g_hud_prev[i][0] = cx[i];
        g_hud_prev[i][1] = cy[i];
    }
    g_hud_prev_ok = 1;

    /* 靠近过的传送点解锁 */
    if (g_ov_px > 1.0f) {
        float r = g_cfg.unlock_radius;
        for (int i = 0; i < g_cfg.dest_count; i++) {
            if (g_unlocked[i]) continue;
            float dx = g_cfg.dests[i].x - g_ov_px;
            float dy = g_cfg.dests[i].y - g_ov_py;
            if (dx * dx + dy * dy < r * r) {
                g_unlocked[i] = 1;
                log_direct("[tp] unlocked destination: %s",
                        g_cfg.dests[i].name);
                save_unlock(i);
            }
        }
    }
}

/* 菜单打开时吞掉 方向键/回车/Esc，防止游戏同时响应。
 * 注意：低级键盘钩子回调先于按键异步状态更新，被吞掉的键
 * GetAsyncKeyState 从任何线程都读不到（已实测确认），
 * 所以钩子同时自记按键状态，菜单线程改读 g_kbd_down（见 kbd_pressed）。 */

/* Alt 是否按住：左右 Alt 的虚拟键码可能由钩子上报为 VK_MENU/VK_LMENU/
 * VK_RMENU，三码全查；GetAsyncKeyState 兜底（Alt 本身不被吞键）。 */
static int kbd_alt_down(void)
{
    if (g_kbd_down[VK_MENU] || g_kbd_down[VK_LMENU] ||
        g_kbd_down[VK_RMENU])
        return 1;
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

static LRESULT CALLBACK kbd_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT* kh = (KBDLLHOOKSTRUCT*)lp;
        int vk = (int)kh->vkCode & 0xFF;
        /* Alt+F6：手动开始定位。组合键由钩子吞掉（不记 F6 按下状态、
         * 不透传给游戏），避免误开传送菜单或触发游戏自身 F6。 */
        if (g_cfg.teleport &&
            (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) &&
            vk == TP_SCAN_VK && kbd_alt_down()) {
            if (!g_scan_busy && !g_scan_request) {
                InterlockedExchange((volatile LONG*)&g_scan_latch, 1);
                log_direct("[tp] Alt+F6: manual scan requested");
            }
            return 1;
        }
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            InterlockedExchange(&g_kbd_down[vk], 1);
            InterlockedExchange(&g_kbd_latch[vk], 1);
            if (vk == g_cfg.tp_key || vk == g_cfg.hud_key)
                log_direct("[tp] kbd key=%d down", vk);
        } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            InterlockedExchange(&g_kbd_down[vk], 0);
        }
        if (g_menu_open && g_cfg.teleport) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                if (g_rebind_row >= 0)
                    return 1; /* 改键等待期间吞掉所有按键，避免游戏误响应 */
                if (vk == g_cfg.tp_key || vk == g_cfg.settings_key ||
                    vk == g_cfg.up_key || vk == g_cfg.dn_key ||
                    vk == g_cfg.pg_key || vk == g_cfg.nk_key ||
                    vk == g_cfg.ok_key || vk == g_cfg.cancel_key)
                    return 1;
            }
        }
    }
    return CallNextHookEx(g_kbd_hook, code, wp, lp);
}

/* 菜单线程读键：三保险——钩子按下边沿锁存 / 钩子按住状态 / GetAsyncKeyState。
 * 钩子正常时被吞掉的键靠前两条读到；钩子被系统摘除时第三条仍可用。 */
static int kbd_pressed(int vk)
{
    if (vk < 0 || vk > 255)
        return 0;
    if (InterlockedExchange(&g_kbd_latch[vk], 0))
        return 1;
    if (g_kbd_down[vk])
        return 1;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* ------------------------------------------------------------------ */
/* 传送                                                               */
/* ------------------------------------------------------------------ */

/* 玩家坐标“实时副本”精确同步。
 *
 * 2026-08-20 用 CE 实测：玩家世界坐标只有以下实时副本（锁定后玩家无法
 * 移动），全部可通过固定虚表 RVA（相对 village.exe 基址）重新定位：
 *   1) 玩家子对象 sub + PLAYER_POS_OFF（sub+560，主坐标）；
 *   2) 相机/物理副本：虚表（Ngf::CObject vftable，运行时解析），
 *      虚表指针位于对象+0，坐标在对象+0x230，共 2~4 份。
 *
 * 之前用“全内存扫描任何等于旧坐标的 float 对”同步，会把恰好等于坐标的
 * 无关数据也改写（小坐标传送点如深山/山顶会命中 70~287 处，还曾误改过
 * Teleport 自身的目的地数组）。这里改为按虚表类型定位，只写上述实时副本，
 * 外加玩家对象/子对象内部的其它副本（见 do_teleport_to）。
 */
#define TP_POS_EPS 0.5f /* 副本坐标允许的浮点误差 */

/* 联动定位玩家：先扫相机/物理副本（CObject+0x230），再找坐标一致的
 * 碰撞体（CCollision2DBody+0x3D0）——玩家是唯一被相机跟随的碰撞体。 */


/* 定位玩家变换/物理坐标副本（只缓存坐标 == 给定位置的实例，最多各 2 份）。
 * reset=1 先清空；reset=0 追加（供补定位，按地址去重）。 */

/* 诊断：全内存扫描仍等于旧坐标的 float 对，并标注所在模块 */

/* 把 buf 范围内所有等于 (ox,oy) 的 float 对更新为 (nx,ny)。
 * 只用于玩家对象/子对象自己的内存窗口（0x1000），不碰无关数据。 */

/* 全内存扫描：所有等于 (ox,oy) 的 float 对改为 (nx,ny)。
 * 只写堆地址（>=0x100000000），跳过本模块（Teleport 自身目的地数组），
 * 跳过栈（低地址临时变量），最多 40 处防误伤。 */

/* 全内存扫描：所有"对象头虚表在 .rdata 且 +0x230 == (ox,oy)"的对象，
 * 把 +0x230 位置写为 (nx,ny)。玩家角色（相机跟随源）可能用任意虚表，
 * 按位置特征定位最可靠。 */


/* 通过相机对象定位玩家实体：实体 = [相机对象 + 0x318]，实体+0x230 = 位置。
 * 相机对象 = CObject 副本坐标地址 - 0x230（locate_coord_copies 缓存）。 */

static void do_teleport_to(float nx, float ny, const char* name)
{
    /* 定位扫描执行中禁止传送：收敛在后台改写 g_pa，避免并发读写 */
    if (g_scan_busy) {
        log_direct("[tp] teleport blocked: manual scan running");
        return;
    }
    /* 传送：收敛副本地址写入前先校验"活副本"——读当前值，取众数
     * 为玩家坐标基准，|值-基准|<=5 的才写（已失效/被复用的地址跳过，
     * 避免写入坏内存导致游戏崩溃）。 */
    int n = 0;
    /* 出生点方案：g_pa = 恰好 9 个精确相等的玩家坐标副本 */
    if (!g_pa_ready || g_pa_n == 0) {
        /* 兜底：通用扫描——全内存找精确相等 7~12 次的组（玩家副本任意位置） */
        uintptr_t copies[32];
        float ox = 0, oy = 0;
        int cn = find_player_copies(copies, 32, &ox, &oy);
        if (cn >= 7) {
            log_direct("[tp] fallback copies: %d @ (%.1f, %.1f)", cn, ox, oy);
            log_direct("[tp] TELEPORT %s -> (%.1f, %.1f)", name ? name : "?", nx, ny);
            for (int i = 0; i < cn; i++) {
                float cx = 0, cy = 0;
                if (!safe_read(&cx, (const void*)copies[i], 4) ||
                    !safe_read(&cy, (const void*)(copies[i] + 4), 4) ||
                    fabsf(cx - ox) > 5.0f || fabsf(cy - oy) > 5.0f) {
                    log_direct("[tp] skip stale %p (%.1f,%.1f)", (void*)copies[i], cx, cy);
                    continue;
                }
                float pair[2] = { nx, ny };
                int wr = safe_write(copies[i], pair, sizeof(pair));
                if (wr == 0) {
                    log_direct("[tp] write fail %p", (void*)copies[i]);
                    continue;
                }
                if (wr == 2)
                    log_direct("[tp] readback mismatch %p", (void*)copies[i]);
                n++;
            }
            log_direct("[tp] copies synced: %d", n);
            return;
        }
        g_tele_status = 1;
        set_hud_pos();
        log_direct("[tp] 正在寻找玩家坐标...");
        return;
    }
    g_tele_status = 0;
    set_hud_pos();
    log_direct("[tp] TELEPORT %s -> (%.1f, %.1f)",
            name ? name : "?", nx, ny);
    /* 写收敛副本全部：收敛时玩家站定，g_pa 即当天全部活副本
     * （含相机跟随源）。已失效/被复用地址值不合理，跳过防崩。 */
    for (int i = 0; i < g_pa_n; i++) {
        float cx = 0, cy = 0;
        if (!safe_read(&cx, (const void*)g_pa[i], 4) ||
            !safe_read(&cy, (const void*)(g_pa[i] + 4), 4) ||
            !(cx > 1.0f && cx < 1000000.0f && cy > 1.0f && cy < 1000000.0f)) {
            log_direct("[tp] skip stale %p (%.1f,%.1f)", (void*)g_pa[i], cx, cy);
            continue;
        }
        float pair[2] = { nx, ny };
        int wr = safe_write(g_pa[i], pair, sizeof(pair));
        if (wr == 0) {
            log_direct("[tp] write fail %p", (void*)g_pa[i]);
            continue;
        }
        if (wr == 2)
            log_direct("[tp] readback mismatch %p", (void*)g_pa[i]);
        n++;
    }
    log_direct("[tp] copies synced: %d", n);
}

static void hold_write(void)
{
    if (g_hold_cnt <= 0 || !g_player) return;
    float nx = g_hold_x, ny = g_hold_y;
    uintptr_t sub = g_sub ? g_sub : find_sub_in_entity(g_player);
    if (!sub)
        return;
    memcpy((void*)(sub + PLAYER_POS_OFF), &nx, 4);
    memcpy((void*)(sub + PLAYER_POS_OFF + 4), &ny, 4);
    memcpy((void*)(sub + PLAYER_POS_OFF_OLD), &nx, 4);
    memcpy((void*)(sub + PLAYER_POS_OFF_OLD + 4), &ny, 4);
    {
        BYTE zero[16] = { 0 };
        memcpy((void*)(sub + 0x330), zero, 16);
    }
    for (int i = 0; i < g_tf_n; i++)
        if (g_tf_coord[i]) {
            memcpy((void*)g_tf_coord[i], &nx, 4);
            memcpy((void*)(g_tf_coord[i] + 4), &ny, 4);
        }
    for (int i = 0; i < g_ph_n; i++)
        if (g_ph_coord[i]) {
            memcpy((void*)g_ph_coord[i], &nx, 4);
            memcpy((void*)(g_ph_coord[i] + 4), &ny, 4);
        }
    if (g_ent_pos) {
        memcpy((void*)(g_ent_pos + 0x230), &nx, 4);
        memcpy((void*)(g_ent_pos + 0x234), &ny, 4);
    }
    g_hold_cnt--;
}

/* 高频锁定线程：每 5ms 重写玩家碰撞体位置（对抗游戏帧循环覆盖）。
 * CE 能锁住 +0x3D0 证明它是有效位置字段，mod 之前失败是写入太慢。 */

#ifndef TP_KEYBOARD_ONLY
/* v2.1.4：手柄轮询独立线程——XInput 任何异常/卡顿都不影响
 * teleport_thread 的键盘轮询（F6/F7 菜单）。 */
static DWORD WINAPI pad_read_thread(LPVOID param)
{
    (void)param;
    Sleep(300);
    while (1) {
        Sleep(16);
        __try {
            if (!g_pad_ok_shared) {
                for (DWORD s = 0; s < 4; s++) {
                    TP_XSTATE st;
                    if (tp_xig && tp_xig(s, &st) == ERROR_SUCCESS) {
                        g_pad_slot = s;
                        InterlockedExchange(&g_pad_ok_shared, 1);
                        log_direct("[tp] pad slot=%u", (unsigned)s);
                        break;
                    }
                }
            }
            if (g_pad_ok_shared && tp_xig) {
                TP_XSTATE st;
                if (tp_xig(g_pad_slot, &st) == ERROR_SUCCESS) {
                    DWORD b = st.Gamepad.wButtons;
                    if (st.Gamepad.bLeftTrigger > 20)
                        b |= TP_PAD_LZ;
                    if (st.Gamepad.bRightTrigger > 20)
                        b |= TP_PAD_RZ;
                    InterlockedExchange((volatile LONG*)&g_pad_buttons,
                                        (LONG)b);
                } else {
                    InterlockedExchange(&g_pad_ok_shared, 0);
                    InterlockedExchange((volatile LONG*)&g_pad_buttons, 0);
                }
            }
        } __except (1) {
            InterlockedExchange(&g_pad_ok_shared, 0);
            InterlockedExchange((volatile LONG*)&g_pad_buttons, 0);
        }
    }
    return 0;
}
#endif

static DWORD WINAPI teleport_thread(LPVOID param)
{
    (void)param;
#ifndef TP_KEYBOARD_ONLY
    DWORD ppad = 0;
#endif
    int pf6 = 0, pf7 = 0, pup = 0, pdn = 0, pent = 0, pesc = 0,
        pbr1 = 0, pbr2 = 0, ptb = 0;
    while (1) {
        Sleep(80);
        hold_write();
        /* 手柄拦截（inline hook 系统 XInput）在游戏进程内会导致闪退，
         * 独立测试正常但游戏环境不兼容，已停用。 */
        if (!g_cfg.teleport) continue;

        /* Alt+F6 手动定位：钩子已吞键并置 latch，这里关菜单并通知扫描线程 */
        if (InterlockedExchange((volatile LONG*)&g_scan_latch, 0)) {
            if (g_scan_busy) {
                log_direct("[tp] Alt+F6 ignored: scan already running");
            } else {
                log_direct("[tp] Alt+F6 -> start manual scan");
                if (g_menu_open) {
                    g_menu_open = 0;
                    g_menu_page = 0;
                    g_rebind_row = -1;
                    g_rebind_armed = 0;
                    overlay_recenter(0);
                }
                g_scan_request = 1;
            }
        }

        int f6 = kbd_pressed(g_cfg.tp_key);
        int menu_opened_now = 0;
        if (f6 && !pf6) {
            g_last_input = 1;
            if (kbd_alt_down()) {
                /* Alt+F6：手动开始定位。Alt 用现读状态判定，不依赖
                 * 钩子事件时序（实测事件级 Alt 检测不可靠）。 */
                if (!g_scan_busy && !g_scan_request) {
                    log_direct("[tp] Alt+F6 -> start manual scan");
                    if (g_menu_open) {
                        g_menu_open = 0;
                        g_menu_page = 0;
                        g_rebind_row = -1;
                        g_rebind_armed = 0;
                        overlay_recenter(0);
                    }
                    g_scan_request = 1;
                }
            } else if ((!g_pa_ready && !g_player) || g_scan_busy) {
                log_direct("[tp] menu blocked: player/coords not found");
            } else {
                g_menu_open = !g_menu_open;
                if (g_menu_open) menu_opened_now = 1;
                overlay_recenter(g_menu_open);
                if (!g_menu_open) {
                    g_menu_page = 0;
                    g_rebind_row = -1;
                    g_rebind_armed = 0;
                }
            }
        }
        pf6 = f6;

        /* F7：切换左上角坐标 HUD（默认隐藏） */
        int f7 = kbd_pressed(g_cfg.hud_key);
        if (f7 && !pf7 && !g_menu_open) {
            g_show_pos = !g_show_pos;
            log_direct("[tp] HUD %s", g_show_pos ? "shown" : "hidden");
        }
        pf7 = f7;

#ifndef TP_KEYBOARD_ONLY
        /* ---- 手柄 ---- */
        /* v2.1.4：手柄状态由独立线程读取（1.08.1 下 XInput 调用可能
         * 卡死 teleport_thread，导致 F6 轮询停摆、菜单打不开）。
         * 这里只取共享状态，绝不直接调 XInput。 */
        DWORD pad = (DWORD)g_pad_buttons;
        DWORD pnew = pad & ~ppad;
        ppad = pad;
        if (pnew) g_last_input = 2;   /* 手柄有新按键 -> 设置页显示手柄配置 */
        if (pnew & (DWORD)g_cfg.pad_toggle_key) {
            log_direct("[tp] pad LB");
            if (g_rebind_row >= 0) {
                g_rebind_row = -1;
                g_rebind_armed = 0;
                log_direct("[tp] rebind cancelled (pad LB)");
            } else {
                if ((!g_pa_ready && !g_player) || g_scan_busy) {
                    log_direct("[tp] menu blocked: player/coords not found");
                } else {
                    g_menu_open = !g_menu_open;
                    overlay_recenter(g_menu_open);
                    if (!g_menu_open) {
                        g_menu_page = 0;
                        g_rebind_row = -1;
                        g_rebind_armed = 0;
                    }
                }
            }
        }
#endif

        if (g_menu_open) {
            int up = 0, dn = 0, ent = 0, esc = 0, br1 = 0, br2 = 0, tb = 0;
            /* 刚打开菜单的本轮不处理菜单内按键：打开菜单的键（如
             * TeleportKey 与 ConfirmKey 同键时）会被按住状态保险误读成
             * 确认/设置键，导致一开菜单就传送/翻页/进改键。 */
            if (!menu_opened_now) {
                up = kbd_pressed(g_cfg.up_key);
                dn = kbd_pressed(g_cfg.dn_key);
                ent = kbd_pressed(g_cfg.ok_key);
                esc = kbd_pressed(g_cfg.cancel_key);
                br1 = kbd_pressed(g_cfg.pg_key);
                br2 = kbd_pressed(g_cfg.nk_key);
                tb = kbd_pressed(g_cfg.settings_key);
            }
            if (up || dn || ent || esc || br1 || br2 || tb)
                g_last_input = 1;   /* 菜单内键盘键也算键盘输入 */
#ifndef TP_KEYBOARD_ONLY
            int hup = (pnew & (DWORD)g_cfg.pad_up_key) != 0;
            int hdn = (pnew & (DWORD)g_cfg.pad_down_key) != 0;
            int hbr1 = 0;
            int hbr2 = (pnew & (DWORD)g_cfg.pad_next_key) != 0;
            int hent = (pnew & (DWORD)g_cfg.pad_ok_key) != 0;
            int htb = (pnew & (DWORD)g_cfg.pad_settings_key) != 0;
#else
            enum { hup = 0, hdn = 0, hbr1 = 0, hbr2 = 0, hent = 0, htb = 0 };
#endif

            /* ---- 改键：等待玩家按下一个新键 ---- */
            if (g_rebind_row >= 0) {
                if (g_rebind_armed) {
                    /* 记住当前已按住的键，避免把“进入改键”的确认键也算进去 */
                    for (int i = 0; i < 256; i++)
                        g_key_prev[i] = kbd_pressed(i);
#ifndef TP_KEYBOARD_ONLY
                    g_pad_rebind_prev = pad;
#endif
                    g_rebind_armed = 0;
                } else if (esc && !pesc) {
                    g_rebind_row = -1;
                    pesc = 1; /* 取消改键，不关闭菜单 */
                    log_direct("[tp] rebind cancelled");
#ifndef TP_KEYBOARD_ONLY
                } else if (g_last_input == 2) {
                    /* 手柄模式：等待按下一个手柄按钮 */
                    for (int b = 0x0001; b <= 0x8000; b <<= 1) {
                        if ((pnew & b) && !(g_pad_rebind_prev & b)) {
                            int* target = NULL;
                            const char* cfgkey = NULL;
                            const char* kname = NULL;
                            switch (g_rebind_row) {
                            case 0: target = &g_cfg.pad_toggle_key;   cfgkey = "PadToggleKey";   kname = "菜单键"; break;
                            case 1: target = &g_cfg.pad_ok_key;       cfgkey = "PadOkKey";       kname = "确认";   break;
                            case 2: target = &g_cfg.pad_up_key;       cfgkey = "PadUpKey";       kname = "选择上"; break;
                            case 3: target = &g_cfg.pad_down_key;     cfgkey = "PadDownKey";     kname = "选择下"; break;
                            case 4: target = &g_cfg.pad_next_key;     cfgkey = "PadNextKey";     kname = "翻页";   break;
                            case 5: target = &g_cfg.pad_settings_key; cfgkey = "PadSettingsKey"; kname = "设置页"; break;
                            }
                            if (target) {
                                *target = b;
                                save_config_int(cfgkey, b);
                                log_direct("[tp] %s -> pad %d", kname, b);
                                g_pad_rebind_prev = pad;
                                g_rebind_row = -1;
                            }
                            break;
                        }
                    }
#endif
                } else {
                    for (int vk = 0x08; vk <= 0xFE; vk++) {
                        int d = kbd_pressed(vk);
                        if (d && !g_key_prev[vk]) {
                            int* target = NULL;
                            const char* cfgkey = NULL;
                            const char* kname = NULL;
                            switch (g_rebind_row) {
                            case 0: target = &g_cfg.tp_key;       cfgkey = "TeleportKey";  kname = "菜单键";   break;
                            case 1: target = &g_cfg.settings_key; cfgkey = "SettingsKey";  kname = "设置页键"; break;
                            case 2: target = &g_cfg.up_key;       cfgkey = "UpKey";        kname = "上移";     break;
                            case 3: target = &g_cfg.dn_key;       cfgkey = "DownKey";      kname = "下移";     break;
                            case 4: target = &g_cfg.pg_key;       cfgkey = "PrevPageKey";  kname = "上一页";   break;
                            case 5: target = &g_cfg.nk_key;       cfgkey = "NextPageKey";  kname = "下一页";   break;
                            case 6: target = &g_cfg.ok_key;       cfgkey = "ConfirmKey";   kname = "确认";     break;
                            case 7: target = &g_cfg.cancel_key;   cfgkey = "CancelKey";    kname = "取消";     break;
                            }
                            if (target) {
                                /* 防冲突：新键不能与其它已用菜单键相同
                                 * （例如把菜单键改成 Enter 会与确认键冲突，
                                 * 一开菜单就误确认/误翻页） */
                                int conflict = 0;
                                if (target != &g_cfg.tp_key && vk == g_cfg.tp_key) conflict = 1;
                                if (target != &g_cfg.settings_key && vk == g_cfg.settings_key) conflict = 1;
                                if (target != &g_cfg.up_key && vk == g_cfg.up_key) conflict = 1;
                                if (target != &g_cfg.dn_key && vk == g_cfg.dn_key) conflict = 1;
                                if (target != &g_cfg.pg_key && vk == g_cfg.pg_key) conflict = 1;
                                if (target != &g_cfg.nk_key && vk == g_cfg.nk_key) conflict = 1;
                                if (target != &g_cfg.ok_key && vk == g_cfg.ok_key) conflict = 1;
                                if (target != &g_cfg.cancel_key && vk == g_cfg.cancel_key) conflict = 1;
                                if (conflict) {
                                    log_direct("[tp] rebind cancelled: key %d conflicts with another menu key", vk);
                                    g_rebind_row = -1;
                                    g_rebind_armed = 0;
                                    break;
                                }
                                *target = vk;
                                save_config_int(cfgkey, vk);
                                log_direct("[tp] %s -> key %d", kname, vk);
                                g_key_prev[vk] = 1;
                                /* 防止新绑定的键立刻触发一次菜单动作 */
                                if (vk == g_cfg.tp_key) pf6 = 1;
                                if (vk == g_cfg.settings_key) ptb = 1;
                                if (vk == g_cfg.up_key) pup = 1;
                                if (vk == g_cfg.dn_key) pdn = 1;
                                if (vk == g_cfg.ok_key) pent = 1;
                                if (vk == g_cfg.cancel_key) pesc = 1;
                                if (vk == g_cfg.pg_key) pbr1 = 1;
                                if (vk == g_cfg.nk_key) pbr2 = 1;
                                g_rebind_row = -1;
                            }
                            break;
                        }
                        g_key_prev[vk] = d;
                    }
                }
            }

            if (g_rebind_row < 0) {
                if ((tb && !ptb) || htb) {
                    g_menu_page ^= 1;
                    g_menu_sel = 0;
                }
                if (g_menu_page == 0 && g_dest_order_n > 0) {
                    if ((up && !pup) || hup)
                        g_menu_sel = (g_menu_sel + g_dest_order_n - 1) % g_dest_order_n;
                    if ((dn && !pdn) || hdn)
                        g_menu_sel = (g_menu_sel + 1) % g_dest_order_n;
                    if ((br1 && !pbr1) || hbr1) {
                        int np = (g_dest_order_n + TP_PAGE_SIZE - 1) / TP_PAGE_SIZE;
                        int pg = (g_menu_sel / TP_PAGE_SIZE + np - 1) % np;
                        g_menu_sel = pg * TP_PAGE_SIZE;
                    }
                    if ((br2 && !pbr2) || hbr2) {
                        int np = (g_dest_order_n + TP_PAGE_SIZE - 1) / TP_PAGE_SIZE;
                        int pg = (g_menu_sel / TP_PAGE_SIZE + 1) % np;
                        g_menu_sel = pg * TP_PAGE_SIZE;
                    }
                    if ((ent && !pent) || hent) {
                        int di = g_dest_order[g_menu_sel];
                        if (g_unlocked[di]) {
                            do_teleport_to(g_cfg.dests[di].x,
                                           g_cfg.dests[di].y,
                                           g_cfg.dests[di].name);
                            g_menu_open = 0;
                            overlay_recenter(0);
                            g_menu_page = 0;
                        } else {
                            log_direct("[tp] destination locked (not visited): %s",
                                    g_cfg.dests[di].name);
                        }
                    }
                } else if (g_menu_page == 1) {
                    if ((up && !pup) || hup)
                        g_menu_sel = (g_menu_sel + SETTINGS_ROWS - 1) % SETTINGS_ROWS;
                    if ((dn && !pdn) || hdn)
                        g_menu_sel = (g_menu_sel + 1) % SETTINGS_ROWS;
                    if ((ent && !pent) || hent) {
                        g_rebind_row = g_menu_sel;
                        g_rebind_armed = 1;
                        log_direct("[tp] rebind row %d: press new key...",
                                g_menu_sel);
                    }
                }
                if (esc && !pesc) {
                    g_menu_open = 0;
                    overlay_recenter(0);
                    g_menu_page = 0;
                    g_rebind_row = -1;
                    g_rebind_armed = 0;
                }
            }
            pup = up; pdn = dn; pent = ent; pesc = esc;
            pbr1 = br1; pbr2 = br2; ptb = tb;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 叠加窗口（空闲显示坐标，菜单显示目的地列表）                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ov_wndproc_impl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        float s = g_ui_scale;
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW((int)(23 * s + 0.5f), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, L"Microsoft YaHei");
        HFONT old = (HFONT)SelectObject(hdc, font);

        if (g_menu_open) {
            /* 键位提示（统一显示在左上角） */
            WCHAR ktg[16], kok[16], kup[16], kdn[16], kbr1[16], kbr2[16],
                  kesc[16], kst[16];
#ifndef TP_KEYBOARD_ONLY
            WCHAR ptg[16], pok[16], pup[16], pdn[16], pnx[16], pst[16];
#endif
            key_name_w(g_cfg.tp_key, ktg, 16);
            key_name_w(g_cfg.ok_key, kok, 16);
            key_name_w(g_cfg.up_key, kup, 16);
            key_name_w(g_cfg.dn_key, kdn, 16);
            key_name_w(g_cfg.pg_key, kbr1, 16);
            key_name_w(g_cfg.nk_key, kbr2, 16);
            key_name_w(g_cfg.cancel_key, kesc, 16);
            key_name_w(g_cfg.settings_key, kst, 16);
#ifndef TP_KEYBOARD_ONLY
            pad_key_name(g_cfg.pad_toggle_key, ptg, 16);
            pad_key_name(g_cfg.pad_ok_key, pok, 16);
            pad_key_name(g_cfg.pad_up_key, pup, 16);
            pad_key_name(g_cfg.pad_down_key, pdn, 16);
            pad_key_name(g_cfg.pad_next_key, pnx, 16);
            pad_key_name(g_cfg.pad_settings_key, pst, 16);
            WCHAR buf[256];
            /* 手柄提示：黄色 */
            SetTextColor(hdc, RGB(255, 255, 0));
            RECT hr1 = { (LONG)(8 * s), (LONG)(4 * s),
                         (LONG)(392 * s), (LONG)(70 * s) };
            swprintf(buf, 256, L"手柄: %ls菜单  %ls传送\r\n"
                                L"%ls%ls选择  %ls翻页  %ls设置",
                     ptg, pok, pup, pdn, pnx, pst);
            DrawTextW(hdc, buf, -1, &hr1, DT_NOPREFIX);
#else
            WCHAR buf[256];
#endif
            /* 键盘提示：绿色 */
            SetTextColor(hdc, RGB(0, 255, 0));
            RECT hr2 = { (LONG)(8 * s), (LONG)(72 * s),
                         (LONG)(392 * s), (LONG)(186 * s) };
            swprintf(buf, 256, L"键盘: %ls菜单\r\n"
                                L"%ls/%ls选择  %ls/%ls翻页\r\n"
                                L"%ls传送  %ls关闭\r\n"
                                L"%ls设置页",
                     ktg, kup, kdn, kbr1, kbr2, kok, kesc, kst);
            DrawTextW(hdc, buf, -1, &hr2, DT_NOPREFIX);
        } else if (g_show_pos || g_tele_status != 0) {
            /* 空闲显示玩家坐标；传送状态居中顶部显示 */
            SetTextColor(hdc, RGB(0, 255, 0));
            WCHAR buf[256];
            if (g_tele_status == 1) {
                SetTextColor(hdc, RGB(255, 0, 0));
                swprintf(buf, 256, L"传送定位未就绪\r\n请在小屋门口站定\r\n按 Alt+F6 开始定位");
            } else if (g_tele_status == 3) {
                SetTextColor(hdc, RGB(255, 0, 0));
                swprintf(buf, 256, L"定位扫描中\r\n请勿移动（约 1~2 秒）");
            } else if (g_tele_status == 2) {
                swprintf(buf, 256, L"已找到玩家坐标，可以进行传送");
            } else {
                swprintf(buf, 256, L"player pos now: (%.0f, %.0f)",
                         g_ov_px, g_ov_py);
            }
            RECT rc;
            if (g_tele_status != 0) {
                /* 居中顶部，大字号 */
                RECT wr;
                GetClientRect(hwnd, &wr);
                rc = (RECT){ (LONG)(16 * s), (LONG)(8 * s),
                             wr.right - (LONG)(16 * s),
                             wr.bottom - (LONG)(8 * s) };
                HFONT big = CreateFontW((int)(30 * s + 0.5f), 0, 0, 0,
                                        FW_BOLD, 0, 0, 0,
                                        DEFAULT_CHARSET, 0, 0,
                                        CLEARTYPE_QUALITY, 0,
                                        L"Microsoft YaHei");
                if (big) {
                    HFONT oldf = (HFONT)SelectObject(hdc, big);
                    DrawTextW(hdc, buf, -1, &rc, DT_CENTER | DT_NOPREFIX);
                    SelectObject(hdc, oldf);
                    DeleteObject(big);
                }
            } else {
                rc = (RECT){ (LONG)(8 * s), (LONG)(4 * s),
                             (LONG)(392 * s), (LONG)(80 * s) };
                DrawTextW(hdc, buf, -1, &rc, DT_CENTER | DT_NOPREFIX);
            }
        }
        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        overlay_update_state();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* 窗口过程包一层异常保护：绘制/回调出错只记录日志并跳过，
 * 不让未处理异常传播到游戏主线程导致闪退 */
static LRESULT CALLBACK ov_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    __try {
        return ov_wndproc_impl(hwnd, msg, wp, lp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_direct("[tp] overlay exception code=0x%08X",
                (unsigned)GetExceptionCode());
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* 中间菜单窗口（半透明黑底，只显示标题/目的地/按键设置）              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK menu_wndproc_impl(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        float s = g_ui_scale;
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        HFONT font = CreateFontW((int)(23 * s + 0.5f), 0, 0, 0, FW_BOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 0, L"Microsoft YaHei");
        HFONT old = (HFONT)SelectObject(hdc, font);

        if (g_menu_page == 0) {
            if (g_dest_order_n > 0) {
                int page = g_menu_sel / TP_PAGE_SIZE;
                int npages = (g_dest_order_n + TP_PAGE_SIZE - 1) / TP_PAGE_SIZE;
                int start = page * TP_PAGE_SIZE;
                int end = start + TP_PAGE_SIZE;
                if (end > g_dest_order_n) end = g_dest_order_n;

                SetTextColor(hdc, RGB(255, 255, 255));
                RECT tr = { (LONG)(8 * s), (LONG)(4 * s),
                            (LONG)(172 * s), (LONG)(30 * s) };
                WCHAR title[96];
                swprintf(title, 96, L"选择传送地点");
                DrawTextW(hdc, title, -1, &tr, DT_NOPREFIX);
                WCHAR pg[128];
                swprintf(pg, 128, L"第 %d/%d 页", page + 1, npages);
                RECT pr = { (LONG)(8 * s), (LONG)(30 * s),
                            (LONG)(172 * s), (LONG)(56 * s) };
                SetTextColor(hdc, RGB(200, 200, 200));
                DrawTextW(hdc, pg, -1, &pr, DT_NOPREFIX);
                for (int i = start; i < end; i++) {
                    int di = g_dest_order[i];
                    RECT lr = { (LONG)(8 * s),
                                (LONG)((56 + (i - start) * 28) * s),
                                (LONG)(172 * s),
                                (LONG)((84 + (i - start) * 28) * s) };
                    int locked = !g_unlocked[di];
                    SetTextColor(hdc, locked ? RGB(120, 120, 120)
                                 : i == g_menu_sel ? RGB(255, 255, 0)
                                 : RGB(0, 255, 0));
                    WCHAR namew[64];
                    MultiByteToWideChar(CP_UTF8, 0, g_cfg.dests[di].name,
                                        -1, namew, 64);
                    WCHAR line[128];
                    swprintf(line, 128, L"%c%d. %ls",
                             i == g_menu_sel ? L'>' : L' ', i + 1, namew);
                    DrawTextW(hdc, line, -1, &lr, DT_NOPREFIX);
                }
            } else {
                SetTextColor(hdc, RGB(255, 255, 255));
                    RECT tr = { (LONG)(8 * s), (LONG)(56 * s),
                                (LONG)(172 * s), (LONG)(100 * s) };
                DrawTextW(hdc, L"teleport.txt 中没有目的地", -1, &tr,
                          DT_NOPREFIX);
            }
        } else {
            /* ---- 按键设置页（按当前输入方式显示键盘或手柄配置） ---- */
            SetTextColor(hdc, RGB(255, 255, 255));
            RECT tr = { (LONG)(8 * s), (LONG)(4 * s),
                        (LONG)(172 * s), (LONG)(30 * s) };
            WCHAR title[96];
#ifndef TP_KEYBOARD_ONLY
            if (g_last_input == 2)
                swprintf(title, 96, L"按键设置（手柄）");
            else
#endif
                swprintf(title, 96, L"按键设置（键盘）");
            DrawTextW(hdc, title, -1, &tr, DT_NOPREFIX);
            RECT hr1 = { (LONG)(8 * s), (LONG)(30 * s),
                         (LONG)(172 * s), (LONG)(50 * s) };
            RECT hr2 = { (LONG)(8 * s), (LONG)(50 * s),
                         (LONG)(172 * s), (LONG)(72 * s) };
            SetTextColor(hdc, RGB(200, 200, 200));
            DrawTextW(hdc, L"选择要修改的按键", -1, &hr1, DT_NOPREFIX);
            DrawTextW(hdc, L"按确认进入改键", -1, &hr2, DT_NOPREFIX);

#ifndef TP_KEYBOARD_ONLY
            if (g_last_input == 2) {
                const char* labels[6] = {
                    "菜单键", "确认", "选择上", "选择下", "翻页", "设置页"
                };
                int keys[6] = {
                    g_cfg.pad_toggle_key, g_cfg.pad_ok_key,
                    g_cfg.pad_up_key, g_cfg.pad_down_key,
                    g_cfg.pad_next_key, g_cfg.pad_settings_key
                };
                for (int i = 0; i < 6; i++) {
                    RECT lr = { (LONG)(8 * s),
                                (LONG)((74 + i * 23) * s),
                                (LONG)(172 * s),
                                (LONG)((97 + i * 23) * s) };
                    int active = (g_rebind_row == i);
                    SetTextColor(hdc, active ? RGB(255, 255, 0)
                                 : i == g_menu_sel ? RGB(0, 255, 255)
                                 : RGB(200, 200, 200));
                    WCHAR lbw[32], kw[16];
                    MultiByteToWideChar(CP_UTF8, 0, labels[i], -1, lbw, 32);
                    pad_key_name(keys[i], kw, 16);
                    WCHAR line[96];
                    if (active)
                        swprintf(line, 96, L"%c%ls: 请按新键...",
                                 i == g_menu_sel ? L'>' : L' ', lbw);
                    else
                        swprintf(line, 96, L"%c%ls: %ls",
                                 i == g_menu_sel ? L'>' : L' ', lbw, kw);
                    DrawTextW(hdc, line, -1, &lr, DT_NOPREFIX);
                }
            } else
#endif
            {
                const char* labels[SETTINGS_ROWS] = {
                    "菜单键", "设置页键", "上移", "下移",
                    "上一页", "下一页", "确认", "取消"
                };
                int keys[SETTINGS_ROWS] = {
                    g_cfg.tp_key, g_cfg.settings_key, g_cfg.up_key,
                    g_cfg.dn_key, g_cfg.pg_key, g_cfg.nk_key,
                    g_cfg.ok_key, g_cfg.cancel_key
                };
                for (int i = 0; i < SETTINGS_ROWS; i++) {
                    RECT lr = { (LONG)(8 * s),
                                (LONG)((74 + i * 23) * s),
                                (LONG)(172 * s),
                                (LONG)((97 + i * 23) * s) };
                    int active = (g_rebind_row == i);
                    SetTextColor(hdc, active ? RGB(255, 255, 0)
                                 : i == g_menu_sel ? RGB(0, 255, 255)
                                 : RGB(200, 200, 200));
                    WCHAR lbw[32], kw[16];
                    MultiByteToWideChar(CP_UTF8, 0, labels[i], -1, lbw, 32);
                    key_name_w(keys[i], kw, 16);
                    WCHAR line[96];
                    if (active)
                        swprintf(line, 96, L"%c%ls: 请按新键...",
                                 i == g_menu_sel ? L'>' : L' ', lbw);
                    else
                        swprintf(line, 96, L"%c%ls: %ls",
                                 i == g_menu_sel ? L'>' : L' ', lbw, kw);
                    DrawTextW(hdc, line, -1, &lr, DT_NOPREFIX);
                }
            }
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
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK menu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    __try {
        return menu_wndproc_impl(hwnd, msg, wp, lp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_direct("[tp] menu exception code=0x%08X",
                (unsigned)GetExceptionCode());
        return 0;
    }
}

static DWORD WINAPI overlay_thread(LPVOID param)
{
    (void)param;
    ui_scale_init();
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ov_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "TeleportOverlay";
    if (!RegisterClassA(&wc))
        return 0;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = menu_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "TeleportMenu";
    if (!RegisterClassA(&wc))
        return 0;

    /* 左上角提示 / HUD 窗口 */
    {
    float s = g_ui_scale;
    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "TeleportOverlay", "Teleport",
        WS_POPUP, (int)(8 * s), (int)(8 * s),
        (int)(400 * s), (int)(190 * s), NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 0;
    g_hwnd = hwnd;
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 500, NULL);
    log_direct("[tp] overlay created vis=%d", (int)IsWindowVisible(hwnd));

    /* 中间菜单窗口（半透明黑底，只显示菜单与目的地） */
    HWND mh = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "TeleportMenu", "TeleportMenu",
#ifdef TP_SD_SCALE
        WS_POPUP, 0, 0, (int)(210 * s), (int)(300 * s),
#else
        WS_POPUP, 0, 0, 180, 300,
#endif
        NULL, NULL, wc.hInstance, NULL);
    if (mh) {
        g_menu_hwnd = mh;
        SetLayeredWindowAttributes(mh, 0, 200, LWA_ALPHA);
        ShowWindow(mh, SW_HIDE);
        SetTimer(mh, 1, 500, NULL);
        log_direct("[tp] menu window created");
    }
    }

    overlay_recenter(0);
    g_kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_hook,
                                   GetModuleHandleA(NULL), 0);
    log_direct("[tp] kbd hook %s", g_kbd_hook ? "installed" : "failed");

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_kbd_hook) {
        UnhookWindowsHookEx(g_kbd_hook);
        g_kbd_hook = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        /* 固定本 DLL：防止游戏运行中卸载导致钩子/线程悬空崩溃 */
        {
            HMODULE hm = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCSTR)(uintptr_t)DllMain, &hm);
        }

        g_base = (uintptr_t)GetModuleHandleA(NULL);

        /* 配置和日志放在 DLL 所在目录（游戏根目录\Mods\Teleport） */
        GetModuleFileNameA(hinst, g_self_dir, MAX_PATH);
        {
            char* slash = strrchr(g_self_dir, '\\');
            if (slash) *(slash + 1) = 0;
        }
        lstrcpyA(g_log_path, g_self_dir);
        lstrcatA(g_log_path, "teleport.log");

        read_config();
        log_init(); /* 打开日志（截断旧文件） */
#ifndef TP_KEYBOARD_ONLY
        tp_load_xinput();
#endif
        log_direct("[teleport] ===== Teleport Mod v%s loaded =====", TP_VERSION);
        log_direct("[teleport] village.exe base=%p", (void*)g_base);
        log_direct("[teleport] config: teleport=%d key=%d dests=%d",
                g_cfg.teleport, g_cfg.tp_key, g_cfg.dest_count);

        /* 先解析全部版本敏感地址（写日志），再启动任何线程 */
        addr_init();

        HANDLE h;
        /* player_scan_thread 已停用：1.08.1 玩家虚表扫描只产出假对象
         * （g_player 仅死代码 hold_write 使用），每 3s 全内存扫描纯负担 */
        h = CreateThread(NULL, 0, birth_scan_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        h = CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
#ifndef TP_KEYBOARD_ONLY
        h = CreateThread(NULL, 0, pad_read_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
#endif
        if (g_cfg.teleport) {
            h = CreateThread(NULL, 0, teleport_thread, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
    }
    return TRUE;
}
