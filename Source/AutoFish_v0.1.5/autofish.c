/* autofish.c - AutoFish v0.1.5（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现，公共机制来自 shared\：
 *   shared\addrsig.c/h  运行时地址签名解析
 *   shared\modkit.c/h   日志(LogKit) / 内存(mk_*) / Hook(HookPoint,HookTxn)
 *                       / 版本档案(GameProfile)
 *
 * 文件内分区：
 *   1 版本档案      偏移、字段、钩子点原文、addrsig 签名表
 *   2 运行期状态    AutoFishContext 单例 g_af（唯一全局）
 *   3 日志          句柄复用；热路径入环，mod_tick 批量落盘
 *   4 上下文        基址/路径/配置/档案加载
 *   5 钓鱼状态机    read → validate → action → record
 *   6 钩子装配      stub 机器码 + 事务式补丁 + 失败回滚
 *   7 垃圾过滤      候选追加点 stub，F10 开关
 *   8 输入与 UI     低级键盘钩子 + 右下角 toast
 *   9 生命周期      mod_init / mod_tick / DllMain
 *
 * 功能：连续钓鱼（三个 NOP 补丁 + 四个状态机钩子）+ 垃圾过滤（F10）。
 * 地址全部由 addrsig 运行时解析；任一必需签名缺失即整体停用，
 * 绝不硬用旧地址。
 *
 * 编译（在 AutoFish_v0.1.5 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o AutoFish.dll
 *     autofish.c ..\shared\addrsig.c ..\shared\modkit.c -lgdi32 -luser32
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

/* update 内相对偏移 */
#define OFF_DISPATCH           0x50
#define OFF_CANCEL_REQUEST     0x533
#define OFF_CANCEL_CONTINUE    0x53E
#define OFF_SUCCESS_PHASE      0xD38
#define OFF_AUTO_REEL          0x9BE
#define OFF_PHASE4_FINISH      0x1859
#define OFF_PHASE4_NATIVE_CONT 0x1861
#define OFF_PHASE4_CLEANUP     0x1862
#define OFF_SUCCESS_GATE       0x19DB
#define OFF_SUCCESS_GATE_CONT  0x19E2
#define OFF_DIRECT_NULL_EXIT   0x19E5
#define OFF_SUCCESS_EXIT_JUMP  0x1A34
#define OFF_COMMON_EXIT        0x1B2A
#define OFF_COMMON_EXIT_CONT   0x1B31
#define OFF_UPDATE_EPILOGUE    0x1B94

/* 咬钩候选抽取 FUN_14020f410 = update+0x2710；候选追加点 update+0x2CA5
 * （TEST R15D,R15D / JLE +0x4C）；RDI=候选物品配置对象 */
#define OFF_ROLL_FUNC          0x2710
#define OFF_NO_TRASH_APPEND    0x2CA5

/* 状态字段偏移 */
#define F_CURRENT       0x1D8
#define F_REQUESTED     0x1DC
#define F_VALID_SPOT    0x270
#define F_TASK          0x290
#define F_SPECIAL       0x2B8
#define F_WAIT_TICKS    0x2F8

/* addrsig 条目名（同时是日志标签；标称 RVA 仅是标签的一部分） */
#define SIG_VT_STATE    "vt_state_0xE114F0"
#define SIG_CLOCK       "clock_0x10CD990"
#define SIG_PATCH_JZ    "fish_jz_0x382B2D"
#define SIG_PATCH_JNZ   "fish_jnz_0x382B64"
#define SIG_UPDATE      "fish_update_0x20CD00"

/* 钩子点原始字节（1.08.1 验证） */
static const unsigned char kExpectSuccessJump[5] = { 0xE9, 0xF1, 0x00, 0x00, 0x00 };
static const unsigned char kExpectDirectNullExit[6] = { 0x0F, 0x84, 0x3F, 0x01, 0x00, 0x00 };
static const unsigned char kExpectSuccessGate[7] = { 0x49, 0x8B, 0x8E, 0x90, 0x02, 0x00, 0x00 };
static const unsigned char kExpectCommonExit[7] = { 0x48, 0x8B, 0x05, 0x5F, 0xF1, 0xEB, 0x00 };
static const unsigned char kExpectDispatch[12] = {
    0x48, 0x63, 0x81, 0xD8, 0x01, 0x00, 0x00, 0x83, 0xF8, 0x07, 0x0F, 0x87
};
static const unsigned char kExpectSuccessPhase[10] = {
    0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x06, 0x00, 0x00
};
static const unsigned char kExpectCancelPhase[11] = {
    0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00
};
static const unsigned char kExpectPhase4Finish[8] = {
    0x49, 0x8B, 0xCE, 0xE8, 0x4F, 0x81, 0xFF, 0xFF
};
static const unsigned char kExpectUpdateEntry[15] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
    0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20
};
static const unsigned char kExpectAppend[5] = { 0x45, 0x85, 0xFF, 0x7E, 0x4C };
/* 三个 NOP 补丁点的原文 */
static const unsigned char kExpectJz6[6] = { 0x0F, 0x84, 0x81, 0x01, 0x00, 0x00 };
static const unsigned char kExpectJnz6[6] = { 0x0F, 0x85, 0x77, 0x01, 0x00, 0x00 };
static const unsigned char kExpectReel2[2] = { 0x74, 0x1D };
static const unsigned char kNops[8] = {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

/* addrsig 签名表（跨版本由签名自动重定位） */
static const AddrSig kSigs[] = {
    { SIG_VT_STATE, ADDRSIG_RIPGLOBAL,
      "5c24104889742418574883ec50488bd9488d0500000000488901488d050000000048894110488d",
      "010101010101010101010101010101010101010000000001010101010100000000010101010101", 19 },
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
    { SIG_PATCH_JZ, ADDRSIG_FUNC,
      "4533c033d2488b4920e80000000084c00f840000000080bf48020000000f8574",
      "0101010101010101010100000000010101010000000001010101010101010101", -16 },
    { SIG_PATCH_JNZ, ADDRSIG_FUNC,
      "00483bc17419660f1f440000837804020f85000000004883c008483bc175ed48",
      "0101010101010101010101010101010101010000000001010101010101010101", -16 },
    { SIG_UPDATE, ADDRSIG_FUNC,
      "486381d801000083f8070f87341b0000",
      "01010100000000010101010100000000", 0x50 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_VT_STATE,  0, 0, 0 },
    { SIG_CLOCK,     0, 0, 0 },
    { SIG_PATCH_JZ,  0, 0, 0 },
    { SIG_PATCH_JNZ, 0, 0, 0 },
    { SIG_UPDATE,    0, 0, 0 },
};

/* ==================================================================
 * 2 运行期状态（唯一全局 g_af）
 * ================================================================== */
#define MAX_KEY        256
#define TOAST_TEXT_MAX 160

typedef struct {
    volatile LONG available;
    volatile LONG enabled;
    volatile LONG loop_ready;
    volatile LONG post_catch_ready;
    volatile LONG monitor_ready;
    uintptr_t     loop_stub;
    uintptr_t     success_stub;
    uintptr_t     cancel_stub;
    uintptr_t     post_catch_stub;
    uintptr_t     cancel_requested_state;
    uintptr_t     original_update;      /* monitor detour 的 trampoline */
    uint64_t      loop_count;
    uintptr_t     last_monitor_state;   /* state-before 去重 */
    uint32_t      last_monitor_current;
    uint32_t      last_monitor_requested;
} FishingState;

typedef struct {
    volatile LONG ready;
    volatile LONG active;
    uintptr_t     stub;
    /* stub 机器码以绝对地址 lock inc 这两个计数器，必须是固定地址 */
    volatile LONG exec_count;
    volatile LONG skip_count;
} TrashState;

typedef struct {
    HHOOK         hook;
    DWORD         thread_id;
    volatile LONG down[MAX_KEY];
    volatile LONG latch[MAX_KEY];
    int           prev_state;
    int           toggle_key;
} InputState;

typedef struct {
    HWND              hwnd;
    HFONT             font;
    wchar_t           text[TOAST_TEXT_MAX];
    COLORREF          accent;
    volatile ULONGLONG hide_at;
} UiState;

typedef struct {
    uintptr_t   base;
    char        self_dir[MAX_PATH];
    char        log_path[MAX_PATH];
    char        ini_path[MAX_PATH];
    volatile LONG enabled;
    LogKit      log;
    GameProfile profile;
    /* addrsig 解析结果 */
    uintptr_t   vt_state;
    uintptr_t   clock;
    uintptr_t   patch_jz;
    uintptr_t   patch_jnz;
    uintptr_t   update_rva;
    int         ready;
    FishingState fishing;
    TrashState   trash;
    InputState   input;
    UiState      ui;
} AutoFishContext;

static AutoFishContext g_af;

/* 分区之间互相引用，统一在此声明 */
static void  log_init(void);
static void  log_close(void);
static void  log_direct(const char* fmt, ...);
static void  log_hot(const char* fmt, ...);
static void  log_tick(void);
static void  ctx_init(void);
static void  ctx_load_config(void);
static BOOL  load_profile(void);
static BOOL  hook_install_loop(void);
static BOOL  hook_install_post_catch(void);
static BOOL  hook_install_monitor(void);
static BOOL  fishing_set(BOOL enable);
static BOOL  trash_install(void);
static BOOL  trash_set(BOOL on);
static void  trash_toggle(void);
static void  ui_toast(const wchar_t* message, COLORREF accent);
static void  ui_pump(void);
static void  input_start(void);
static void  input_stop(void);
static void  input_poll(void);
/* 回调地址被写进 stub 机器码 */
static void __fastcall cb_mark_cancel_request(uintptr_t state);
static BOOL __fastcall cb_observe_success_gate(uintptr_t state);
static BOOL __fastcall cb_rearm_common_exit(uintptr_t state);
static BOOL __fastcall cb_rearm_after_catch(uintptr_t state);
static BOOL __fastcall cb_monitor_detour(uintptr_t state, void* frame_context);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_af.log, g_af.log_path, "af", TRUE);
}

static void log_close(void)
{
    logkit_close(&g_af.log);
}

/* 安装/初始化期：调用即落盘 */
static void log_direct(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_write(&g_af.log, "%s", buf);
}

/* 热路径：只写内存环，不产生文件 IO */
static void log_hot(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_queue(&g_af.log, "%s", buf);
}

static void log_tick(void)
{
    logkit_flush(&g_af.log);
}

/* ==================================================================
 * 4 上下文
 * ================================================================== */
static void ctx_init(void)
{
    g_af.base = (uintptr_t)GetModuleHandleA(NULL);
    GetModuleFileNameA(GetModuleHandleA("AutoFish.dll"), g_af.self_dir,
                       MAX_PATH);
    {
        char* slash = strrchr(g_af.self_dir, '\\');
        if (slash) *(slash + 1) = 0;
    }
    lstrcpynA(g_af.log_path, g_af.self_dir, MAX_PATH);
    lstrcatA(g_af.log_path, "autofish.log");
    lstrcpynA(g_af.ini_path, g_af.self_dir, MAX_PATH);
    lstrcatA(g_af.ini_path, "autofish.ini");
    g_af.enabled = 1;
    g_af.input.toggle_key = 0x79;   /* VK_F10 */
}

static void ctx_load_config(void)
{
    DWORD n = GetPrivateProfileIntA("main", "enabled", 1, g_af.ini_path);
    g_af.enabled = n ? 1 : 0;
    log_direct("[af] config enabled=%d", (int)g_af.enabled);
}

static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_af.profile.mod = "af";
    g_af.profile.base = g_af.base;
    g_af.profile.sigs = kSigs;
    g_af.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_af.profile.res = s_res;
    g_af.profile.entries = s_entries;
    g_af.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_af.profile);

    g_af.vt_state   = profile_addr(&g_af.profile, SIG_VT_STATE);
    g_af.clock      = profile_addr(&g_af.profile, SIG_CLOCK);
    g_af.patch_jz   = profile_addr(&g_af.profile, SIG_PATCH_JZ);
    g_af.patch_jnz  = profile_addr(&g_af.profile, SIG_PATCH_JNZ);
    g_af.update_rva = (uintptr_t)profile_rva(&g_af.profile, SIG_UPDATE);
    g_af.ready = (g_af.vt_state && g_af.clock && g_af.patch_jz &&
                  g_af.patch_jnz && g_af.update_rva) ? 1 : 0;
    return g_af.ready;
}

static uintptr_t update_addr(uintptr_t off)
{
    return g_af.base + g_af.update_rva + off;
}

/* ==================================================================
 * 5 钓鱼状态机：read → validate → action → record
 * ================================================================== */
typedef struct {
    uintptr_t vtable;
    uint32_t  current;
    uint32_t  requested;
    BOOL      valid_spot;
    uintptr_t task;
} StateView;

static void state_view_reset(StateView* v)
{
    v->vtable = 0;
    v->current = ~0u;
    v->requested = ~0u;
    v->valid_spot = FALSE;
    v->task = 0;
}

/* read */
static BOOL state_view_read(uintptr_t state, StateView* v)
{
    unsigned char flag = 0;
    if (!state || !mk_readable((void*)state, (size_t)(F_WAIT_TICKS + 8)))
        return FALSE;
    if (!mk_read(&v->vtable, (const void*)state, 8)) return FALSE;
    if (!mk_read(&v->current, (const void*)(state + F_CURRENT), 4)) return FALSE;
    if (!mk_read(&v->requested, (const void*)(state + F_REQUESTED), 4))
        return FALSE;
    if (!mk_read(&flag, (const void*)(state + F_VALID_SPOT), 1)) return FALSE;
    v->valid_spot = flag != 0;
    if (!mk_read(&v->task, (const void*)(state + F_TASK), 8)) return FALSE;
    return TRUE;
}

/* record */
static void state_view_record(const char* what, uintptr_t state,
                              const StateView* v)
{
    log_hot("[af] %s object=%p current=%u requested=%u valid_spot=%d task=%p",
            what, (void*)state, v->current, v->requested,
            v->valid_spot ? 1 : 0, (void*)v->task);
}

/* action */
static void action_clear_request(uintptr_t state)
{
    uint32_t zero32 = 0;
    uint64_t zero64 = 0;
    unsigned char zero8 = 0;
    mk_write((void*)(state + F_REQUESTED), &zero32, 4);
    mk_write((void*)(state + F_SPECIAL), &zero8, 1);
    mk_write((void*)(state + F_WAIT_TICKS), &zero64, 8);
}

static void __fastcall cb_mark_cancel_request(uintptr_t state)
{
    StateView v;
    g_af.fishing.cancel_requested_state = state;
    state_view_reset(&v);
    state_view_read(state, &v);
    state_view_record("cancel-request marked", state, &v);
}

static BOOL __fastcall cb_observe_success_gate(uintptr_t state)
{
    StateView v;
    state_view_reset(&v);
    state_view_read(state, &v);
    log_hot("[af] pre-animation gate object=%p current=%u requested=%u "
            "valid_spot=%d task=%p; native preserved", (void*)state,
            v.current, v.requested, v.valid_spot ? 1 : 0, (void*)v.task);
    return FALSE;
}

static BOOL __fastcall cb_rearm_common_exit(uintptr_t state)
{
    StateView v;
    BOOL explicit_cancel;

    if (!g_af.fishing.enabled) return FALSE;

    state_view_reset(&v);
    if (!state_view_read(state, &v)) {
        log_hot("[af] post-animation rejected invalid_state object=%p",
                (void*)state);
        return FALSE;
    }

    explicit_cancel = (g_af.fishing.cancel_requested_state == state);
    log_hot("[af] post-animation exit object=%p cancel=%d current=%u "
            "requested=%u valid_spot=%d task=%p", (void*)state,
            explicit_cancel ? 1 : 0, v.current, v.requested,
            v.valid_spot ? 1 : 0, (void*)v.task);
    if (explicit_cancel) {
        g_af.fishing.cancel_requested_state = 0;
        log_hot("[af] cancel native exit preserved; loop stopped");
        return FALSE;
    }

    if (!g_af.vt_state || v.vtable != g_af.vt_state || !v.valid_spot ||
        v.task != 0) {
        log_hot("[af] post-animation rejected mismatch vtable_rva=0x%llx "
                "current=%u requested=%u valid_spot=%d task=%p",
                (unsigned long long)(g_af.base && v.vtable >= g_af.base ?
                                     v.vtable - g_af.base : 0),
                v.current, v.requested, v.valid_spot ? 1 : 0, (void*)v.task);
        return FALSE;
    }

    action_clear_request(state);
    g_af.fishing.loop_count++;
    log_hot("[af] loop rearmed cycle=%llu requested=0 task=null",
            (unsigned long long)g_af.fishing.loop_count);
    return TRUE;
}

static BOOL __fastcall cb_rearm_after_catch(uintptr_t state)
{
    StateView v;

    if (!g_af.fishing.enabled) return FALSE;

    state_view_reset(&v);
    if (!state_view_read(state, &v)) {
        log_hot("[af] catch-animation-finish rejected invalid_state");
        return FALSE;
    }
    state_view_record("catch-animation-finish", state, &v);

    if (!g_af.vt_state || v.vtable != g_af.vt_state || !v.valid_spot ||
        v.task != 0 || v.current != 4) {
        log_hot("[af] catch-animation-finish rejected mismatch current=%u",
                v.current);
        return FALSE;
    }

    action_clear_request(state);
    g_af.fishing.cancel_requested_state = 0;
    g_af.fishing.loop_count++;
    log_hot("[af] loop rearmed cycle=%llu source=phase4-finish",
            (unsigned long long)g_af.fishing.loop_count);
    return TRUE;
}

static BOOL __fastcall cb_monitor_detour(uintptr_t state, void* frame_context)
{
    StateView before, after;
    BOOL result = FALSE;

    state_view_reset(&before);
    if (state_view_read(state, &before) &&
        (state != g_af.fishing.last_monitor_state ||
         before.current != g_af.fishing.last_monitor_current ||
         before.requested != g_af.fishing.last_monitor_requested)) {
        state_view_record("state-before", state, &before);
        g_af.fishing.last_monitor_state = state;
        g_af.fishing.last_monitor_current = before.current;
        g_af.fishing.last_monitor_requested = before.requested;
    }

    if (g_af.fishing.original_update) {
        typedef BOOL (__fastcall *UpdateFn)(uintptr_t, void*);
        UpdateFn fn = (UpdateFn)g_af.fishing.original_update;
        result = fn(state, frame_context);
    }

    state_view_reset(&after);
    if (state_view_read(state, &after) &&
        (after.current != before.current ||
         after.requested != before.requested)) {
        log_hot("[af] state-after object=%p before=%u/%u after=%u/%u "
                "task=%p result=%d", (void*)state, before.current,
                before.requested, after.current, after.requested,
                (void*)after.task, result ? 1 : 0);
    }
    return result;
}

/* 三个 NOP 补丁点开关（事务式：任一写入失败整体回滚） */
static BOOL fishing_set(BOOL enable)
{
    if (!g_af.ready) return FALSE;

    if (!g_af.fishing.available) {
        unsigned char buf[8];
        BOOL ok = TRUE;
        uintptr_t reel = update_addr(OFF_AUTO_REEL);

        if (g_af.patch_jz &&
            (!mk_read(buf, (const void*)g_af.patch_jz, 6) ||
             (memcmp(buf, kExpectJz6, 6) != 0 && memcmp(buf, kNops, 6) != 0))) {
            log_direct("[af] settle-gate byte mismatch; fishing disabled");
            ok = FALSE;
        }
        if (g_af.patch_jnz &&
            (!mk_read(buf, (const void*)g_af.patch_jnz, 6) ||
             (memcmp(buf, kExpectJnz6, 6) != 0 && memcmp(buf, kNops, 6) != 0))) {
            log_direct("[af] note-hit byte mismatch; fishing disabled");
            ok = FALSE;
        }
        if (!mk_read(buf, (const void*)reel, 2) ||
            (memcmp(buf, kExpectReel2, 2) != 0 && memcmp(buf, kNops, 2) != 0)) {
            log_direct("[af] auto-reel byte mismatch; fishing disabled");
            ok = FALSE;
        }
        if (!ok) return FALSE;

        if (!hook_install_loop() || !hook_install_post_catch() ||
            !hook_install_monitor()) {
            log_direct("[af] hook chain incomplete; fishing disabled");
            g_af.fishing.available = 0;
            return FALSE;
        }
        g_af.fishing.available = 1;
    }

    if (!g_af.fishing.loop_ready || !g_af.fishing.post_catch_ready ||
        !g_af.fishing.monitor_ready)
        return FALSE;

    {
        HookTxn txn;
        struct {
            uintptr_t            addr;
            int                  len;
            const unsigned char* orig;
        } pts[3];
        int npts = 0;
        int i;

        if (g_af.patch_jz) {
            pts[npts].addr = g_af.patch_jz;
            pts[npts].len = 6;
            pts[npts].orig = kExpectJz6;
            npts++;
        }
        if (g_af.patch_jnz) {
            pts[npts].addr = g_af.patch_jnz;
            pts[npts].len = 6;
            pts[npts].orig = kExpectJnz6;
            npts++;
        }
        pts[npts].addr = update_addr(OFF_AUTO_REEL);
        pts[npts].len = 2;
        pts[npts].orig = kExpectReel2;
        npts++;

        hook_txn_begin(&txn, "af.patch");
        for (i = 0; i < npts; i++) {
            unsigned char cur[8] = { 0 };
            const unsigned char* src;
            BOOL already;
            mk_read(cur, (const void*)pts[i].addr, (size_t)pts[i].len);
            already = (memcmp(cur, kNops, (size_t)pts[i].len) == 0);
            if (already && enable) continue;   /* 已 NOP，重复开启跳过 */
            src = enable ? kNops : (already ? pts[i].orig : cur);
            if (!hook_txn_write(&txn, pts[i].addr, src, pts[i].len)) {
                hook_txn_rollback(&txn);
                log_direct("[af] base-patch transaction failed index=%d; "
                           "rollback", i);
                return FALSE;
            }
        }
        hook_txn_commit(&txn);
    }

    g_af.fishing.enabled = enable ? 1 : 0;
    log_direct("[af] %s continuous_recast=%s", enable ? "ENABLED" : "disabled",
               (g_af.fishing.loop_ready && g_af.fishing.post_catch_ready)
                   ? "ready" : "unavailable");
    return TRUE;
}

/* ==================================================================
 * 6 钩子装配（stub 机器码与 1.08.1 指令布局一一对应）
 * ================================================================== */
static BOOL hook_install_loop(void)
{
    HookTxn   txn;
    HookPoint pts[7];
    uintptr_t base, update, hook_point, success_gate, cancel_point;
    uintptr_t stub, direct_stub, cancel_stub;
    int i;

    if (g_af.fishing.loop_ready) return TRUE;

    base = g_af.base;
    update = g_af.update_rva;
    hook_point = base + update + OFF_COMMON_EXIT;
    success_gate = base + update + OFF_SUCCESS_GATE;
    cancel_point = base + update + OFF_CANCEL_REQUEST;

    pts[0] = (HookPoint){ "success_exit_jump",
                          base + update + OFF_SUCCESS_EXIT_JUMP,
                          kExpectSuccessJump, 5 };
    pts[1] = (HookPoint){ "direct_null_exit",
                          base + update + OFF_DIRECT_NULL_EXIT,
                          kExpectDirectNullExit, 6 };
    pts[2] = (HookPoint){ "success_gate", success_gate,
                          kExpectSuccessGate, 7 };
    pts[3] = (HookPoint){ "common_exit", hook_point, kExpectCommonExit, 7 };
    pts[4] = (HookPoint){ "state_dispatch", base + update + OFF_DISPATCH,
                          kExpectDispatch, 12 };
    pts[5] = (HookPoint){ "success_phase", base + update + OFF_SUCCESS_PHASE,
                          kExpectSuccessPhase, 10 };
    pts[6] = (HookPoint){ "cancel_phase", cancel_point, kExpectCancelPhase, 11 };
    for (i = 0; i < 7; i++)
        if (!hook_point_verify(&pts[i])) return FALSE;

    hook_txn_begin(&txn, "af.loop");
    stub = hook_txn_stub(&txn, hook_point, 96);
    if (!stub) { hook_txn_rollback(&txn); return FALSE; }
    direct_stub = hook_txn_stub(&txn, success_gate, 96);
    if (!direct_stub) { hook_txn_rollback(&txn); return FALSE; }
    cancel_stub = hook_txn_stub(&txn, cancel_point, 96);
    if (!cancel_stub) { hook_txn_rollback(&txn); return FALSE; }

    /* common-exit stub：mov rcx,r14; sub rsp,0x20; call helper;
       test al,al; je native; jmp epilogue; native: mov rax,[game_root];
       jmp continue */
    {
        unsigned char code[68] = {
            0x4C, 0x89, 0xF1,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0xFF, 0xD0,
            0x48, 0x83, 0xC4, 0x20,
            0x84, 0xC0,
            0x74, 0x0E,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0x48, 0x8B, 0x00,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0
        };
        uintptr_t helper = (uintptr_t)&cb_rearm_common_exit;
        uintptr_t epi = base + update + OFF_UPDATE_EPILOGUE;
        uintptr_t gslot = g_af.clock;
        uintptr_t cont = base + update + OFF_COMMON_EXIT_CONT;
        memcpy(code + 9, &helper, 8);
        memcpy(code + 33, &epi, 8);
        memcpy(code + 43, &gslot, 8);
        memcpy(code + 60, &cont, 8);
        memcpy((void*)stub, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), (void*)stub, sizeof(code));
    }

    /* success-gate stub */
    {
        unsigned char code[62] = {
            0x4C, 0x89, 0xF1,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0xFF, 0xD0,
            0x48, 0x83, 0xC4, 0x20,
            0x84, 0xC0,
            0x74, 0x0E,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0,
            0x49, 0x8B, 0x8E, 0x90, 0x02, 0x00, 0x00,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0
        };
        uintptr_t helper = (uintptr_t)&cb_observe_success_gate;
        uintptr_t epi = base + update + OFF_UPDATE_EPILOGUE;
        uintptr_t cont = base + update + OFF_SUCCESS_GATE_CONT;
        memcpy(code + 9, &helper, 8);
        memcpy(code + 33, &epi, 8);
        memcpy(code + 54, &cont, 8);
        memcpy((void*)direct_stub, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), (void*)direct_stub,
                              sizeof(code));
    }

    /* cancel stub：记录后重放 11 字节写入并继续 */
    {
        unsigned char code[48] = {
            0x4C, 0x89, 0xF1,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0xFF, 0xD0,
            0x48, 0x83, 0xC4, 0x20,
            0x41, 0xC7, 0x86, 0xDC, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0
        };
        uintptr_t helper = (uintptr_t)&cb_mark_cancel_request;
        uintptr_t cont = base + update + OFF_CANCEL_CONTINUE;
        memcpy(code + 9, &helper, 8);
        memcpy(code + 40, &cont, 8);
        memcpy((void*)cancel_stub, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), (void*)cancel_stub,
                              sizeof(code));
    }

    if (!mk_seal_exec(stub, 96) || !mk_seal_exec(direct_stub, 96) ||
        !mk_seal_exec(cancel_stub, 96)) {
        hook_txn_rollback(&txn);
        return FALSE;
    }

    /* 重定向写入顺序与原实现一致：cancel → success-gate → common-exit */
    {
        unsigned char redirect[11];
        hook_make_jmp32(redirect, (int)sizeof(redirect), cancel_point,
                        cancel_stub);
        if (!hook_txn_write(&txn, cancel_point, redirect,
                            (int)sizeof(redirect))) {
            hook_txn_rollback(&txn);
            return FALSE;
        }
    }
    {
        unsigned char redirect[7];
        hook_make_jmp32(redirect, (int)sizeof(redirect), success_gate,
                        direct_stub);
        if (!hook_txn_write(&txn, success_gate, redirect,
                            (int)sizeof(redirect))) {
            hook_txn_rollback(&txn);
            return FALSE;
        }
    }
    {
        unsigned char redirect[7];
        hook_make_jmp32(redirect, (int)sizeof(redirect), hook_point, stub);
        if (!hook_txn_write(&txn, hook_point, redirect,
                            (int)sizeof(redirect))) {
            hook_txn_rollback(&txn);
            return FALSE;
        }
    }
    hook_txn_commit(&txn);

    g_af.fishing.loop_stub = stub;
    g_af.fishing.success_stub = direct_stub;
    g_af.fishing.cancel_stub = cancel_stub;
    g_af.fishing.loop_ready = 1;
    log_direct("[af] loop hooks installed common=0x%X gate=0x%X cancel=0x%X",
               (unsigned)(update + OFF_COMMON_EXIT),
               (unsigned)(update + OFF_SUCCESS_GATE),
               (unsigned)(update + OFF_CANCEL_REQUEST));
    return TRUE;
}

static BOOL hook_install_post_catch(void)
{
    HookTxn   txn;
    HookPoint pt;
    uintptr_t target, stub;

    if (g_af.fishing.post_catch_ready) return TRUE;
    target = update_addr(OFF_PHASE4_FINISH);
    pt = (HookPoint){ "phase4_finish", target, kExpectPhase4Finish, 8 };
    if (!hook_point_verify(&pt)) return FALSE;

    hook_txn_begin(&txn, "af.post_catch");
    stub = hook_txn_stub(&txn, target, 96);
    if (!stub) { hook_txn_rollback(&txn); return FALSE; }

    {
        unsigned char code[70] = {
            0x4C, 0x89, 0xF1,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0xFF, 0xD0,
            0x48, 0x83, 0xC4, 0x20,
            0x84, 0xC0,
            0x74, 0x0E,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0,
            0x49, 0x8B, 0xCE,
            0x48, 0xB8, 0,0,0,0,0,0,0,0,
            0xFF, 0xD0,
            0xFF, 0x25, 0,0,0,0, 0,0,0,0,0,0,0,0
        };
        /* native action finish 从 phase4 的 E8 位移运行时解算：
           [49 8B CE][E8 rel32]，rel 在 +4，目标 = target+8+rel */
        int32_t rel = 0;
        uintptr_t native_finish, clean, ncont, helper;

        mk_read(&rel, (const void*)(target + 4), 4);
        native_finish = target + 8 + (uintptr_t)(int32_t)rel;
        helper = (uintptr_t)&cb_rearm_after_catch;
        clean = update_addr(OFF_PHASE4_CLEANUP);
        ncont = update_addr(OFF_PHASE4_NATIVE_CONT);
        memcpy(code + 9, &helper, 8);
        memcpy(code + 33, &clean, 8);
        memcpy(code + 46, &native_finish, 8);
        memcpy(code + 62, &ncont, 8);
        memcpy((void*)stub, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), (void*)stub, sizeof(code));

        if (!mk_seal_exec(stub, 96)) {
            hook_txn_rollback(&txn);
            return FALSE;
        }
        {
            unsigned char redirect[8];
            hook_make_jmp32(redirect, (int)sizeof(redirect), target, stub);
            if (!hook_txn_write(&txn, target, redirect,
                                (int)sizeof(redirect))) {
                hook_txn_rollback(&txn);
                return FALSE;
            }
        }
        hook_txn_commit(&txn);

        g_af.fishing.post_catch_stub = stub;
        g_af.fishing.post_catch_ready = 1;
        log_direct("[af] post-catch hook installed rva=0x%X "
                   "native_finish=0x%llX",
                   (unsigned)(g_af.update_rva + OFF_PHASE4_FINISH),
                   (unsigned long long)(native_finish - g_af.base));
    }
    return TRUE;
}

static BOOL hook_install_monitor(void)
{
    HookTxn   txn;
    HookPoint pt;
    uintptr_t target, trampoline;

    if (g_af.fishing.monitor_ready) return TRUE;
    target = update_addr(0);
    pt = (HookPoint){ "update_entry", target, kExpectUpdateEntry, 15 };
    if (!hook_point_verify(&pt)) return FALSE;

    hook_txn_begin(&txn, "af.monitor");
    trampoline = hook_txn_alloc(&txn, 64);
    if (!trampoline) { hook_txn_rollback(&txn); return FALSE; }

    memcpy((void*)trampoline, kExpectUpdateEntry, 15);
    hook_make_jmp_abs((unsigned char*)(trampoline + 15), target + 15);
    FlushInstructionCache(GetCurrentProcess(), (void*)trampoline, 64);
    if (!mk_seal_exec(trampoline, 64)) {
        hook_txn_rollback(&txn);
        return FALSE;
    }
    g_af.fishing.original_update = trampoline;

    {
        unsigned char hook[15];
        hook_make_jmp_abs(hook, (uintptr_t)&cb_monitor_detour);
        hook[14] = 0x90;
        if (!hook_txn_write(&txn, target, hook, (int)sizeof(hook))) {
            hook_txn_rollback(&txn);
            g_af.fishing.original_update = 0;
            return FALSE;
        }
    }
    hook_txn_commit(&txn);

    g_af.fishing.monitor_ready = 1;
    log_direct("[af] monitor hook installed update_rva=0x%X",
               (unsigned)g_af.update_rva);
    return TRUE;
}

/* ==================================================================
 * 7 垃圾过滤（钓鱼候选池排除）
 * ================================================================== */
/* 600000 空罐 / 600010 塑料袋 / 600020 水草 / 600030 树枝 */
static const uint32_t kTrashIds[4] = { 600000, 600010, 600020, 600030 };

/* 核对 stub 内嵌的四个立即数（偏移不写死，改动指令序列后仍能自检） */
static BOOL trash_check_ids(const unsigned char* code, int len)
{
    int found = 0;
    int i;
    for (i = 0; i + 7 <= len; i++) {
        if (code[i] != 0x41 || code[i + 1] != 0x81 || code[i + 2] != 0xFB)
            continue;
        {
            uint32_t v = 0;
            int k;
            memcpy(&v, code + i + 3, 4);
            for (k = 0; k < 4; k++) {
                if (v == kTrashIds[k]) { found++; break; }
            }
        }
    }
    return found == 4;
}

static BOOL trash_install(void)
{
    HookTxn   txn;
    uintptr_t target, stub;
    unsigned char code[192];
    int o = 0;

    if (g_af.trash.ready) return TRUE;
    target = update_addr(OFF_NO_TRASH_APPEND);

    {
        unsigned char buf[8];
        if (!mk_read(buf, (const void*)target, 5) ||
            memcmp(buf, kExpectAppend, 5) != 0) {
            log_direct("[af] no-trash anchor mismatch at 0x%X; "
                       "filter disabled",
                       (unsigned)(g_af.update_rva + OFF_NO_TRASH_APPEND));
            return FALSE;
        }
    }

    hook_txn_begin(&txn, "af.trash");
    stub = hook_txn_stub(&txn, target, 192);
    if (!stub) { hook_txn_rollback(&txn); return FALSE; }

    memset(code, 0x90, sizeof(code));
    code[o++] = 0x48; code[o++] = 0x8B; code[o++] = 0x03;   /* mov rax,[rbx] */
    code[o++] = 0x44; code[o++] = 0x8B; code[o++] = 0x18;   /* mov r11d,[rax] */
    code[o++] = 0x48; code[o++] = 0xB9;                     /* mov rcx,&exec */
    {
        uintptr_t addr = (uintptr_t)&g_af.trash.exec_count;
        memcpy(code + o, &addr, 8);
    }
    o += 8;
    code[o++] = 0xF0; code[o++] = 0xFF; code[o++] = 0x01;   /* lock inc [rcx] */
    /* 垃圾 ID 比较（立即数顺序与 kTrashIds 一致） */
    code[o++] = 0x41; code[o++] = 0x81; code[o++] = 0xFB;
    code[o++] = 0xC0; code[o++] = 0x27; code[o++] = 0x09; code[o++] = 0x00;
    code[o++] = 0x74; code[o++] = 0x2E;                     /* je GARBAGE */
    code[o++] = 0x41; code[o++] = 0x81; code[o++] = 0xFB;
    code[o++] = 0xCA; code[o++] = 0x27; code[o++] = 0x09; code[o++] = 0x00;
    code[o++] = 0x74; code[o++] = 0x25;                     /* je GARBAGE */
    code[o++] = 0x41; code[o++] = 0x81; code[o++] = 0xFB;
    code[o++] = 0xD4; code[o++] = 0x27; code[o++] = 0x09; code[o++] = 0x00;
    code[o++] = 0x74; code[o++] = 0x1C;                     /* je GARBAGE */
    code[o++] = 0x41; code[o++] = 0x81; code[o++] = 0xFB;
    code[o++] = 0xDE; code[o++] = 0x27; code[o++] = 0x09; code[o++] = 0x00;
    code[o++] = 0x74; code[o++] = 0x13;                     /* je GARBAGE */
    code[o++] = 0x45; code[o++] = 0x85; code[o++] = 0xFF;   /* test r15d,r15d */
    code[o++] = 0x7E; code[o++] = 0x1E;                     /* jle SKIP_RET */
    code[o++] = 0xFF; code[o++] = 0x25;                     /* jmp [rip+0] */
    code[o++] = 0; code[o++] = 0; code[o++] = 0; code[o++] = 0;
    {
        uintptr_t addr = update_addr(OFF_NO_TRASH_APPEND + 5);
        memcpy(code + o, &addr, 8);
    }
    o += 8;
    /* GARBAGE: 补偿 SUB ESI,R15D + 计数跳过 */
    code[o++] = 0x44; code[o++] = 0x29; code[o++] = 0xFE;   /* sub esi,r15d */
    code[o++] = 0x48; code[o++] = 0xB9;                     /* mov rcx,&skip */
    {
        uintptr_t addr = (uintptr_t)&g_af.trash.skip_count;
        memcpy(code + o, &addr, 8);
    }
    o += 8;
    code[o++] = 0xF0; code[o++] = 0xFF; code[o++] = 0x01;   /* lock inc [rcx] */
    /* SKIP_RET: 跳追加点 +0x51，不 count++ / 不加权重 */
    code[o++] = 0xFF; code[o++] = 0x25;
    code[o++] = 0; code[o++] = 0; code[o++] = 0; code[o++] = 0;
    {
        uintptr_t addr = update_addr(OFF_NO_TRASH_APPEND + 0x51);
        memcpy(code + o, &addr, 8);
    }
    o += 8;
    /* --- 发射结束 --- */

    if (!trash_check_ids(code, o)) {
        log_direct("[af] no-trash stub id mismatch; filter disabled");
        hook_txn_rollback(&txn);
        return FALSE;
    }

    memcpy((void*)stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), (void*)stub, sizeof(code));
    if (!mk_seal_exec(stub, 192)) {
        hook_txn_rollback(&txn);
        return FALSE;
    }

    {
        unsigned char redirect[5];
        hook_make_jmp32(redirect, (int)sizeof(redirect), target, stub);
        if (!hook_txn_write(&txn, target, redirect, (int)sizeof(redirect))) {
            hook_txn_rollback(&txn);
            return FALSE;
        }
    }
    hook_txn_commit(&txn);

    g_af.trash.stub = stub;
    g_af.trash.ready = 1;
    log_direct("[af] no-trash hook ready at 0x%X",
               (unsigned)(g_af.update_rva + OFF_NO_TRASH_APPEND));
    return TRUE;
}

static BOOL trash_set(BOOL on)
{
    uintptr_t target;
    if (!g_af.trash.ready) return FALSE;
    target = update_addr(OFF_NO_TRASH_APPEND);

    if (on) {
        unsigned char redirect[5];
        hook_make_jmp32(redirect, (int)sizeof(redirect), target,
                        g_af.trash.stub);
        if (!mk_write((void*)target, redirect, (int)sizeof(redirect)))
            return FALSE;
    } else {
        if (!mk_write((void*)target, kExpectAppend, 5)) return FALSE;
    }
    g_af.trash.active = on ? 1 : 0;

    if (on) {
        InterlockedExchange(&g_af.trash.exec_count, 0);
        InterlockedExchange(&g_af.trash.skip_count, 0);
        log_direct("[af] no-trash ON (junk filter active)");
        ui_toast(L"垃圾过滤：已开启", RGB(91, 192, 122));
    } else {
        log_direct("[af] no-trash OFF (exec=%ld skip=%ld)",
                   (long)g_af.trash.exec_count,
                   (long)g_af.trash.skip_count);
        ui_toast(L"垃圾过滤：已关闭", RGB(80, 155, 220));
    }
    return TRUE;
}

static void trash_toggle(void)
{
    if (g_af.trash.active) {
        trash_set(FALSE);
        return;
    }
    if (!g_af.trash.ready) trash_install();
    trash_set(TRUE);
}

/* ==================================================================
 * 8 输入与 UI
 * ================================================================== */
static const wchar_t TOAST_CLASS[] = L"AutoFishToast";

static LRESULT CALLBACK toast_wndproc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps = { 0 };
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = { 0 };
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 2, g_af.ui.accent);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 1, 1, rc.right - 1, rc.bottom - 1);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        HGDIOBJ old_font = SelectObject(dc, g_af.ui.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 240));
        RECT tr = { 16, 8, rc.right - 12, rc.bottom - 8 };
        DrawTextW(dc, g_af.ui.text, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static int toast_init(void)
{
    WNDCLASSEXW cls = { 0 };
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = toast_wndproc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = TOAST_CLASS;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    g_af.ui.font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
    if (!g_af.ui.font)
        g_af.ui.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_af.ui.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        TOAST_CLASS, L"", WS_POPUP, 0, 0, 420, 58,
        NULL, NULL, cls.hInstance, NULL);
    if (!g_af.ui.hwnd) return 0;
    SetLayeredWindowAttributes(g_af.ui.hwnd, 0, 238, LWA_ALPHA);
    return 1;
}

static void ui_toast(const wchar_t* message, COLORREF accent)
{
    MONITORINFO mi = { 0 };
    HMONITOR mon;
    int x, y;

    if (!g_af.ui.hwnd && !toast_init()) return;
    wcsncpy_s(g_af.ui.text, TOAST_TEXT_MAX, message, _TRUNCATE);
    g_af.ui.accent = accent;
    mi.cbSize = sizeof(mi);
    mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    x = mi.rcWork.right - 420 - 24;
    y = mi.rcWork.bottom - 58 - 24;
    SetWindowPos(g_af.ui.hwnd, HWND_TOPMOST, x, y, 420, 58,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_af.ui.hwnd, NULL, TRUE);
    UpdateWindow(g_af.ui.hwnd);
    InterlockedExchange64((volatile LONG64*)&g_af.ui.hide_at,
                          (LONG64)(GetTickCount64() + 1800));
}

static void ui_pump(void)
{
    MSG msg = { 0 };
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_af.ui.hwnd && IsWindowVisible(g_af.ui.hwnd)) {
        ULONGLONG hide_at = (ULONGLONG)InterlockedCompareExchange64(
            (volatile LONG64*)&g_af.ui.hide_at, 0, 0);
        if (hide_at != 0 && GetTickCount64() >= hide_at) {
            ShowWindow(g_af.ui.hwnd, SW_HIDE);
            InterlockedExchange64((volatile LONG64*)&g_af.ui.hide_at, 0);
        }
    }
}

/* 低级键盘钩子回调（纯观察，不拦截） */
static LRESULT CALLBACK kbd_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT* kh = (KBDLLHOOKSTRUCT*)lp;
        int vk = (int)kh->vkCode & 0xFF;
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            InterlockedExchange(&g_af.input.down[vk], 1);
            InterlockedExchange(&g_af.input.latch[vk], 1);
        } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            InterlockedExchange(&g_af.input.down[vk], 0);
        }
    }
    return CallNextHookEx(g_af.input.hook, code, wp, lp);
}

static DWORD WINAPI kbd_thread(LPVOID param)
{
    MSG msg;
    (void)param;
    g_af.input.thread_id = GetCurrentThreadId();
    g_af.input.hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_hook,
                                        GetModuleHandleA("AutoFish.dll"), 0);
    /* 入缓冲而非直写：日志文件句柄只由 mod_tick 线程落盘 */
    log_hot("[af] kbd hook %s", g_af.input.hook ? "installed" : "failed");
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_af.input.hook) {
        UnhookWindowsHookEx(g_af.input.hook);
        g_af.input.hook = NULL;
    }
    return 0;
}

static void input_start(void)
{
    if (g_af.input.thread_id) return;
    CreateThread(NULL, 0, kbd_thread, NULL, 0, NULL);
}

static void input_stop(void)
{
    if (g_af.input.thread_id)
        PostThreadMessageA(g_af.input.thread_id, WM_QUIT, 0, 0);
}

/* 读键：钩子按下边沿锁存 / 钩子按住状态 / GetAsyncKeyState 兜底 */
static int key_pressed(int vk)
{
    if (vk < 0 || vk > 255) return 0;
    if (InterlockedExchange(&g_af.input.latch[vk], 0)) return 1;
    if (g_af.input.down[vk]) return 1;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* mod_tick 每帧调用：F10 边沿 → 垃圾过滤开关 */
static void input_poll(void)
{
    int down = key_pressed(g_af.input.toggle_key);
    if (down && !g_af.input.prev_state) {
        if (g_af.trash.active)
            trash_set(FALSE);
        else
            trash_toggle();
    }
    g_af.input.prev_state = down;
}

/* ==================================================================
 * 9 生命周期
 * ================================================================== */
__declspec(dllexport) void mod_init(void)
{
    ctx_init();
    log_init();
    hook_set_logger(log_direct);

    log_direct("[af] AutoFish v0.1.5 loaded; base=%p", (void*)g_af.base);
    if (!load_profile()) {
        log_direct("[af] FATAL: addrsig MISS, mod disabled");
        return;
    }
    ctx_load_config();
    fishing_set(g_af.enabled ? TRUE : FALSE);
    input_start();
    log_direct("[af] no-trash off by default; press F10 to toggle");
    log_direct("[af] no-trash exec=%p skip=%p",
               (void*)&g_af.trash.exec_count, (void*)&g_af.trash.skip_count);
}

__declspec(dllexport) void mod_tick(void)
{
    ui_pump();
    input_poll();
    log_tick();
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (g_af.trash.active) trash_set(FALSE);
        input_stop();
        if (g_af.fishing.enabled) fishing_set(FALSE);
        log_close();
    }
    return TRUE;
}
