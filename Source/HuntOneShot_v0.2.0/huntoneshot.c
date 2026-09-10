/* huntoneshot.c - HuntOneShot v0.2.0（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；公共机制来自 shared\（addrsig + modkit）。
 * 分区：1 版本档案 / 2 运行期状态 / 3 日志 / 4 上下文 / 5 时间
 *       / 6 实例扫描 / 7 虚表自动发现 / 8 工作线程 / 9 生命周期
 *
 * 功能：可狩猎动物按“初始命中次数白名单”一枪狩猎成功。
 *   实例 +8 = 剩余可被击中次数（每次命中 -10，归零即成功）
 *   实例 +0x10 高 32 位 = 初始次数（战斗中不变）
 *   命中后把 +8 钳为 10，白名单动物第一枪归零。
 *   白名单 30/40/50/150/200/500（松鼠/狸猫鼬鼠兔子/狐狸绿雉/野猪/鹿/熊）；
 *   熊主 600 不在白名单。动物实例内存中无可读的物种 ID，只能按初始次数区分。
 *   实例特征：虚表 = vt_hunt，+0x18 == 0（动态池），+0x20 == vt_sub。
 *
 * 扫描策略：空列表时每 5s 全扫，扫到动物即停，当天不再扫；跨日
 * （时钟 day 变化）立即重扫；g_n==0 时保留 120s 限频的虚表自动发现
 * 兜底（防游戏更新后虚表 RVA 变化）。
 *
 * 编译（在 HuntOneShot_v0.2.0 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o huntoneshot.dll
 *     huntoneshot.c ..\shared\addrsig.c ..\shared\modkit.c -luser32
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
#define CLAMP_TO    10         /* 每枪伤害 10，钳到 10 = 一枪归零 */
#define TIME_OFF    0x3270     /* 时钟字段（秒） */
#define DISCOVER_MS 120000     /* 版本保护：空列表时自动发现虚表限频 */
#define RESCAN_MS   5000       /* 空列表时周期全扫间隔（扫到即停） */
#define MAX_ANIMALS 2048

/* 可狩猎动物初始命中次数白名单（普通版数值） */
static const uint32_t kHitList[] = { 30, 40, 50, 150, 200, 500 };
#define HIT_LIST_N (sizeof(kHitList) / sizeof(kHitList[0]))

#define SIG_VT_HUNT "vt_hunt_0xE005F8"
#define SIG_VT_SUB  "vt_sub_0xE47F80"
#define SIG_CLOCK   "clock_0x10CD990"

static const AddrSig kSigs[] = {
    { SIG_VT_HUNT, ADDRSIG_RIPGLOBAL,
      "a00200004c8d83b00200004c89442430488d0500000000498900418b50108bca41395014410f4c",
      "010101010101010101010101010101010101010000000001010101010101010101010101010101", 19 },
    { SIG_VT_SUB, ADDRSIG_RIPGLOBAL,
      "242848897c24304c896c2420498b06488d542420498bceff50389048895c2420488d05000000004889442440837c2460017559488b5c24484885db744848391d365e0f03771a48",
      "0101010101010101010101010101010101010101010101010101010101010101010101000000000101010101010101010101010101010101010101010101010101010101010101", 35 },
    { SIG_CLOCK, ADDRSIG_RIPGLOBAL,
      "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
      "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
};
static AddrRes   s_res[sizeof(kSigs) / sizeof(kSigs[0])];
static ProfEntry s_entries[] = {
    { SIG_VT_HUNT, 0, 0, 0 },
    { SIG_VT_SUB,  0, 0, 0 },
    { SIG_CLOCK,   0, 0, 0 },
};

/* ==================================================================
 * 2 运行期状态（唯一全局 g_ctx）
 * ================================================================== */
typedef struct {
    uintptr_t     base;
    uintptr_t     clock;      /* addrsig 时钟全局（绝对地址） */
    uintptr_t     vft_hunt;   /* 以下两者内部统一存 RVA，可被 discover 热更新 */
    uintptr_t     vft_sub;
    char          self_dir[MAX_PATH];
    char          log_path[MAX_PATH];
    LogKit        log;
    GameProfile   profile;
    uintptr_t     list[MAX_ANIMALS];
    int           n;
} HuntContext;

static HuntContext g_ctx;

static void log_init(void);
static void log_close(void);
static void log_direct(const char* fmt, ...);

/* ==================================================================
 * 3 日志
 * ================================================================== */
static void log_init(void)
{
    logkit_open(&g_ctx.log, g_ctx.log_path, "ho", TRUE);
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
    lstrcatA(g_ctx.log_path, "huntoneshot.log");
}

/* addrsig 运行时解析：虚表 RVA + 时钟全局；虚表 MISS 时由 discover 兜底 */
static void load_profile(void)
{
    profile_set_logger(log_direct);
    g_ctx.profile.mod = "ho";
    g_ctx.profile.base = g_ctx.base;
    g_ctx.profile.sigs = kSigs;
    g_ctx.profile.n_sigs = (int)(sizeof(kSigs) / sizeof(kSigs[0]));
    g_ctx.profile.res = s_res;
    g_ctx.profile.entries = s_entries;
    g_ctx.profile.n_entries = (int)(sizeof(s_entries) / sizeof(s_entries[0]));
    profile_load(&g_ctx.profile);
    if (profile_addr(&g_ctx.profile, SIG_VT_HUNT))
        g_ctx.vft_hunt = (uintptr_t)profile_rva(&g_ctx.profile, SIG_VT_HUNT);
    if (profile_addr(&g_ctx.profile, SIG_VT_SUB))
        g_ctx.vft_sub = (uintptr_t)profile_rva(&g_ctx.profile, SIG_VT_SUB);
    g_ctx.clock = profile_addr(&g_ctx.profile, SIG_CLOCK);
}

/* ==================================================================
 * 5 时间
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

/* 游戏日期：[[clock]+0x208]+0x3270 累计秒 / 86400 */
static int64_t clock_day(void)
{
    uintptr_t g = 0, d = 0;
    int64_t sec = -1;
    if (!g_ctx.clock || !mk_read(&g, (const void*)g_ctx.clock, 8)) return -1;
    if (!mk_read(&d, (const void*)(g + 0x208), 8)) return -1;
    if (!mk_read(&sec, (const void*)(d + TIME_OFF), 8)) return -1;
    return (sec >= 0) ? sec / 86400 : -1;
}

static int in_whitelist(uint32_t v)
{
    size_t i;
    for (i = 0; i < HIT_LIST_N; i++)
        if (v == kHitList[i]) return 1;
    return 0;
}

/* ==================================================================
 * 6 实例扫描
 * ================================================================== */
/* 全内存扫描：收集 vt_hunt 动态实例（+0x18 == 0，+0x20 == vt_sub） */
static int scan_pool(void)
{
    SYSTEM_INFO si;
    BYTE* addr;
    int n = 0;

    GetSystemInfo(&si);
    addr = (BYTE*)si.lpMinimumApplicationAddress;
    while ((uintptr_t)addr < (uintptr_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            BYTE buf[0x40000];
            size_t pos = 0;
            while (pos < mbi.RegionSize) {
                size_t chunk = mbi.RegionSize - pos;
                size_t i;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!mk_read(buf, (void*)((uintptr_t)mbi.BaseAddress + pos),
                             chunk))
                    break;
                for (i = 0; i + 8 <= chunk; i += 8) {
                    uint64_t v;
                    memcpy(&v, buf + i, 8);
                    if (v == g_ctx.base + g_ctx.vft_hunt) {
                        uintptr_t obj = (uintptr_t)mbi.BaseAddress + pos + i;
                        if (rd64(obj + 0x18) == 0 &&
                            rd64(obj + 0x20) == g_ctx.base + g_ctx.vft_sub &&
                            n < MAX_ANIMALS)
                            g_ctx.list[n++] = obj;
                    }
                }
                pos += chunk;
            }
        }
        if (mbi.RegionSize == 0) break;
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    g_ctx.n = n;
    return n;
}

/* ==================================================================
 * 7 虚表自动发现（游戏更新后虚表 RVA 变化时自愈）
 * ================================================================== */
static int pe_ranges(uintptr_t base, uintptr_t* tr, uintptr_t* ts,
                     uintptr_t* rr, uintptr_t* rs)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt;
    IMAGE_SECTION_HEADER* sc;
    int got_t = 0, got_r = 0, i;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    sc = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sc++) {
        char nm[9];
        memcpy(nm, sc->Name, 8);
        nm[8] = 0;
        if (!lstrcmpA(nm, ".text")) {
            *tr = sc->VirtualAddress;
            *ts = sc->Misc.VirtualSize;
            got_t = 1;
        } else if (!lstrcmpA(nm, ".rdata")) {
            *rr = sc->VirtualAddress;
            *rs = sc->Misc.VirtualSize;
            got_r = 1;
        }
    }
    return got_t && got_r;
}

/* 单遍反扫全内存白名单实例，反推 vt_hunt / vt_sub */
static int discover_vtables(void)
{
    uintptr_t tr = 0, ts = 0, rr = 0, rs = 0;
    uintptr_t r_lo, r_hi;
    int found = 0;
    uintptr_t vft = 0, sub = 0;
    SYSTEM_INFO si;
    BYTE* addr;
    BYTE buf[0x40000 + 0x40];

    if (!pe_ranges(g_ctx.base, &tr, &ts, &rr, &rs)) return 0;
    r_lo = g_ctx.base + rr;
    r_hi = r_lo + rs;
    GetSystemInfo(&si);
    addr = (BYTE*)si.lpMinimumApplicationAddress;
    while ((uintptr_t)addr < (uintptr_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 64 &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            size_t pos = 0;
            while (pos < mbi.RegionSize && found < 4) {
                size_t chunk = mbi.RegionSize - pos;
                size_t i;
                if (chunk > sizeof(buf) - 0x40) chunk = sizeof(buf) - 0x40;
                if (!mk_read(buf, (void*)((uintptr_t)mbi.BaseAddress + pos),
                             chunk))
                    break;
                if (!mk_read(buf + chunk,
                             (void*)((uintptr_t)mbi.BaseAddress + pos + chunk),
                             0x40))
                    memset(buf + chunk, 0, 0x40);
                for (i = 0; i + 8 <= chunk && found < 4; i += 8) {
                    uint64_t vptr, init, p18, p20;
                    memcpy(&vptr, buf + i, 8);
                    if (vptr < r_lo || vptr >= r_hi) continue;
                    memcpy(&init, buf + i + 0x10, 8);
                    if (!in_whitelist((uint32_t)(init >> 32))) continue;
                    memcpy(&p18, buf + i + 0x18, 8);
                    if (p18 != 0) continue;
                    memcpy(&p20, buf + i + 0x20, 8);
                    if (p20 < r_lo || p20 >= r_hi) continue;
                    if (!found) {
                        vft = vptr - g_ctx.base;
                        sub = p20 - g_ctx.base;
                    }
                    found++;
                }
                pos += chunk;
            }
        }
        if (found >= 4) break;
        if (mbi.RegionSize == 0) break;
        addr = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    if (found) {
        g_ctx.vft_hunt = vft;
        g_ctx.vft_sub = sub;
        log_direct("[ho] discovered vtable=%08X sub=%08X instances=%d",
                   (unsigned)vft, (unsigned)sub, found);
    }
    return found;
}

/* ==================================================================
 * 8 工作线程
 * ================================================================== */
static DWORD WINAPI worker_thread(LPVOID param)
{
    DWORD last_discover = 0;
    DWORD last_rescan = 0;
    int zero_rounds = 0;
    int64_t last_day;

    (void)param;
    Sleep(1000);
    g_ctx.base = (uintptr_t)GetModuleHandleA(NULL);
    load_profile();
    log_direct("[ho] HuntOneShot v0.2.0 loaded; base=%p", (void*)g_ctx.base);
    if (!g_ctx.base) return 0;
    if (!g_ctx.clock) {
        log_direct("[ho] FATAL: clock sig MISS, mod disabled");
        return 0;
    }
    log_direct("[ho] scan: %d animal instances", scan_pool());
    last_day = clock_day();

    while (1) {
        int64_t day;
        DWORD now;
        int i;

        Sleep(100);
        now = GetTickCount();
        /* 跨日：强制重扫（覆盖动物补齐/重生） */
        day = clock_day();
        if (day >= 0 && last_day >= 0 && day != last_day) {
            last_day = day;
            scan_pool();
            zero_rounds = 0;
            log_direct("[ho] new day %lld rescan: %d instances",
                       (long long)day, g_ctx.n);
        } else if (day >= 0) {
            last_day = day;
        }
        /* 空列表周期扫描（5s）：扫到即停，当天不再扫 */
        if (g_ctx.n == 0 && (int)(now - last_rescan) > RESCAN_MS) {
            last_rescan = now;
            scan_pool();
            if (g_ctx.n > 0)
                log_direct("[ho] rescan found: %d instances", g_ctx.n);
        }
        /* 版本保护：长时间扫不到动物时自动发现虚表（120s 限频） */
        if (g_ctx.n == 0 && zero_rounds >= 5 &&
            (int)(now - last_discover) > DISCOVER_MS) {
            last_discover = now;
            zero_rounds = 0;
            log_direct("[ho] zero for a while, discovering vtables ...");
            discover_vtables();
            scan_pool();
            log_direct("[ho] after discovery: %d instances", g_ctx.n);
        }
        zero_rounds = (g_ctx.n == 0) ? zero_rounds + 1 : 0;

        for (i = 0; i < g_ctx.n; i++) {
            uintptr_t obj = g_ctx.list[i];
            uint32_t v, init;
            if (!obj) continue;
            v = rd32(obj + 8);
            init = (uint32_t)(rd64(obj + 0x10) >> 32);
            if (in_whitelist(init) && v > 0 && v != CLAMP_TO) {
                uint32_t c = CLAMP_TO;
                if (mk_write((void*)(obj + 8), &c, 4)) {
                    log_direct("[ho] clamp %016llX: %u -> %u (init=%u)",
                               (unsigned long long)obj, v, c, init);
                }
            }
        }
    }
    return 0;
}

/* ==================================================================
 * 9 生命周期
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
    } else if (reason == DLL_PROCESS_DETACH) {
        log_close();
    }
    return TRUE;
}
