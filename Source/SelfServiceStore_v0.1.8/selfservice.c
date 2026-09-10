/* selfservice.c - SelfServiceStore v0.1.8（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文 / 5 商店判定
 *       / 6 跳板与安装 / 7 工作线程 / 8 生命周期
 *
 * 功能：杂货店 / 树木建设 / 狩猎小屋 / 料理屋 / 图书馆 全天营业——
 * shopOpen / Open / shutOpen / doorOpen 区域状态在 07:00-22:59 恒为 1。
 *
 * 原理：hook 通用“设置区域状态”函数入口（rcx=地图对象, rdx=键ID,
 * r8=键名, r9d=值），键名属商店类且处于营业时段时强制 r9d=1；
 * 其余时段放行游戏原值。地址由 addrsig 运行时解析，MISS 时停用。
 *
 * 编译（在 SelfServiceStore_v0.1.8 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o selfservice.dll
 *     selfservice.c ..\shared\addrsig.c ..\shared\modkit.c -luser32
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
#define PATCH_LEN       12       /* mov rax,imm64; jmp rax */
#define TIME_B_OFF      0x3270   /* 时钟字段（QWORD 累计秒） */
#define DISPLAY_OFFSET  7        /* hour = (b/3600 + 7) % 24 */
#define OPEN_HOUR_FROM  7        /* 营业时段 07:00-22:59 */
#define OPEN_HOUR_TO    22
#define DIAG_LIMIT      10       /* 前 N 次商店键写入打诊断日志 */

#define SIG_HOOK   "store_hook_0x27B1E0"
#define SIG_CLOCK  "clock_0x10CD990"

static const AddrSig kSigs[] = {
    { SIG_HOOK, ADDRSIG_FUNC,
      "48895c240848896c24205657415441564157b880100000e800000000482be0458be1498be8000000008b5930488b9b60",
      "010101010101010101010101010101010101010101010101000000000101010101010101010000000001010101010101", 0 },
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_HOOK,  0, 0, 0 },
    { SIG_CLOCK, 0, 0, 0 },
};

/* 入口序言原文（14 字节，安装前校验） */
static const unsigned char kExpectPrologue[14] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x48, 0x89, 0x6C, 0x24, 0x20,
    0x56, 0x57, 0x41, 0x54
};

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    uintptr_t     hook;        /* 目标函数入口（addrsig 解析） */
    uintptr_t     clock;       /* 时钟全局（addrsig 解析） */
    uintptr_t     back;        /* 跳回原函数的位置 */
    uintptr_t     stub;        /* 运行时构建的跳板 */
    volatile LONG diag_n;      /* 诊断日志计数 */
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    LogKit        log;
    GameProfile   profile;
} StoreContext;

static StoreContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "store", TRUE);
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
    lstrcatA(g_ctx.log_path, "selfservice.log");
}

static BOOL load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "store";
    g_ctx.profile.base = g_ctx.base;
    g_ctx.profile.sigs = kSigs;
    g_ctx.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_ctx.profile.res = s_res;
    g_ctx.profile.entries = s_entries;
    g_ctx.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_ctx.profile);
    g_ctx.hook = profile_addr(&g_ctx.profile, SIG_HOOK);
    g_ctx.clock = profile_addr(&g_ctx.profile, SIG_CLOCK);
    return (g_ctx.hook && g_ctx.clock) ? TRUE : FALSE;
}

/* ==================================================================
 * 5 商店判定
 * ================================================================== */
static uintptr_t get_clock_base(void)
{
    uintptr_t g = 0, d = 0;
    if (!g_ctx.clock) return 0;
    if (!mk_read(&g, (const void*)g_ctx.clock, 8) || g < 0x10000) return 0;
    if (!mk_read(&d, (const void*)(g + 0x208), 8) || d < 0x10000) return 0;
    return d;
}

/* 当前游戏小时（0-23）；读不到返回 -1 */
static int get_hour(void)
{
    uintptr_t clock = get_clock_base();
    uint64_t b = 0;
    if (!clock) return -1;
    if (!mk_read(&b, (const void*)(clock + TIME_B_OFF), 8)) return -1;
    return (int)((b / 3600 + DISPLAY_OFFSET) % 24);
}

/* 安全读取键名并判断是否商店类（逐字节、遇 \0 停，越界由 __try 捕获） */
static int key_is_shop(const char* k)
{
    char buf[9];
    int n = 0;
    int shop = 0;
    int hour;

    if ((uintptr_t)k < 0x10000) return 0;
    __try {
        for (n = 0; n < 8; n++) {
            buf[n] = k[n];
            if (buf[n] == 0) break;
        }
        buf[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (strcmp(buf, "Open") == 0) shop = 1;
    else if (strcmp(buf, "shopOpen") == 0) shop = 1;
    else if (strcmp(buf, "shutOpen") == 0) shop = 1;
    else if (strcmp(buf, "doorOpen") == 0) shop = 1;
    if (!shop) return 0;                 /* 非商店键：不强制 */
    hour = get_hour();
    if (hour < 0) return 0;              /* 读不到时间：放行，不干预 */
    if (InterlockedIncrement(&g_ctx.diag_n) <= DIAG_LIMIT) {
        log_direct("[store] diag key='%s' hour=%d -> %s", buf, hour,
                   (hour >= OPEN_HOUR_FROM && hour <= OPEN_HOUR_TO)
                       ? "open(1)" : "pass(orig)");
    }
    if (hour >= OPEN_HOUR_FROM && hour <= OPEN_HOUR_TO) return 1;
    return 0;                            /* 其余时段放行，游戏原版 */
}

/* ==================================================================
 * 6 跳板与安装
 * ================================================================== */
/* 跳板：重放被覆盖的 12 字节序言 → 保存参数 → 调用 key_is_shop →
 * 营业时段强制 [rsp]=1 → 恢复 → jmp 回原函数 +PATCH_LEN */
static BOOL build_stub(void)
{
    HookTxn txn;
    unsigned char* m;
    size_t o = 0;
    size_t fnpos, backpos;
    uint64_t fn, back;

    hook_txn_begin(&txn, "store.stub");
    g_ctx.stub = hook_txn_alloc(&txn, 0x100);
    if (!g_ctx.stub) {
        hook_txn_rollback(&txn);
        return FALSE;
    }
    m = (unsigned char*)g_ctx.stub;
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x5C; m[o++] = 0x24; m[o++] = 0x08; /* mov [rsp+8],rbx */
    m[o++] = 0x48; m[o++] = 0x89; m[o++] = 0x6C; m[o++] = 0x24; m[o++] = 0x20; /* mov [rsp+20],rbp */
    m[o++] = 0x56;                                   /* push rsi */
    m[o++] = 0x57;                                   /* push rdi */
    m[o++] = 0x51;                                   /* push rcx */
    m[o++] = 0x52;                                   /* push rdx */
    m[o++] = 0x41; m[o++] = 0x50;                    /* push r8 */
    m[o++] = 0x41; m[o++] = 0x51;                    /* push r9 */
    m[o++] = 0x49; m[o++] = 0x8B; m[o++] = 0xC8;     /* mov rcx, r8 */
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xEC; m[o++] = 0x28; /* sub rsp,40 */
    m[o++] = 0x48; m[o++] = 0xB8;                    /* mov rax, key_is_shop */
    fnpos = o;
    o += 8;
    m[o++] = 0xFF; m[o++] = 0xD0;                    /* call rax */
    m[o++] = 0x48; m[o++] = 0x83; m[o++] = 0xC4; m[o++] = 0x28; /* add rsp,40 */
    m[o++] = 0x85; m[o++] = 0xC0;                    /* test eax,eax */
    m[o++] = 0x74; m[o++] = 0x07;                    /* je +7：放行，跳到 pop r9 */
    m[o++] = 0xC7; m[o++] = 0x04; m[o++] = 0x24;
    m[o++] = 0x01; m[o++] = 0x00; m[o++] = 0x00; m[o++] = 0x00; /* mov dword [rsp],1 */
    m[o++] = 0x41; m[o++] = 0x59;                    /* pop r9 */
    m[o++] = 0x41; m[o++] = 0x58;                    /* pop r8 */
    m[o++] = 0x5A;                                   /* pop rdx */
    m[o++] = 0x59;                                   /* pop rcx */
    m[o++] = 0x48; m[o++] = 0xB8;                    /* mov rax, imm64 */
    backpos = o;
    o += 8;
    m[o++] = 0xFF; m[o++] = 0xE0;                    /* jmp rax */

    fn = (uint64_t)(uintptr_t)key_is_shop;
    memcpy(m + fnpos, &fn, 8);
    back = (uint64_t)g_ctx.back;
    memcpy(m + backpos, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), m, o);
    if (!mk_seal_exec(g_ctx.stub, 0x100)) {
        hook_txn_rollback(&txn);
        g_ctx.stub = 0;
        return FALSE;
    }
    hook_txn_commit(&txn);
    return TRUE;
}

/* 安装入口跳转：12 字节绝对跳转（±2GB 限制无关） */
static BOOL install_hook(void)
{
    HookTxn txn;
    HookPoint pt;
    unsigned char patch[PATCH_LEN];
    unsigned char probe[14] = { 0 };

    pt = (HookPoint){ "store_prologue", g_ctx.hook, kExpectPrologue, 14 };
    if (!hook_point_verify(&pt)) {
        if (mk_read(probe, (const void*)g_ctx.hook, sizeof(probe)))
            log_direct("[store] ERROR: bytes at %p mismatch "
                       "(got %02X %02X %02X %02X %02X), abort",
                       (void*)g_ctx.hook, probe[0], probe[1], probe[2],
                       probe[3], probe[4]);
        return FALSE;
    }

    g_ctx.back = g_ctx.hook + PATCH_LEN;   /* 跳回序言之后（push r12 处） */
    if (!build_stub()) {
        log_direct("[store] ERROR: stub alloc failed");
        return FALSE;
    }

    patch[0] = 0x48;
    patch[1] = 0xB8;                       /* mov rax, imm64 */
    memcpy(patch + 2, &g_ctx.stub, 8);
    patch[10] = 0xFF;
    patch[11] = 0xE0;                      /* jmp rax */

    hook_txn_begin(&txn, "store.hook");
    if (!hook_txn_write(&txn, g_ctx.hook, patch, PATCH_LEN)) {
        hook_txn_rollback(&txn);
        return FALSE;
    }
    hook_txn_commit(&txn);
    return TRUE;
}

/* ==================================================================
 * 7 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    uintptr_t clock;
    uint64_t b = 0;
    int hour = -1;

    (void)param;
    Sleep(1000);
    log_init();
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    load_profile();
    log_direct("[store] SelfServiceStore v0.1.8 loaded; base=%p",
               (void*)g_ctx.base);
    if (!g_ctx.hook || !g_ctx.clock) {
        log_direct("[store] FATAL: addrsig MISS, mod disabled");
        log_close();
        return 0;
    }
    if (!install_hook()) {
        log_close();
        return 0;
    }
    log_direct("[store] hooked %p -> stub %p (back %p), keys: "
               "shopOpen/Open/shutOpen/doorOpen",
               (void*)g_ctx.hook, (void*)g_ctx.stub, (void*)g_ctx.back);

    clock = get_clock_base();
    if (clock) mk_read(&b, (const void*)(clock + TIME_B_OFF), 8);
    if (clock && b) hour = (int)((b / 3600 + DISPLAY_OFFSET) % 24);
    log_direct("[store] clock base=%p b=%llu hour=%d (0x10CD990 chain)",
               (void*)clock, (unsigned long long)b, hour);
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
        hw = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (hw) CloseHandle(hw);
    }
    return TRUE;
}
