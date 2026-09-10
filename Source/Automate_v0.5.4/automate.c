/* automate.c - Automate v0.5.4（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现；
 * 公共机制来自 shared\：addrsig（签名解析）+ modkit（日志 LogKit、
 * 内存 mk_*、Hook 事务 HookTxn、版本档案 GameProfile）。
 *
 * 分区：
 *   1 版本档案      addrsig 签名表（时钟 / 容器全局 / 原生函数）
 *   2 运行期状态    文件作用域全局（g_*）
 *   3 基础工具      日志、内存读写、物品/容器结构
 *   4 扫描与连接    walk_gimmicks / build_group / 机器与箱子成组
 *   5 装料与取料    load_batch / collect_product（原生 OUTPUT_HELPER）
 *   6 主线程取料    mod_tick → automate_tick_main
 *   7 生命周期      DllMain（路径/日志/worker 线程）
 *
 * 顺序语义：与星露谷 Automate 一致——
 *   机器处理顺序 = walk_gimmicks 发现顺序；
 *   连接组 = build_group 邻接洪泛（机器 + 箱子）；
 *   装料源按 sort_sources_by_rank 排序（同层保持发现顺序）；
 *   机器启动顺序 = 以箱子为源的多源 BFS（水流扩散）。
 *
 * 编译（在 Automate_v0.5.4 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o Automate.dll
 *     automate.c ..\shared\addrsig.c ..\shared\modkit.c -luser32
 */
#define AUTOMATE_MOD

/*
 * automate.c - 静谧田园 自动化 Mod
 * ------------------------------------------------------------------
 * 复刻星露谷 Automate 思路：机器与相邻箱子成组，定期扫描，
 * 先收成品再补原料；能用游戏原生函数的地方一律调用原生函数。
 *
 * 已验证内存结构（build 24771274）：
 *   存档根 S = [[base+0x10CD990]+0x208]
 *   游戏时间(秒) = [S+0x3268]
 *   gimmick 链表：S+0x3440 哨兵 {next+0, prev+8, gimmick*+0x10}
 *   gimmick：uniqueID +0xE0 / pos +0xF0 / pData_ +0x240(->def, def+0x10=dataID)
 *            statusValueMap_(u32) +0x08 / statusIDValueMap_(u64) +0x48
 *            timeValue_[3] +0x288 / createTime_[3] +0x2A0
 *            itemList_(vector<Item*>) +0x2B8 {begin,+0 end,+8 cap,+0x10}
 *   item：dataID = [item+0x240]->def+0x10；数量 = *(u32*)(item+0x260)
 *   map 节点：key+0x10, next+8, 值 +0x18（原生 find 0x152C00 验证）
 *   熔炉批次槽：0/8/16=原料，1/9/17=燃料；实测 createTime 熔炉 42600s
 *   蜂箱 255600s、蚕箱 169200s（配置文件可改）
 *
 * 配置 automate.txt（热重载）：
 *   Recipe=机器ID:原料ID:原料数:燃料ID:燃料数:成品ID:时长秒
 *   Load=1 / Collect=0 / PollMs=1000 / Radius=500
 *
 * 编译：zig cc -shared -O2 -fms-extensions -o automate.dll automate.c
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "addrsig.h"
#include "modkit.h"

#define TIME_A_OFF 0x3270   /* gameTime_.second_（实测 s+0x3270=12873600） */

#define M_FURNACE  240090100
#define M_BEEHIVE  240240000
#define M_SILK     240270000

#define MAX_RECIPE  1024
#define MAX_MACHINE 512

/* map key 常量（8 字节 ASCII 小端 u64） */
#define KEY_INPROC   0x636F72506E69ULL          /* 'inProc'  */
#define KEY_ITEMVAL0 0x306C61566D657469ULL      /* 'itemVal0'*/
#define KEY_ITEMVAL1 0x316C61566D657469ULL      /* 'itemVal1'*/
#define KEY_ITEMVAL2 0x326C61566D657469ULL      /* 'itemVal2'*/
#define KEY_ITEMID0  0x3044496D657469ULL        /* 'itemID0' */
#define KEY_ITEMID1  0x003144496D657469ULL      /* 'itemID1' */
#define KEY_ITEMID2  0x003244496D657469ULL      /* 'itemID2' */

/* 3 槽机器的批次 key 必须按槽位取正确字符串
 * （此前 KEY_ITEMID0+b*0x100000000000000 算出 "itemID0\x10"，
 *  游戏读 itemID1 永远为 0 → 三槽只认 b0） */
static uint64_t key_itemid(int b)
{
    return b == 0 ? KEY_ITEMID0 : (b == 1 ? KEY_ITEMID1 : KEY_ITEMID2);
}

static uint64_t key_itemval(int b)
{
    return b == 0 ? KEY_ITEMVAL0 : (b == 1 ? KEY_ITEMVAL1 : KEY_ITEMVAL2);
}

typedef struct {
    uint64_t machine;
    uint64_t input;
    int      input_cnt;
    uint64_t fuel;
    int      fuel_cnt;
    uint64_t output;
    uint64_t duration;
    int      output_cnt;   /* 配方产出数量（0=未解析，运行时从配方表读） */
    uint64_t recipe_dur;   /* 配方真实时长（0=未解析/回退配置） */
} Recipe;

/* ---- 类型定义上移（供下方 AcContext 使用；原位置见删除处注释） ---- */
typedef struct {
    uintptr_t g;
    uint64_t mid;
    int batch;
    uintptr_t chests[MAX_MACHINE];
    int nchests;
} PendingRec;

typedef struct {
    uintptr_t g;
    uint64_t mid;
    int type;   /* 0=stale slots, 1=heal idle residue */
} CleanPending;

typedef struct {
    uintptr_t g;
    uint64_t mid;
    int batch;
    uintptr_t chests[MAX_MACHINE];
    int nchests;
    int used;
} LoadPending;
#define MAX_LPEND 1024

typedef struct {
    uintptr_t g;      /* 机器 gimmick */
    int batch;
    uint64_t output;  /* 成品 dataID */
    uint64_t loaded_at;
    int used;
} LoadRec;
#define MAX_LOADS 1024

typedef struct {
    uintptr_t g;
    float x, y;
    uint64_t did;   /* 扫描时记录 dataID，process 阶段复用 g_all_machines */
} ChestPos;

/* 队列容量（原定义在各自队列区，供 AcContext 使用上移） */
#define MACHINE_SEQ_MAX 4096
#define MAX_FAULT 512

/* ==================================================================
 * 运行期状态：唯一全局 g_ctx
 * ------------------------------------------------------------------
 * 原 50 个文件作用域全局按用途收进 AcContext；为避免改动全文引用点，
 * 用等名宏把旧名映射到 g_ctx 字段（语义等价于直接写 g_ctx.xxx，
 * 同一静态对象的字段），保留旧写法便于对照历史日志/断点。
 * ================================================================== */
typedef struct {
    LogKit        logkit;
    /* 配置与运行开关 */
    char          log_path[MAX_PATH];
    char          cfg_path[MAX_PATH];
    uint64_t      cfg_mtime;
    int           load_enabled;
    int           collect_enabled;
    int           force_finish;
    int           poll_ms;
    int           tile;
    int           diag_round;
    /* 配方表与机器表 */
    Recipe        recipes[MAX_RECIPE];
    int           recipe_n;
    uint64_t      machines[MAX_MACHINE];
    int           machine_n;
    /* addrsig 解析结果 */
    uintptr_t     base;
    uintptr_t     a_clock;
    uintptr_t     a_alloc;
    uintptr_t     a_map_find;
    uintptr_t     a_item_ctor;
    uintptr_t     a_native_load;
    uintptr_t     a_native_collect;
    uintptr_t     a_output_helper;
    uintptr_t     a_u64_map_find;
    uintptr_t     a_event_lookup;
    uintptr_t     a_event_notify;
    uintptr_t     a_recipe_table;
    uintptr_t     a_recipeMgr;
    uintptr_t     a_ui_check_jz;
    uintptr_t     a_ui2_mov;
    uintptr_t     a_map_getobj;
    uintptr_t     a_mapA;
    uintptr_t     a_gimmickMgr;
    /* 机器序列（worker → 主线程） */
    uintptr_t     ms_g[MACHINE_SEQ_MAX];
    uint64_t      ms_mid[MACHINE_SEQ_MAX];
    int           ms_seq[MACHINE_SEQ_MAX];
    int           ms_n;
    volatile LONG ms_lock;
    /* 故障隔离 */
    uintptr_t     fault_g[MAX_FAULT];
    int           fault_cnt[MAX_FAULT];
    int           fault_wait[MAX_FAULT];
    int           fault_n;
    /* 扫描快照（worker 写，主线程取料传快照） */
    uintptr_t     scan_chests[MAX_MACHINE];
    int           scan_n;
    ChestPos      all_chests[MAX_MACHINE];
    int           all_n;
    ChestPos      all_machines[MAX_MACHINE * 4];
    int           all_machine_n;
    /* 主线程队列 */
    PendingRec    pending[MAX_MACHINE * 4];
    int           pending_n;
    volatile LONG pending_lock;
    CleanPending  cleanp[MAX_MACHINE];
    int           cleanp_n;
    volatile LONG cleanp_lock;
    LoadPending   lpend[MAX_LPEND];
    volatile LONG lpend_lock;
    LoadRec       loads[MAX_LOADS];
} AcContext;

static AcContext g_ctx;

/* 旧名 → g_ctx 字段（同名映射，既有引用点无需改动） */
#define g_logkit          (g_ctx.logkit)
#define g_log_path        (g_ctx.log_path)
#define g_cfg_path        (g_ctx.cfg_path)
#define g_cfg_mtime       (g_ctx.cfg_mtime)
#define g_load_enabled    (g_ctx.load_enabled)
#define g_collect_enabled (g_ctx.collect_enabled)
#define g_force_finish    (g_ctx.force_finish)
#define g_poll_ms         (g_ctx.poll_ms)
#define g_tile            (g_ctx.tile)
#define g_diag_round      (g_ctx.diag_round)
#define g_recipes         (g_ctx.recipes)
#define g_recipe_n        (g_ctx.recipe_n)
#define g_machines        (g_ctx.machines)
#define g_machine_n       (g_ctx.machine_n)
#define g_base            (g_ctx.base)
#define g_a_clock         (g_ctx.a_clock)
#define g_a_alloc         (g_ctx.a_alloc)
#define g_a_map_find      (g_ctx.a_map_find)
#define g_a_item_ctor     (g_ctx.a_item_ctor)
#define g_a_native_load   (g_ctx.a_native_load)
#define g_a_native_collect (g_ctx.a_native_collect)
#define g_a_output_helper (g_ctx.a_output_helper)
#define g_a_u64_map_find  (g_ctx.a_u64_map_find)
#define g_a_event_lookup  (g_ctx.a_event_lookup)
#define g_a_event_notify  (g_ctx.a_event_notify)
#define g_a_recipe_table  (g_ctx.a_recipe_table)
#define g_a_recipeMgr     (g_ctx.a_recipeMgr)
#define g_a_ui_check_jz   (g_ctx.a_ui_check_jz)
#define g_a_ui2_mov       (g_ctx.a_ui2_mov)
#define g_a_map_getobj    (g_ctx.a_map_getobj)
#define g_a_mapA          (g_ctx.a_mapA)
#define g_a_gimmickMgr    (g_ctx.a_gimmickMgr)
#define g_ms_g            (g_ctx.ms_g)
#define g_ms_mid          (g_ctx.ms_mid)
#define g_ms_seq          (g_ctx.ms_seq)
#define g_ms_n            (g_ctx.ms_n)
#define g_ms_lock         (g_ctx.ms_lock)
#define g_fault_g         (g_ctx.fault_g)
#define g_fault_cnt       (g_ctx.fault_cnt)
#define g_fault_wait      (g_ctx.fault_wait)
#define g_fault_n         (g_ctx.fault_n)
#define g_scan_chests     (g_ctx.scan_chests)
#define g_scan_n          (g_ctx.scan_n)
#define g_all_chests      (g_ctx.all_chests)
#define g_all_n           (g_ctx.all_n)
#define g_all_machines    (g_ctx.all_machines)
#define g_all_machine_n   (g_ctx.all_machine_n)
#define g_pending         (g_ctx.pending)
#define g_pending_n       (g_ctx.pending_n)
#define g_pending_lock    (g_ctx.pending_lock)
#define g_cleanp          (g_ctx.cleanp)
#define g_cleanp_n        (g_ctx.cleanp_n)
#define g_cleanp_lock     (g_ctx.cleanp_lock)
#define g_lpend           (g_ctx.lpend)
#define g_lpend_lock      (g_ctx.lpend_lock)
#define g_loads           (g_ctx.loads)

/* addrsig 运行时解析结果（跨版本自动定位） */

/* 机器实例编号——同一 mid 下按地址分配 0 起序号，日志用
 * mid=xxx#N 区分同类型多台机器。表只增不删（会话内地址稳定）。 */

static int machine_seq(uintptr_t g, uint64_t mid)
{
    while (InterlockedExchange(&g_ms_lock, 1)) Sleep(1);
    int r = 0;
    for (int i = 0; i < g_ms_n; i++) {
        if (g_ms_g[i] == g) {
            r = g_ms_seq[i];
            InterlockedExchange(&g_ms_lock, 0);
            return r;
        }
    }
    int seq = 0;
    for (int i = 0; i < g_ms_n; i++)
        if (g_ms_mid[i] == mid && g_ms_seq[i] >= seq)
            seq = g_ms_seq[i] + 1;
    if (g_ms_n < MACHINE_SEQ_MAX) {
        g_ms_g[g_ms_n] = g;
        g_ms_mid[g_ms_n] = mid;
        g_ms_seq[g_ms_n] = seq;
        g_ms_n++;
        r = seq;
    }
    InterlockedExchange(&g_ms_lock, 0);
    return r;
}

/* 机器取料连续失败 3 次则隔离 60 轮（暂停处理），
 * 避免反复尝试异常机器。清伪槽不受隔离影响（始终先清）。 */

static void fault_mark(uintptr_t g)
{
    for (int i = 0; i < g_fault_n; i++) {
        if (g_fault_g[i] == g) {
            if (g_fault_cnt[i] < 100) g_fault_cnt[i]++;
            if (g_fault_cnt[i] >= 3) g_fault_wait[i] = 60;
            return;
        }
    }
    if (g_fault_n < MAX_FAULT) {
        g_fault_g[g_fault_n] = g;
        g_fault_cnt[g_fault_n] = 1;
        g_fault_wait[g_fault_n] = 0;
        g_fault_n++;
    }
}

/* 返回 1=该机器被隔离（本轮跳过装/取） */
static int fault_isolated(uintptr_t g)
{
    for (int i = 0; i < g_fault_n; i++) {
        if (g_fault_g[i] == g) {
            if (g_fault_wait[i] > 0) {
                g_fault_wait[i]--;
                return 1;
            }
            g_fault_cnt[i] = 0;
            return 0;
        }
    }
    return 0;
}

/* 机器连通箱子组（check_batch 也要用，提前声明） */

/* worker 检测完成批次 -> 主线程取料
 * （装料/检测在后台线程，取料在主线程——创建物品必须主线程，
 * 又不让重活卡游戏主线程导致掉帧） */

static void log_direct(const char* fmt, ...);

/* 锁等待带超时——持锁方若被 SEH 异常打断（未释放锁），
 * 等待方 2 秒后强制抢占并打日志，避免两个线程永久卡在锁循环。 */
static void lock_wait(volatile LONG* lock, const char* name)
{
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (InterlockedExchange(lock, 1) == 0) return;
        Sleep(1);
        if (GetTickCount64() - t0 > 2000) {
            log_direct("[au] LOCK TIMEOUT %s: holder lost, forced acquire",
                    name);
            return;
        }
    }
}

static void add_pending(uintptr_t g, uint64_t mid, int batch,
                        uintptr_t* chests, int nchests)
{
    lock_wait(&g_pending_lock, "pending");
    __try {
        if (g_pending_n < MAX_MACHINE * 4) {
            PendingRec* p = &g_pending[g_pending_n++];
            p->g = g;
            p->mid = mid;
            p->batch = batch;
            p->nchests = nchests < MAX_MACHINE ? nchests : MAX_MACHINE;
            memcpy(p->chests, chests,
                   (size_t)p->nchests * sizeof(uintptr_t));
        }
    } __except (1) {
        log_direct("[au] add_pending EXCEPTION g=%p b=%d",
                (void*)g, batch);
    }
    InterlockedExchange(&g_pending_lock, 0);
}

/* 伪槽清理（写状态 map + backing release）必须
 * 在游戏主线程执行——worker 与主线程 LOAD 并发写同一状态 map 会互相
 * 覆盖（读回验证失败 → status FAIL → 半截槽 → 清→装循环）。worker
 * 只做只读预判（has_stale_slots），有伪槽才排队。 */

static void cleanup_stale_slots(uintptr_t g, uint64_t mid);
static void heal_idle_state(uintptr_t g, uint64_t mid);
static int idle_residue(uintptr_t g);

static void add_cleanp(uintptr_t g, uint64_t mid, int type)
{
    lock_wait(&g_cleanp_lock, "cleanp");
    __try {
        for (int i = 0; i < g_cleanp_n; i++) {
            if (g_cleanp[i].g == g && g_cleanp[i].type == type) {
                g_cleanp[i].mid = mid;
                InterlockedExchange(&g_cleanp_lock, 0);
                return;
            }
        }
        if (g_cleanp_n < MAX_MACHINE) {
            g_cleanp[g_cleanp_n].g = g;
            g_cleanp[g_cleanp_n].mid = mid;
            g_cleanp[g_cleanp_n].type = type;
            g_cleanp_n++;
        }
    } __except (1) {
        log_direct("[au] add_cleanp EXCEPTION g=%p", (void*)g);
    }
    InterlockedExchange(&g_cleanp_lock, 0);
}

static void process_clean_pending(void)
{
    CleanPending cps[MAX_MACHINE];
    int n = 0;
    lock_wait(&g_cleanp_lock, "cleanp_proc");
    __try {
        n = g_cleanp_n;
        memcpy(cps, g_cleanp, (size_t)n * sizeof(CleanPending));
        g_cleanp_n = 0;
    } __except (1) {
        n = 0;
    }
    InterlockedExchange(&g_cleanp_lock, 0);
    for (int i = 0; i < n; i++) {
        __try {
            if (cps[i].type == 1)
                heal_idle_state(cps[i].g, cps[i].mid);
            else
                cleanup_stale_slots(cps[i].g, cps[i].mid);
        } __except (1) {
            /* 单台清理异常不拖垮 */
        }
    }
}

/* 装料待办——worker 只做只读扫描并排队，真正改游戏状态
 * （0xFF830 构造物品、槽位、状态 map）全部在游戏主线程执行，
 * 与取料同机制（worker 线程建游戏对象会崩）。 */

static void load_batch(uintptr_t g, uint64_t mid, int batch,
                       uintptr_t* chests, int nchests, int dry);

static void add_lpending(uintptr_t g, uint64_t mid, int batch,
                         uintptr_t* chests, int nchests)
{
    lock_wait(&g_lpend_lock, "lpend");
    __try {
        for (int i = 0; i < MAX_LPEND; i++) {
            if (g_lpend[i].used && g_lpend[i].g == g &&
                g_lpend[i].batch == batch) {
                InterlockedExchange(&g_lpend_lock, 0);
                return;   /* 已在队列，去重 */
            }
            if (!g_lpend[i].used && g_lpend[i].g == 0) {
                g_lpend[i].g = g;
                g_lpend[i].mid = mid;
                g_lpend[i].batch = batch;
                g_lpend[i].nchests = nchests < MAX_MACHINE ? nchests
                                                           : MAX_MACHINE;
                memcpy(g_lpend[i].chests, chests,
                       (size_t)g_lpend[i].nchests * sizeof(uintptr_t));
                g_lpend[i].used = 1;
                break;
            }
        }
    } __except (1) {
        log_direct("[au] add_lpending EXCEPTION g=%p b=%d",
                (void*)g, batch);
    }
    InterlockedExchange(&g_lpend_lock, 0);
}

static void process_load_pending(void)
{
    /* 装料分批——每 tick 最多 4 个，且累计
     * 执行超 8ms 立即停止（剩余留队列，下次 tick 继续）。避免跨日
     * 十几台机器一次性克隆+写状态导致掉帧。 */
#define LOAD_MAX_PER_TICK 4
#define LOAD_BUDGET_US    8000
    LARGE_INTEGER freq = { 0 };
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0 = { 0 };
    QueryPerformanceCounter(&t0);
    for (int done = 0; done < LOAD_MAX_PER_TICK; done++) {
        LoadPending cur;
        int have = 0;
        lock_wait(&g_lpend_lock, "lpend_proc");
        __try {
            for (int i = 0; i < MAX_LPEND; i++) {
                if (g_lpend[i].used) {
                    cur = g_lpend[i];
                    g_lpend[i].used = 0;
                    g_lpend[i].g = 0;
                    have = 1;
                    break;
                }
            }
        } __except (1) {
            have = 0;
        }
        InterlockedExchange(&g_lpend_lock, 0);
        if (!have) break;
        __try {
            load_batch(cur.g, cur.mid, cur.batch,
                       cur.chests, cur.nchests, 0);
        } __except (1) {
            /* 单批次装料异常不拖垮 */
        }
        if (freq.QuadPart > 0) {
            LARGE_INTEGER t1 = { 0 };
            QueryPerformanceCounter(&t1);
            LONGLONG us = (t1.QuadPart - t0.QuadPart) * 1000000LL /
                          freq.QuadPart;
            if (us >= LOAD_BUDGET_US) break;
        }
    }
}

/* mod 装料台账：游戏会接管并清掉mod写的 map/时间字段，
 * 所以用这份内存表判断"哪些批次是 mod 装的、成品是什么"，
 * 不依赖游戏字段 */

static LoadRec* find_load(uintptr_t g, int batch)
{
    for (int i = 0; i < MAX_LOADS; i++)
        if (g_loads[i].used && g_loads[i].g == g &&
            g_loads[i].batch == batch)
            return &g_loads[i];
    return 0;
}

static void add_load(uintptr_t g, int batch, uint64_t output,
                     uint64_t loaded_at)
{
    LoadRec* r = find_load(g, batch);
    if (!r) {
        for (int i = 0; i < MAX_LOADS; i++)
            if (!g_loads[i].used) {
                r = &g_loads[i];
                break;
            }
    }
    if (!r) return;
    r->g = g;
    r->batch = batch;
    r->output = output;
    r->loaded_at = loaded_at;
    r->used = 1;
}

static void clear_load(uintptr_t g, int batch)
{
    LoadRec* r = find_load(g, batch);
    if (r) r->used = 0;
}

static void log_direct(const char* fmt, ...);

/* ---- 配置 ---- */
static uint64_t get_mtime(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(g_cfg_path, GetFileExInfoStandard, &fd))
        return 0;
    return ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
           fd.ftLastWriteTime.dwLowDateTime;
}

static void read_config(void)
{
    g_recipe_n = 0;
    g_load_enabled = 1;
    g_collect_enabled = 0;
    g_force_finish = 0;
    g_poll_ms = 1000;
    g_tile = 50;
    FILE* f = fopen(g_cfg_path, "r");
    if (!f) {
        log_direct("[au] config not found, use defaults");
        goto out;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\r' ||
            line[0] == '\n')
            continue;
        if (_strnicmp(line, "Load=", 5) == 0)
            g_load_enabled = atoi(line + 5) != 0;
        else if (_strnicmp(line, "Collect=", 8) == 0)
            g_collect_enabled = atoi(line + 8) != 0;
        else if (_strnicmp(line, "ForceFinish=", 12) == 0)
            g_force_finish = atoi(line + 12) != 0;
        else if (_strnicmp(line, "PollMs=", 7) == 0) {
            int v = atoi(line + 7);
            if (v >= 100 && v <= 60000) g_poll_ms = v;
        } else if (_strnicmp(line, "Tile=", 5) == 0) {
            int v = atoi(line + 5);
            if (v >= 30 && v <= 200) g_tile = v;
        } else if (_strnicmp(line, "Recipe=", 7) == 0 &&
                   g_recipe_n < MAX_RECIPE) {
            Recipe* r = &g_recipes[g_recipe_n];
            int i = sscanf(line + 7, "%llu:%llu:%d:%llu:%d:%llu:%llu",
                           &r->machine, &r->input, &r->input_cnt,
                           &r->fuel, &r->fuel_cnt, &r->output, &r->duration);
            if (i == 7) {
                g_recipe_n++;
                int found = 0;
                for (int m = 0; m < g_machine_n; m++)
                    if (g_machines[m] == r->machine) {
                        found = 1;
                        break;
                    }
                if (!found && g_machine_n < MAX_MACHINE)
                    g_machines[g_machine_n++] = r->machine;
            }
        }
    }
    fclose(f);
out:
    g_cfg_mtime = get_mtime();
    log_direct("[au] config Load=%d Collect=%d PollMs=%d Tile=%d recipes=%d",
            g_load_enabled, g_collect_enabled, g_poll_ms, g_tile,
            g_recipe_n);
}

/* ---- 日志 ---- */

/* ---- 日志（shared logkit，句柄一次打开） ---- */

static void log_init(void)
{
    logkit_open(&g_logkit, g_log_path, "au", TRUE);
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

/* ---- 内存读写 ---- */
static int rd8(uintptr_t a, uint64_t* v)
{
    SIZE_T rd = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)a, v, 8, &rd) &&
           rd == 8;
}

static int rd4(uintptr_t a, uint32_t* v)
{
    SIZE_T rd = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)a, v, 4, &rd) &&
           rd == 4;
}

static int wr8(uintptr_t a, uint64_t v)
{
    SIZE_T wr = 0;
    DWORD old;
    if (!VirtualProtect((void*)a, 8, PAGE_READWRITE, &old)) return 0;
    int ok = WriteProcessMemory(GetCurrentProcess(), (void*)a, &v, 8, &wr) &&
             wr == 8;
    VirtualProtect((void*)a, 8, old, &old);
    return ok;
}

static int wr4(uintptr_t a, uint32_t v)
{
    SIZE_T wr = 0;
    DWORD old;
    if (!VirtualProtect((void*)a, 4, PAGE_READWRITE, &old)) return 0;
    int ok = WriteProcessMemory(GetCurrentProcess(), (void*)a, &v, 4, &wr) &&
             wr == 4;
    VirtualProtect((void*)a, 4, old, &old);
    return ok;
}

static int rdptr(uintptr_t a, uintptr_t* out)
{
    uint64_t v;
    if (!rd8(a, &v)) return 0;
    if (v < 0x10000 || v > 0x7FFFFFFFFFFFULL) return 0;
    *out = (uintptr_t)v;
    return 1;
}

/* 引用计数（shared_ptr 模式：+8 是引用计数） */
static int ref_add(uintptr_t p)
{
    uint32_t rc;
    if (!rd4(p + 8, &rc)) return 0;
    return wr4(p + 8, rc + 1);
}

/* 物品数量（+0x260，0x101C60 验证） */
static int item_count(uintptr_t item, uint32_t* cnt)
{
    return rd4(item + 0x260, cnt);
}

static int item_set_count(uintptr_t item, uint32_t cnt)
{
    return wr4(item + 0x260, cnt);
}

/* 游戏分配器（1.08）：0x8AD3A0(size)——内部用 [base+0x3121518] 分配
 * 上下文 + 0x20 对齐分配并自动清零（0x1400D5D4E 物品创建实测确认；
 * 1.06 旧 0x8BF060(池,0x20,size) 已失效）。 */
static uintptr_t alloc_game(size_t size)
{
    typedef void* (__fastcall *AllocFn)(uint32_t);
    if (!g_a_alloc) return 0;
    AllocFn fn = (AllocFn)g_a_alloc;
    uintptr_t r = 0;
    __try {
        r = (uintptr_t)fn((uint32_t)size);
    } __except (1) {
        return 0;
    }
    return r;
}

/* addrsig 运行时解析：全部版本敏感地址（签名表见 addrsig_table_1.08.1.c） */
static void addr_init(void)
{
    static const AddrSig sigs[] = {
        { "alloc_0x8AD4A0", ADDRSIG_FUNC,
          "48895c2408574883ec20488bf94c8bc1488b0d00000000ba20000000e800000000803dd004820000488bd874154885c0",
          "010101010101010101010101010101010101010000000001010101010100000000010101010101010101010101010101", 0 },
        { "map_find_0x150B10", ADDRSIG_FUNC,
          "890641c6460800498bc64883c430415f415e415c5f5e5d5bc3488bf848b8ffffffffffffff0748394610750e488d0d40",
          "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 85 },
        { "item_ctor_0xFF830", ADDRSIG_FUNC,
          "48895c24205556574154415541564157488bec4883ec504d8be0488bf2488bd948894df8e80000000090488d05f735d0",
          "010101010101010101010101010101010101010101010101010101010101010101010100000000010101010101010101", 0 },
        { "native_load_0x26FF20", ADDRSIG_FUNC,
          "48895c24185556574154415541564157488dac2460f0ffffb8a0100000e800000000482be0488bda4c8be94885c90f84",
          "010101010101010101010101010101010101010101010101010101010101000000000101010101010101010101010101", 0 },
        { "native_collect_0x270C10", ADDRSIG_FUNC,
          "48895c24185556574154415541564157488d6c24b04881ec50010000488b05000000004833c4488945404c8bf2488bd9",
          "010101010101010101010101010101010101010101010101010101010101010000000001010101010101010101010101", 0 },
        { "output_helper_0x165DF0", ADDRSIG_FUNC,
          "48895c24205556574154415541564157488dac2460f0ffffb8a0100000e800000000482be04963f0897424204c8bea4c",
          "010101010101010101010101010101010101010101010101010101010101000000000101010101010101010101010101", 0 },
        { "u64_map_find_0x150CD0", ADDRSIG_FUNC,
          "488906c6460800488bc64883c430415f415e415c5f5e5d5bc3488bf848b8ffffffffffffff0748394510750e488d0d81",
          "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 84 },
        { "event_lookup_0x1506F0", ADDRSIG_FUNC,
          "084c39781075f149890641c6460800498bc6488b9c24880000004883c430415f415e415d415c5f5e5dc3488bf048b822",
          "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 88 },
        { "event_notify_0x150510", ADDRSIG_FUNC,
          "40534883ec20488b59184885db745848897c24380f1f40000f1f840000000000f0ff4308488b4b584885c97440488b01",
          "010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101", 0 },
        { "recipe_table_0x029720", ADDRSIG_FUNC,
          "12050800440fb6c049bab301000000010000448bc848b825232284e49cf2cb4c33c0418bc148c1e800000000418bc148",
          "010101010101010101010101010101010101010101010101010101010101010101010101010101010000000001010101", 10 },
        { "ui_check_jz_0x5B6B6C", ADDRSIG_FUNC,
          "2420e8000000004c8b4424204d3b43607404498d7818488b07488b8098030000",
          "0101010000000001010101010101010101010101010101010101010101010101", -16 },
        { "ui2_mov_0x710A28", ADDRSIG_FUNC,
          "3b4360418bfc7404498d78184c8965a74c8b074533c94d8b4058488d55a7488b",
          "0101010101010101010101010101010101010101010101010101010101010101", -16 },
        { "map_getobj_0xF5D30", ADDRSIG_FUNC,
          "184889742420574881ec90000000488b05000000004833c44889842480000000488bf2488bd9488954247033ffe8cacd",
          "010101010101010101010101010101010100000000010101010101010101010101010101010101010101010101010101", 4 },
        { "clock_0x10CD990", ADDRSIG_RIPGLOBAL,
          "c8e8000000004c8b034d8b8088000000488b1500000000488b9208020000488bcee8a09a020048",
          "010100000000010101010101010101010101010000000001010101010101010101010101010101", 19 },
        { "g_mapA_0x10D5A78", ADDRSIG_RIPGLOBAL,
          "0d5a00000041b805000000488d542440488b0d00000000e80000000090488b4c24404885c9741d",
          "010101010101010101010101010101010101010000000001000000000101010101010101010101", 19 },
        { "g_gimmickMgr_0x10D5A60", ADDRSIG_RIPGLOBAL,
          "950f57c033c00f114424304889442440488b1d0000000048895c2448f0ff038b430485c0743bb8",
          "010101010101010101010101010101010101010000000001010101010101010101010101010101", 19 },
        { "g_recipeMgr_0x10D59F0", ADDRSIG_RIPGLOBAL,
          "55574156488d6c24904881ec70010000488b0d00000000e800000000488b0d00000000e8ce0b00",
          "010101010101010101010101010101010101010000000001000000000101010000000001010101", 19 },
    };
    AddrRes res[sizeof(sigs) / sizeof(sigs[0])];
    addrsig_resolve(sigs, (int)(sizeof(sigs) / sizeof(sigs[0])), g_base,
                    res, log_direct);
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (!res[i].hit) continue;
        if (strcmp(res[i].name, "alloc_0x8AD4A0") == 0)
            g_a_alloc = res[i].addr;
        else if (strcmp(res[i].name, "map_find_0x150B10") == 0)
            g_a_map_find = res[i].addr;
        else if (strcmp(res[i].name, "item_ctor_0xFF830") == 0)
            g_a_item_ctor = res[i].addr;
        else if (strcmp(res[i].name, "native_load_0x26FF20") == 0)
            g_a_native_load = res[i].addr;
        else if (strcmp(res[i].name, "native_collect_0x270C10") == 0)
            g_a_native_collect = res[i].addr;
        else if (strcmp(res[i].name, "output_helper_0x165DF0") == 0)
            g_a_output_helper = res[i].addr;
        else if (strcmp(res[i].name, "u64_map_find_0x150CD0") == 0)
            g_a_u64_map_find = res[i].addr;
        else if (strcmp(res[i].name, "event_lookup_0x1506F0") == 0)
            g_a_event_lookup = res[i].addr;
        else if (strcmp(res[i].name, "event_notify_0x150510") == 0)
            g_a_event_notify = res[i].addr;
        else if (strcmp(res[i].name, "recipe_table_0x029720") == 0)
            g_a_recipe_table = res[i].addr;
        else if (strcmp(res[i].name, "map_getobj_0xF5D30") == 0)
            g_a_map_getobj = res[i].addr;
        else if (strcmp(res[i].name, "clock_0x10CD990") == 0)
            g_a_clock = res[i].addr;
        else if (strcmp(res[i].name, "g_mapA_0x10D5A78") == 0)
            g_a_mapA = res[i].addr;
        else if (strcmp(res[i].name, "g_gimmickMgr_0x10D5A60") == 0)
            g_a_gimmickMgr = res[i].addr;
        else if (strcmp(res[i].name, "g_recipeMgr_0x10D59F0") == 0)
            g_a_recipeMgr = res[i].addr;
        else if (strcmp(res[i].name, "ui_check_jz_0x5B6B6C") == 0)
            g_a_ui_check_jz = res[i].addr;
        else if (strcmp(res[i].name, "ui2_mov_0x710A28") == 0)
            g_a_ui2_mov = res[i].addr;
    }
}

/* ---- 原生启动加工（1.08：GimmickSetProcessTime，FUN_14026FF20） ----
 * 静态确认：FUN_14026FF20 不读 TLS/事件上下文（与 0x270C10
 * 不同），全程空指针保护；RDX = 地图格子对象（FUN_1401CB010 按位置
 * 哈希取格），成功后原生写 createTime/itemID0 map、用 FUN_1400FF830
 * 构造完整成品并放入机器容器 [g+0x2B8]——即全原生装料。
 * RDX 链（分析文档）：A=[base+0x10D5A48] → FUN_1400F5D30(A,&out)=B →
 * [B+0x200]=C → scene → [scene+0x98]=map；备选 [map+0x548] */


/* 1.08 原生物品构造 FUN_1400FF830(obj, S, itemID)——
 * CMapObjectStatus 构造器（0x270C10 内部用同一个）。0x101F00 在 1.08
 * 已失效（反汇编无此函数）。数量写子对象 [+0x258]+0x260（
 * 主对象 +0x260 是堆叠上限，不能覆盖）。返回完整对象，可安全入槽。 */
static uintptr_t make_item_native_18(uint64_t item_id, uint32_t count)
{
    uintptr_t obj = alloc_game(0x2C0);
    if (!obj) return 0;
    /* qol（ProductionCreateInputIngredients）：ctor 前整块清零，
     * 避免分配器残留字段进入对象。 */
    memset((void*)obj, 0, 0x2C0);
    uintptr_t root = 0, s = 0;
    if (!g_a_clock || !rdptr(g_a_clock, &root)) return 0;
    if (!rdptr(root + 0x208, &s)) return 0;
    typedef uintptr_t (*CtorFn)(uintptr_t, uintptr_t, uint64_t);
    if (!g_a_item_ctor) return 0;
    CtorFn fn = (CtorFn)g_a_item_ctor;
    uintptr_t ret = 0;
    __try {
        ret = fn(obj, s, item_id);
    } __except (1) {
        return 0;
    }
    if (!ret) ret = obj;
    /* 数量 gauge 内嵌在
     * obj+0x258（vftable 在 +0x258，当前值在 +0x260）。 */
    wr4(ret + 0x260, count);
    return ret;
}

/* 从机器/箱子容器找同 dataID 物品（游戏创建的完整对象） */
static int vec_slots(uintptr_t vec, uintptr_t* begin, uintptr_t* end);
static int obj_dataid(uintptr_t obj, uint64_t* did);

/* 完整克隆游戏现成物品（0x2C0 内存整体复制），
 * 只改引用计数/数量——对象字段与游戏创建的一模一样 */

/* 对比 mod 生成物品 vs 游戏创建同款物品的关键字段 */

/* gimmick/item → dataID */
static int obj_dataid(uintptr_t obj, uint64_t* did)
{
    /* 游戏自身读取方式（0x270089 熔炉处理代码）：
     * def = [obj+0x240]; p = [def]; dataID = [p] */
    uintptr_t def, p;
    if (!rdptr(obj + 0x240, &def)) return 0;
    if (!rdptr(def, &p)) return 0;
    return rd8(p, did);
}

/* ---- gimmick 链表 ---- */
static void walk_gimmicks(uintptr_t s,
                          int (*cb)(uintptr_t g, void* ctx), void* ctx)
{
    uintptr_t head, node;
    if (!rdptr(s + 0x3440, &head)) return;
    if (!rdptr(head, &node)) return;
    __try {
        for (int guard = 0; node != head && guard < 20000; guard++) {
            uintptr_t g;
            if (rdptr(node + 0x10, &g) && g) {
                if (cb(g, ctx)) break;
            }
            if (!rdptr(node, &node)) break;
        }
    } __except (1) {
        /* gimmick 链表遍历异常：跳过本轮，不拖崩 */
    }
}

/* ---- 容器 vector ---- */
static int vec_slots(uintptr_t vec, uintptr_t* begin, uintptr_t* end)
{
    if (!rdptr(vec, begin)) return 0;
    if (!rdptr(vec + 8, end)) return 0;
    if (*end < *begin || (*end - *begin) > 0x20000) return 0;
    return 1;
}

/* 直接把 Item* 放入容器槽（增加引用；槽内旧指针释放） */
static void intrusive_release(uintptr_t p);
static int slot_put(uintptr_t vec, int slot, uintptr_t item)
{
    uintptr_t b, e;
    if (!vec_slots(vec, &b, &e)) return 0;
    int n = (int)((e - b) / 8);
    if (slot < 0 || slot >= n) return 0;
    uint64_t oldv = 0;
    if (!rd8(b + slot * 8, &oldv)) return 0;
    uintptr_t old = (uintptr_t)oldv;
    if (old == item) return 1;
    if (!ref_add(item)) return 0;
    if (!wr8(b + slot * 8, item)) {
        intrusive_release(item);
        return 0;
    }
    if (old) intrusive_release(old);
    return 1;
}

/* 槽位移动：src[si] -> dst[di]，引用计数先加后减，无归零窗口 */

/* 在容器里找指定 itemID 的堆，返回槽位与数量 */

/* ---- 状态 map：调用原生 find(0x150B10) 找节点，值在 +0x18 ----
 * 1.08：0x152C00 已失效（现为指令中间）；0x150B10 是 u64 key
 * unordered_map find（RSI=map, R14=out, R8=key 指针，节点 +0x10=key
 * +0x18=值），与 1.06 0x152C00 同布局。 */
static uintptr_t map_find(uintptr_t map, uint64_t key)
{
    if (!g_a_map_find) return 0;
    typedef uintptr_t (__fastcall *FindFn)(uintptr_t, uintptr_t, uintptr_t);
    FindFn find = (FindFn)g_a_map_find;
    uint8_t out[16] = { 0 };
    __try {
        find(map, (uintptr_t)&out, (uintptr_t)&key);
    } __except (1) {
        return 0;
    }
    uintptr_t node = 0;
    memcpy(&node, out, 8);
    if (!node) return 0;
    uint64_t k;
    if (!rd8(node + 0x10, &k) || k != key) return 0;
    return node;
}

/* u64 map find（1.08.1 0x150CD0，qol STATUS_U64_LOOKUP 对应；
 * u32 map 用 map_find=0x150B10。节点布局同：+0x10=key、+0x18=值） */
static uintptr_t map_find_u64(uintptr_t map, uint64_t key)
{
    if (!g_a_u64_map_find) return 0;
    typedef uintptr_t (__fastcall *FindFn)(uintptr_t, uintptr_t, uintptr_t);
    FindFn find = (FindFn)g_a_u64_map_find;
    uint8_t out[16] = { 0 };
    __try {
        find(map, (uintptr_t)&out, (uintptr_t)&key);
    } __except (1) {
        return 0;
    }
    uintptr_t node = 0;
    memcpy(&node, out, 8);
    if (!node) return 0;
    uint64_t k;
    if (!rd8(node + 0x10, &k) || k != key) return 0;
    return node;
}

/* 原生 intrusive release（qol 0x4C86F0 语义；1.08.1 游戏内联实现，
 * 反汇编 140166490 同款：refcount XADD -1，旧值==1 时 vtable[0](obj,1)
 * 析构）。替代 ref_sub（只减计数不析构 → 泄漏）。 */
static void intrusive_release(uintptr_t p)
{
    if (!p) return;
    uint32_t rc = 0;
    if (!rd4(p + 8, &rc) || rc == 0) return;
    uint32_t old = rc;
    wr4(p + 8, rc - 1);
    if (old == 1) {
        uintptr_t vt = 0, dtor = 0;
        if (rdptr(p, &vt) && vt && rdptr(vt, &dtor) && dtor) {
            __try {
                ((void(__fastcall*)(uintptr_t, int))dtor)(p, 1);
            } __except (1) {
            }
        }
    }
}

/* qol ProductionForcesRankZero：特定物品产出 rank 强制 0（位图照抄） */
static int forces_rank_zero(uint64_t item_id)
{
    if (item_id >= 0x330ccULL && item_id - 0x330ccULL <= 0x14ULL &&
        ((0x100401ULL >> (item_id - 0x330ccULL)) & 1ULL) != 0)
        return 1;
    if (item_id >= 0x330eaULL && item_id - 0x330eaULL <= 0x3cULL &&
        ((0x1004010040100401ULL >> (item_id - 0x330eaULL)) & 1ULL) != 0)
        return 1;
    return 0;
}

/* qol ProductionReadMachineSlotLimit：机器 Lv2 判定从运行时数据读
 * （[g+0x240]→[data]，data+0x128 CString == "2" → 3 槽，否则 1）。
 * 不再用机器 ID 末三位规则。 */
static int machine_slot_limit(uintptr_t g)
{
    uintptr_t holder = 0, data = 0;
    if (!rdptr(g + 0x240, &holder) || !holder) return 1;
    if (!rdptr(holder, &data) || !data) return 1;
    uint64_t len = 0;
    rd8(data + 0x130, &len);
    if (len != 1) return 1;
    uintptr_t lvl = 0;
    if (!rdptr(data + 0x128, &lvl) || !lvl) return 1;
    uint64_t v = 0;
    if (!rd8(lvl, &v)) return 1;
    return ((char)v == '2') ? 3 : 1;
}

/* qol 装料原料源（ProductionBuildInputPlan / ApplyInputSources） */
typedef struct {
    uintptr_t item;    /* 箱子堆物品 */
    uintptr_t chest;   /* 箱子 g */
    int slot;
    uint32_t cnt;
    uint32_t rank;
} IngSrc;

/* qol ProductionBuildRankOrder：rank 相对 anchor 的顺序分（anchor 递减
 * 到 0 再递增到 4） */
static int rank_order_score(int rank, int anchor)
{
    if (rank <= anchor) return anchor - rank;
    return anchor + 1 + (rank - anchor - 1);
}

static void sort_sources_by_rank(IngSrc* s, int n, int anchor)
{
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (rank_order_score((int)s[j].rank, anchor) <
                rank_order_score((int)s[best].rank, anchor))
                best = j;
        IngSrc t = s[i]; s[i] = s[best]; s[best] = t;
    }
}

/* 收集相连箱子中指定 did 的所有堆（qol：箱子槽顺序权威） */
static int collect_sources(uintptr_t* chests, int nchests, uint64_t did,
                           IngSrc* srcs, int max_srcs)
{
    int n = 0;
    for (int ci = 0; ci < nchests && n < max_srcs; ci++) {
        uintptr_t cvec = chests[ci] + 0x2B8;
        uintptr_t cb, ce;
        if (!vec_slots(cvec, &cb, &ce)) continue;
        int cn = (int)((ce - cb) / 8);
        for (int si = 0; si < cn && n < max_srcs; si++) {
            uintptr_t o;
            if (!rdptr(cb + si * 8, &o) || !o) continue;
            uint64_t od = 0;
            if (!obj_dataid(o, &od) || od != did) continue;
            uint32_t c = 0;
            item_count(o, &c);
            if (c == 0) continue;
            uint32_t r = 0;
            rd4(o + 0x280, &r);
            srcs[n].item = o;
            srcs[n].chest = chests[ci];
            srcs[n].slot = si;
            srcs[n].cnt = c;
            srcs[n].rank = r;
            n++;
        }
    }
    return n;
}

/* 从多源扣减 take_cnt 个（qol ApplyInputSources：adjust -amount；
 * 源耗尽 detach 槽+释放槽引用）。返回 1=成功。 */
static int apply_sources(IngSrc* s, int n, int take_cnt)
{
    int remaining = take_cnt;
    for (int k = 0; k < n && remaining > 0; k++) {
        /* qol：扣减前验证槽位仍指向该物品（notify 可能重建箱子 storage） */
        {
            uintptr_t cb, ce;
            if (!vec_slots(s[k].chest + 0x2B8, &cb, &ce)) return 0;
            uintptr_t cur = 0;
            if (!rdptr(cb + (uintptr_t)s[k].slot * 8, &cur) ||
                cur != s[k].item)
                return 0;
        }
        int take = (int)s[k].cnt < remaining ? (int)s[k].cnt : remaining;
        uint32_t newc = s[k].cnt - (uint32_t)take;
        item_set_count(s[k].item, newc);
        if (newc == 0) {
            uintptr_t cb, ce;
            if (vec_slots(s[k].chest + 0x2B8, &cb, &ce))
                wr8(cb + (uintptr_t)s[k].slot * 8, 0);
            intrusive_release(s[k].item);
        }
        remaining -= take;
    }
    return remaining == 0;
}

/* 状态事件 owner：status+0x10 的 vtable[1](status+0x10)（qol
 * ProductionGetStatusEventOwner 同款，反汇编 14026F12D 确认） */
static uintptr_t status_event_owner(uintptr_t g)
{
    uintptr_t io = g + 0x10;
    uintptr_t vt = 0;
    if (!rdptr(io, &vt) || !vt) return 0;
    uintptr_t getter = 0;
    if (!rdptr(vt + 8, &getter) || !getter) return 0;
    uintptr_t owner = 0;
    __try {
        owner = ((uintptr_t(__fastcall*)(uintptr_t))getter)(io);
    } __except (1) {
        return 0;
    }
    return owner;
}

/* qol setStatusInt 原生等价：u32 map find（0x150B10）→ 写 node+0x18 →
 * eventOwner vtable[1] → 事件 map find（0x1506F0，owner+0x08）→
 * EVENT_NOTIFY（0x150510，node+0x18）→ 读回验证。
 * 反汇编对照：游戏装料 0x26F238-0x26F27C。 */
static int status_set_int(uintptr_t g, uint64_t key, int value)
{
    uintptr_t node = map_find(g + 0x18, key);
    if (!node) return 0;
    uint32_t before = 0;
    rd4(node + 0x18, &before);
    if ((int)before == value) return 1;   /* 值未变，不发通知 */
    wr4(node + 0x18, (uint32_t)value);
    {
        uint32_t rb = 0;
        if (!rd4(node + 0x18, &rb) || (int)rb != value) return 0;
    }
    if (!g_a_event_lookup || !g_a_event_notify) return 1;
    uintptr_t owner = status_event_owner(g);
    if (!owner) return 1;
    uintptr_t enode = 0;
    {
        typedef uintptr_t (__fastcall *FindFn)(uintptr_t, uintptr_t, uintptr_t);
        uint8_t out[16] = { 0 };
        __try {
            ((FindFn)g_a_event_lookup)(owner + 0x08, (uintptr_t)&out,
                                       (uintptr_t)&key);
        } __except (1) {
            return 1;
        }
        memcpy(&enode, out, 8);
    }
    if (!enode) return 1;
    __try {
        ((void(__fastcall*)(uintptr_t))g_a_event_notify)(enode + 0x18);
    } __except (1) {
    }
    return 1;
}

/* qol setStatusU64 原生等价：u64 map find（0x150CD0）→ 写 node+0x18 →
 * 事件 map find（0x1506F0，owner+0x48）→ EVENT_NOTIFY。
 * 反汇编对照：游戏装料 0x26F108-0x26F14E。 */
static int status_set_u64(uintptr_t g, uint64_t key, uint64_t value)
{
    uintptr_t node = map_find_u64(g + 0x58, key);
    if (!node) return 0;
    uint64_t before = 0;
    rd8(node + 0x18, &before);
    if (before == value) return 1;
    wr8(node + 0x18, value);
    {
        uint64_t rb = 0;
        if (!rd8(node + 0x18, &rb) || rb != value) return 0;
    }
    if (!g_a_event_lookup || !g_a_event_notify) return 1;
    uintptr_t owner = status_event_owner(g);
    if (!owner) return 1;
    uintptr_t enode = 0;
    {
        typedef uintptr_t (__fastcall *FindFn)(uintptr_t, uintptr_t, uintptr_t);
        uint8_t out[16] = { 0 };
        __try {
            ((FindFn)g_a_event_lookup)(owner + 0x48, (uintptr_t)&out,
                                       (uintptr_t)&key);
        } __except (1) {
            return 1;
        }
        memcpy(&enode, out, 8);
    }
    if (!enode) return 1;
    __try {
        ((void(__fastcall*)(uintptr_t))g_a_event_notify)(enode + 0x18);
    } __except (1) {
    }
    return 1;
}

/* 配方产出数量：遍历游戏配方表（[g_recipeMgr] → 0x29720 → [RAX+0x98]..
 * [RAX+0xA0]，元素=配方对象 → [obj]=数据对象）。数据对象布局（反汇编
 * 0x26EEA8-0x26F1C9 确认）：+0xA8=输入1、+0xB0=输入1数量、+0xB8=输入2、
 * +0xC0=输入2数量、+0xC8=输出ID、+0xD0=产出数量、+0xD8=时长基数（×60）。
 * 按 (input, output) 匹配（同 input+output 唯一；时长/产出数以运行时为准，
 * 不信任配置文件——蜂箱每朵花时长不同，配置全错）。
 * 返回 1=命中；命中时填充 out_cnt/out_dur。 */
static int lookup_recipe(uint64_t input, uint64_t output,
                         uint32_t* out_cnt, uint64_t* out_dur)
{
    if (!g_a_recipeMgr || !g_a_recipe_table) return 0;
    uintptr_t mgr = 0;
    if (!rdptr(g_a_recipeMgr, &mgr) || !mgr) return 0;
    uintptr_t tbl = 0;
    typedef uintptr_t (__fastcall *TblFn)(uintptr_t);
    __try {
        tbl = ((TblFn)g_a_recipe_table)(mgr);
    } __except (1) {
        return 0;
    }
    if (!tbl) return 0;
    uintptr_t begin = 0, end = 0;
    if (!rdptr(tbl + 0x98, &begin) || !rdptr(tbl + 0xA0, &end)) return 0;
    if (end < begin || (end - begin) > 0x20000) return 0;
    for (uintptr_t p = begin; p < end; p += 8) {
        uintptr_t ro = 0;
        if (!rdptr(p, &ro) || !ro) continue;
        /* 反汇编 0x26EEA8/0x26F1C3：配方对象两层解引用，
         * 字段在 data = [ro]（ro = [数组元素]） */
        uintptr_t data = 0;
        if (!rdptr(ro, &data) || !data) continue;
        uint64_t in1 = 0, outid = 0, dur = 0;
        rd8(data + 0xA8, &in1);
        rd8(data + 0xC8, &outid);
        rd8(data + 0xD8, &dur);
        if (dur > 0xFFFFFFFFFFFFULL) continue;
        dur *= 60;
        if (in1 == input && outid == output) {
            uint32_t oc = 1;
            rd4(data + 0xD0, &oc);
            if (out_cnt) *out_cnt = (oc > 0 && oc <= 999) ? oc : 1;
            if (out_dur) *out_dur = dur ? dur : 0;
            return 1;
        }
    }
    return 0;
}

static int map_set_u64(uintptr_t map, uint64_t key, uint64_t val,
                       int is_u32);
static int game_time(uint64_t* t);
static int map_get_u64(uintptr_t map, uint64_t key, uint64_t* val);

/* 配方表是否有该输出物品（伪状态清理用：游戏 UI 完成检查按 itemIDN
 * 查配方表，查不到就崩 0x5B6B72）。遍历与 lookup_recipe 同源。 */
static int lookup_has_output(uint64_t output)
{
    if (!g_a_recipeMgr || !g_a_recipe_table) return 0;
    uintptr_t mgr = 0;
    if (!rdptr(g_a_recipeMgr, &mgr) || !mgr) return 0;
    uintptr_t tbl = 0;
    typedef uintptr_t (__fastcall *TblFn)(uintptr_t);
    __try {
        tbl = ((TblFn)g_a_recipe_table)(mgr);
    } __except (1) {
        return 0;
    }
    if (!tbl) return 0;
    uintptr_t begin = 0, end = 0;
    if (!rdptr(tbl + 0x98, &begin) || !rdptr(tbl + 0xA0, &end)) return 0;
    if (end < begin || (end - begin) > 0x20000) return 0;
    for (uintptr_t p = begin; p < end; p += 8) {
        uintptr_t ro = 0;
        if (!rdptr(p, &ro) || !ro) continue;
        uintptr_t data = 0;
        if (!rdptr(ro, &data) || !data) continue;
        uint64_t outid = 0;
        rd8(data + 0xC8, &outid);
        if (outid == output) return 1;
    }
    return 0;
}

/* 伪状态清理：历史版本同 tick 装 3 槽导致 b1/b2 成为
 * 游戏不认的伪槽（itemIDN/时间/backing 残留且已存进存档），游戏 UI
 * 完成检查查配方表失败 → 崩 0x5B6B72。检测并清空异常槽：
 *  - itemIDN 为空闲标记（>= 2^32 指针值）或 0 但时间非 0（半截）
 *  - itemIDN 在配方表无对应输出（游戏 UI 同样会崩）
 * 合法槽（配方表有输出、时间/itemIDN 齐备）不动。 */
static void cleanup_stale_slots(uintptr_t g, uint64_t mid)
{
    int nb = machine_slot_limit(g);
    for (int b = 0; b < nb; b++) {
        /* mod 台账保护的槽不清——本次会话 mod 装的槽（含
         * 游戏把时间覆盖回旧值的场景）有台账记录，不能按伪状态清理，
         * 否则"清→装→清"循环白白消耗原料。只清无台账的历史残留。 */
        if (find_load(g, b)) continue;
        uint64_t t0 = 0, ct = 0, idn = 0;
        rd8(g + 0x288 + b * 8, &t0);
        rd8(g + 0x2A0 + b * 8, &ct);
        map_get_u64(g + 0x58,
                    key_itemid(b),
                    &idn);
        if (t0 == 0 && ct == 0 && (idn == 0 || idn >= 0x100000000ULL))
            continue;   /* 正常空闲 */
        int bad = 0;
        if (idn == 0 || idn >= 0x100000000ULL)
            bad = 1;    /* 空闲标记或半截（时间非 0 但无成品 ID） */
        else if (!lookup_has_output(idn))
            bad = 1;    /* 配方表无此输出：游戏 UI 检查必崩 */
        else if (t0 != 0 && ct != 0) {
            /* idn 真实 + 配方表有输出 = 加工中或
             * 完成待取（mod 装的槽游戏完成时不自动清时间），不是伪槽。
             * 时间已过且 idn 真实时清掉会销毁成品（backing release），
             * 且触发"清→装→清"循环刷原料/掉帧——必须留给 collect。 */
            continue;
        }
        if (!bad) continue;
        /* 清空该槽：状态 map + 时间 + backing（8 项） */
        map_set_u64(g + 0x58,
                    key_itemid(b),
                    0, 0);
        map_set_u64(g + 0x18,
                    key_itemval(b),
                    0, 1);
        wr8(g + 0x288 + b * 8, 0);
        wr8(g + 0x2A0 + b * 8, 0);
        {
            uintptr_t ob = 0;
            if (rdptr(g + 0x2B8, &ob) && ob && ob > 0x10000 &&
                ob < 0x7FFF00000000ULL) {
                for (int s = b * 8; s < b * 8 + 8; s++) {
                    uintptr_t o = 0;
                    if (!rdptr(ob + s * 8, &o) || !o) continue;
                    wr8(ob + s * 8, 0);
                    intrusive_release(o);
                }
            }
        }
        log_direct("[au] STALE-CLEAN mid=%llu b=%d idn=%llu t0=%llu"
                " ct=%llu",
                (unsigned long long)mid, b,
                (unsigned long long)idn, (unsigned long long)t0,
                (unsigned long long)ct);
    }
    /* 全部槽空闲则清 inProc */
    {
        int any = 0;
        for (int b = 0; b < nb; b++) {
            uint64_t t2 = 0, c2 = 0, id2 = 0;
            rd8(g + 0x288 + b * 8, &t2);
            rd8(g + 0x2A0 + b * 8, &c2);
            if (t2 || c2) { any = 1; break; }
            map_get_u64(g + 0x58,
                        key_itemid(b),
                        &id2);
            if (id2 && id2 < 0x100000000ULL) { any = 1; break; }
        }
        if (!any) status_set_int(g, KEY_INPROC, 0);
    }
}

/* 只读伪槽预判（worker 线程）：有"必须清理"的槽才排队给主线程。
 * 判定与 cleanup_stale_slots 一致：半截（时间非 0 但 idn 无效）、
 * 空闲标记（idn>=2^32）、配方表无输出（UI 检查 0x5B6B72 崩溃源）。
 * 完成待取（idn 真实）不算伪槽。 */
static int has_stale_slots(uintptr_t g)
{
    int nb = machine_slot_limit(g);
    for (int b = 0; b < nb; b++) {
        if (find_load(g, b)) continue;
        uint64_t t0 = 0, ct = 0, idn = 0;
        rd8(g + 0x288 + b * 8, &t0);
        rd8(g + 0x2A0 + b * 8, &ct);
        map_get_u64(g + 0x58, key_itemid(b), &idn);
        if (t0 == 0 && ct == 0 && (idn == 0 || idn >= 0x100000000ULL))
            continue;   /* 正常空闲 */
        if (idn == 0 || idn >= 0x100000000ULL) return 1;
        if (!lookup_has_output(idn)) return 1;
    }
    return 0;
}

static int map_get_u64(uintptr_t map, uint64_t key, uint64_t* val)
{
    uintptr_t node = map_find(map, key);
    if (!node) return 0;
    return rd8(node + 0x18, val);
}

/* g+0x18 状态 map 的值是 u32（inProc/itemValN），读 8 字节会
 * 把 node+0x1C 相邻内存带进来（实测 inproc=0x24800000000，低 32 位 0、
 * 高 32 位堆指针垃圾 → inproc 守卫恒误判非 0，装料被静默跳过）。 */
static int map_get_u32(uintptr_t map, uint64_t key, uint32_t* val)
{
    uintptr_t node = map_find(map, key);
    if (!node) return 0;
    return rd4(node + 0x18, val);
}

static int map_set_u64(uintptr_t map, uint64_t key, uint64_t val, int is_u32)
{
    uintptr_t node = map_find(map, key);
    if (!node) {
        log_direct("[au] map key missing key=%llu", (unsigned long long)key);
        return 0;
    }
    uint64_t before = 0;
    rd8(node + 0x18, &before);
    int ok = is_u32 ? wr4(node + 0x18, (uint32_t)val)
                    : wr8(node + 0x18, val);
    return ok;
}

/* ---- 放置残留自愈 + 垃圾 backing 清理 ----
 * 机器从背包重新放置/游戏初始化时可能残留 inProc、台账或非法的
 * backing 槽指针（实测新放置烟熏机 backing b1/b2 含 exe .data 地址
 * 等垃圾指针，导致装料被 backing-occ 守卫静默跳过，且后续 collect
 * 操作垃圾指针有闪退风险）。worker 只做只读预判并排队，写操作全部
 * 在游戏主线程执行（与 LOAD/COLLECT 同机制）。日志每台机器限一次。 */

/* 残留预判：inProc 与实际槽状态不一致（任何槽在跑必须=1，全空闲
 * 必须=0），或全空闲但台账有记录且对应 itemID 是空槽标记
 * （0 / >=2^32，即没有真实待取成品）。待取成品（itemID 为合法
 * 物品 ID）不算残留，不清，防丢成品。 */
static int idle_residue(uintptr_t g)
{
    int nb = machine_slot_limit(g);
    int any_running = 0;
    for (int b = 0; b < nb; b++) {
        uint64_t t0 = 0, ct = 0;
        rd8(g + 0x288 + b * 8, &t0);
        rd8(g + 0x2A0 + b * 8, &ct);
        if (t0 != 0 && ct != 0) { any_running = 1; break; }
    }
    uint32_t inproc = 0;
    if (map_get_u32(g + 0x18, KEY_INPROC, &inproc)) {
        int want = any_running ? 1 : 0;
        if (inproc != (uint32_t)want) return 1;
    }
    if (any_running) return 0;
    for (int b = 0; b < nb; b++) {
        if (find_load(g, b)) {
            uint64_t pend = 0;
            map_get_u64(g + 0x58, key_itemid(b), &pend);
            if (pend == 0 || pend >= 0x100000000ULL)
                return 1;
        }
    }
    return 0;
}

/* 主线程：同步 inProc 到实际槽状态（任何槽在跑→1，全空闲→0），
 * 全空闲时顺带清台账空槽标记残留。状态 map 写必须主线程。 */
static void heal_idle_state(uintptr_t g, uint64_t mid)
{
    int nb = machine_slot_limit(g);
    int changed = 0;
    int any_running = 0;
    for (int b = 0; b < nb; b++) {
        uint64_t t0 = 0, ct = 0;
        rd8(g + 0x288 + b * 8, &t0);
        rd8(g + 0x2A0 + b * 8, &ct);
        if (t0 != 0 && ct != 0) { any_running = 1; break; }
    }
    uint32_t inproc = 0;
    if (map_get_u32(g + 0x18, KEY_INPROC, &inproc)) {
        int want = any_running ? 1 : 0;
        /* 不用 status_set_int——它带 eventOwner vtable 事件
         * 通知，依赖游戏 UI/TLS 上下文（fault RVA 0x5C9B45 实测：
         * TLS 槽 0x57F0 解引用空指针崩）。纯 map 写无事件通知，
         * 与游戏上下文无关。 */
        if (inproc != (uint32_t)want) {
            if (map_set_u64(g + 0x18, KEY_INPROC, want, 1)) changed = 1;
        }
    }
    if (!any_running) {
        for (int b = 0; b < nb; b++) {
            if (find_load(g, b)) {
                uint64_t pend = 0;
                map_get_u64(g + 0x58, key_itemid(b), &pend);
                if (pend == 0 || pend >= 0x100000000ULL) {
                    clear_load(g, b);
                    changed = 1;
                }
            }
        }
    }
    if (changed) {
        static uintptr_t logged[32];
        static int ln = 0;
        for (int i = 0; i < ln; i++)
            if (logged[i] == g) return;
        if (ln < 32) logged[ln++] = g;
        log_direct("[au] heal idle state mid=%llu#%d",
                (unsigned long long)mid, machine_seq(g, mid));
    }
}

/* ---- 游戏时间 ---- */
static int game_time(uint64_t* t)
{
    /* gameTime_.second_ = [S+0x3270]（实测 12873600 = Day149） */
    uintptr_t root, s;
    if (!g_a_clock || !rdptr(g_a_clock, &root)) return 0;
    if (!rdptr(root + 0x208, &s)) return 0;
    return rd8(s + TIME_A_OFF, t);
}

/* ---- 机器 gimmick -> 机器处理对象（旧取料逻辑残留，仅编译期引用） ---- */

/* ---- 游戏原生取料 ---- */

/* ---- 游戏 TLS 上下文（0x270C10 前置检查） ----
 * 读主线程自定义 TLS 槽 0x56C8 的上下文对象：
 * [GS:0x58]=TEB.ThreadLocalStoragePointer[0] → [数组]=TLS 基址 →
 * [基址+0x56C8]=槽索引 → [基址+0x5A20+(索引-1)*8]=上下文对象。
 * 游戏在 UI/事件处理里才设置该槽；mod_tick 直接调用时通常为 0。 */

/* 0x270C10 前置检查：机器 g 是否登记在 [0x10D5A30]+0x58 全局 map。
 * 游戏 FUN_140250690 检查通过才返回 gimmick；检查不过走
 * "%s invalid baseID %d" 报错路径，TLS 上下文=0 时空指针崩
 * （1.08 实测 fault=0x112E20 = FUN_140112e20 的 [RCX+8]）。 */

/* TLS 上下文、[0x10D5A30] 登记状态、[g+0xB0] 与 itemIDN 候选 key
 * 在 g+0x58 / [g+0xB0]+0x58 两个 map 的命中情况。 */

/* 原生"机器取成品" 0x270C10(gimmick, 批次记录 rec)：
 * 玩家对完成机器取成品时游戏执行的函数——读状态 map itemID0、
 * 分配物品对象、写入机器 itemList_（+0x2B8）并更新状态。
 * mod 直接调它生成成品，再从容器搬进箱子。SEH 保护。 */

/* 批次是否"完成可收取"：状态 map itemIDN 非 0，且时间已到
 * （timeValue+createTime <= now；游戏完成清零 t0/ct 时直接视为完成） */
static int batch_ready(uintptr_t g, int b)
{
    uint64_t idn = 0;
    if (!map_get_u64(g + 0x58,
                     key_itemid(b),
                     &idn) || !idn)
        return 0;
    /* 游戏对空槽写共享指针（如 0x20600000000）而非 0，
     * 只有像真实物品 ID（< 2^32）才算待取成品，指针标记视为空闲 */
    if (idn >= 0x100000000ULL) return 0;
    uint64_t t0 = 0, ct = 0, now;
    rd8(g + 0x288 + b * 8, &t0);
    rd8(g + 0x2A0 + b * 8, &ct);
    if (t0 == 0 && ct == 0) return 1;
    if (!game_time(&now)) return 0;
    return now >= t0 + ct;
}

/* 直接调 1.08.1 原生 OUTPUT_HELPER（FUN_140165DF0，qol 1.06
 * 0x168130 对应；addrsig output_helper_0x165DF0 运行时解析）。
 * 反汇编取证（140165DF0）：helper 内部完整完成
 *   读 itemIDN/itemValN 状态 map → 0xFF830 构造物品 + 0xFF590
 *   设数量 → 清 itemIDN/itemValN map → 清 +0x288/+0x2A0 时间
 *   字段 → 清 backing 槽（slot*8, slot*8+1）并正确释放引用 →
 *   经输出参数返回物品对象。
 * 前置条件：①机器生产数据 [g+0x240]→[data] 必须有效（helper
 * 首步 0x165E40 直接解引用 [RDX+0x240]→[RAX]，NULL 必崩）；
 * ②箱子必须有位置（合并堆或空位），否则不调 helper、保持机器
 * 状态等箱子有空位（qol capacity-wait 语义）。
 * 调用后只做：搬箱子（合并同种堆数量 / 空位）、inProc 收尾、
 * mod 台账清理。不再手动写任何机器字段。 */
/* chests/nchests 由调用方传快照（v0.4.9 核查）：worker 线程每轮会
 * 重写全局 g_scan_chests/g_scan_n（build_group），主线程 collect 若
 * 读全局会与 worker 并发竞争（可能读到半更新数组）。 */
static int collect_product(uintptr_t g, uint64_t mid, int batch,
                           uintptr_t* chests, int nchests)
{
    uint64_t did = 0, iv = 0;
    map_get_u64(g + 0x58,
                key_itemid(batch),
                &did);
    {
        uint32_t iv32 = 0;
        map_get_u32(g + 0x18, key_itemval(batch), &iv32);
        iv = iv32;
    }
    if (!did || !g_a_output_helper) return 0;
    /* 前置 1：生产数据有效（[g+0x240] → [data] 非空） */
    {
        uintptr_t data = 0, tbl = 0;
        if (!rdptr(g + 0x240, &data) || !data ||
            !rdptr(data, &tbl) || !tbl) {
            static int dbg_nodata = 0;
            if (!dbg_nodata) {
                dbg_nodata = 1;
                log_direct("[au] collect mid=%llu b=%d SKIP (prod data"
                        " invalid, wait game)",
                        (unsigned long long)mid, batch);
            }
            return 0;
        }
    }
    /* qol ProductionReadExpectedOutputRank：helper 会清 backing，
     * 必须先读期望品质（槽内物品 +0x280 最大值；特定物品强制 0） */
    int expected_rank = 0;
    {
        uintptr_t ob = 0;
        if (rdptr(g + 0x2B8, &ob) && ob && ob > 0x10000 &&
            ob < 0x7FFF00000000ULL) {
            for (int s = batch * 8; s < batch * 8 + 8; s++) {
                uintptr_t o = 0;
                if (!rdptr(ob + s * 8, &o) || !o) continue;
                uint32_t r = 0;
                if (rd4(o + 0x280, &r) && (int)r > expected_rank)
                    expected_rank = (int)r;
            }
        }
        if (forces_rank_zero(did)) expected_rank = 0;
    }

    /* qol PlanChestDelivery 前置规划（capacity-wait：放不下不调
     * helper、保持机器状态等箱子有空位）：
     *  ①同 did 且同 rank 的堆（<999）→ 合并目标
     *  ②第一个空槽 → 空位目标
     * 两者都没有 → 等待。 */
    int merge_ci = -1, merge_cs = -1;
    uint32_t merge_before = 0;
    int free_ci = -1, free_cs = -1;
    for (int ci = 0; ci < nchests; ci++) {
        uintptr_t cvec = chests[ci] + 0x2B8;
        uintptr_t cb, ce;
        if (!vec_slots(cvec, &cb, &ce)) continue;
        int cn = (int)((ce - cb) / 8);
        for (int cs = 0; cs < cn; cs++) {
            uint64_t sv = 0;
            if (!rd8(cb + cs * 8, &sv)) break;
            if (sv == 0) {
                if (free_ci < 0) {
                    free_ci = ci;
                    free_cs = cs;
                }
                continue;
            }
            uintptr_t o = (uintptr_t)sv;
            uint64_t od = 0;
            if (!obj_dataid(o, &od) || od != did) continue;
            uint32_t r = 0;
            rd4(o + 0x280, &r);
            if ((int)r != expected_rank) continue;
            uint32_t oc = 0;
            item_count(o, &oc);
            if (oc >= 999) continue;
            merge_ci = ci;
            merge_cs = cs;
            merge_before = oc;
            break;
        }
        if (merge_ci >= 0) break;
    }
    if (merge_ci < 0 && free_ci < 0) {
        static int dbg_full = 0;
        if (dbg_full < 5) {
            dbg_full++;
            log_direct("[au] collect mid=%llu b=%d chest FULL did=%llu"
                    " (wait)",
                    (unsigned long long)mid, batch,
                    (unsigned long long)did);
        }
        return 0;
    }

    /* 调原生 OUTPUT_HELPER：构造物品 + 清机器状态/槽，一步到位 */
    typedef uintptr_t (*OHFn)(uintptr_t*, uintptr_t, int);
    uintptr_t product = 0;
    static uintptr_t exc_addr = 0;
    __try {
        ((OHFn)g_a_output_helper)(&product, g, batch);
    } __except (exc_addr = (uintptr_t)GetExceptionInformation()
                               ->ExceptionRecord->ExceptionAddress, 1) {
        fault_mark(g);
        static int dbg_oh = 0;
        if (dbg_oh < 5) {
            dbg_oh++;
            log_direct("[au] OUTPUT_HELPER EXCEPTION g=%p b=%d fault=%p",
                    (void*)g, batch, (void*)exc_addr);
        }
        return 0;
    }
    if (!product) return 0;

    /* qol 产物验证（NearbyReadItem + slotCleared）：itemId、数量、
     * rank 由 helper 设置；时间字段必须已清（helper 清槽后） */
    {
        uint64_t pdid = 0;
        uint32_t pcnt = 0;
        uint32_t prank = 0;
        uint64_t t0 = 0, ct = 0;
        rd8(g + 0x288 + batch * 8, &t0);
        rd8(g + 0x2A0 + batch * 8, &ct);
        rd4(product + 0x280, &prank);
        if (!obj_dataid(product, &pdid) || pdid != did ||
            !item_count(product, &pcnt) || pcnt != (uint32_t)iv ||
            (int)prank != expected_rank ||
            t0 != 0 || ct != 0) {
            static int dbg_badprod = 0;
            if (!dbg_badprod) {
                dbg_badprod = 1;
                log_direct("[au] collect mid=%llu b=%d product verify FAIL"
                        " did=%llu pdid=%llu cnt=%u want=%llu rank=%d"
                        " expRank=%d t0=%llu ct=%llu",
                        (unsigned long long)mid, batch,
                        (unsigned long long)did, (unsigned long long)pdid,
                        pcnt, (unsigned long long)iv, (int)prank,
                        expected_rank,
                        (unsigned long long)t0, (unsigned long long)ct);
            }
            intrusive_release(product);
            fault_mark(g);
            return 0;
        }
    }

    /* 执行放入（qol：合并 adjust + 读回验证；空位 AddRef+写+读回；
     * 失败回滚）。合并优先，空位兜底。 */
    int placed = 0;
    uintptr_t placed_chest = 0;
    if (merge_ci >= 0) {
        uintptr_t chest = chests[merge_ci];
        placed_chest = chest;
        uintptr_t cvec = chest + 0x2B8;
        uintptr_t cb, ce;
        if (vec_slots(cvec, &cb, &ce)) {
            uintptr_t cur = 0;
            if (rdptr(cb + merge_cs * 8, &cur) && cur) {
                uint32_t after = merge_before + (uint32_t)iv;
                item_set_count(cur, after);
                uint32_t rb = 0;
                if (item_count(cur, &rb) && rb == after) {
                    /* 产物全量并入已有堆：数量清零 + 释放（qol sourceDelta）*/
                    item_set_count(product, 0);
                    intrusive_release(product);
                    placed = 1;
                } else {
                    item_set_count(cur, merge_before);   /* 回滚 */
                    static int dbg_merge = 0;
                    if (!dbg_merge) {
                        dbg_merge = 1;
                        log_direct("[au] collect merge readback FAIL"
                                " mid=%llu b=%d",
                                (unsigned long long)mid, batch);
                    }
                }
            }
        }
    }
    if (!placed && free_ci >= 0) {
        uintptr_t chest = chests[free_ci];
        placed_chest = chest;
        uintptr_t cvec = chest + 0x2B8;
        uintptr_t cb, ce;
        if (vec_slots(cvec, &cb, &ce) && slot_put(cvec, free_cs, product)) {
            item_set_count(product, (uint32_t)iv);
            uintptr_t rb = 0;
            if (rdptr(cb + free_cs * 8, &rb) && rb == product) {
                intrusive_release(product);
                placed = 1;
            } else {
                wr8(cb + free_cs * 8, 0);   /* 回滚：清槽 */
                static int dbg_free = 0;
                if (!dbg_free) {
                    dbg_free = 1;
                    log_direct("[au] collect free-slot readback FAIL"
                            " mid=%llu b=%d",
                            (unsigned long long)mid, batch);
                }
            }
        }
    }
    if (!placed) {
        intrusive_release(product);
        fault_mark(g);
        return 0;
    }
    clear_load(g, batch);
    /* qol ProductionWillResetInProcAfterSlot 语义：inProc 描述的是
     * "未完成的定时器"，不是"待取成品"。只有兄弟槽仍在运行
     * （t0/ct 有效且未到完成时间）才保持 inProc=1；完成待取不阻止清。
     * 当前槽已由 helper 清时间，排除在外。
     * 游戏 OUTPUT_HELPER 完成当前槽会把 inProc 清 0，若兄弟
     * 槽仍在运行必须显式恢复 inProc=1（原代码"保持"的是游戏刚清的 0
     * → 后续装料被 inproc 守卫误挡：sibling=1 inproc=0）。
     * 兄弟槽判定统一为"时间字段非 0 即在跑"（与 load_batch
     * 守卫、heal 一致）——原"未到完成时间才算跑"严格判定与守卫宽松
     * 判定不一致，造成 collect 清 0 → 守卫要求 1 → heal → collect 循环。 */
    {
        int nb = machine_slot_limit(g);
        int sibling_running = 0;
        for (int b2 = 0; b2 < nb; b2++) {
            if (b2 == batch) continue;
            uint64_t t2 = 0, c2 = 0;
            rd8(g + 0x288 + b2 * 8, &t2);
            rd8(g + 0x2A0 + b2 * 8, &c2);
            if (t2 != 0 && c2 != 0) { sibling_running = 1; break; }
        }
        if (sibling_running)
            status_set_int(g, KEY_INPROC, 1);
        else
            status_set_int(g, KEY_INPROC, 0);
    }
    log_direct("[au] COLLECT mid=%llu#%d b=%d product=%llu x%llu -> chest=%p"
            " (native OUTPUT_HELPER 0x165DF0)",
            (unsigned long long)mid, machine_seq(g, mid), batch,
            (unsigned long long)did, (unsigned long long)iv,
            (void*)placed_chest);
    return 1;
}

/* ---- 装料：单个批次 ----
 * dry=1（worker）：只做只读守卫+扫描，需要装料就排队给主线程；
 * dry=0（主线程）：真正执行（0xFF830 构造 + 槽位 + 状态写）。 */
static void load_batch(uintptr_t g, uint64_t mid, int batch,
                       uintptr_t* chests, int nchests, int dry)
{
    int ore_slot, fuel_slot;
    /* 所有加工机同容器布局：每组 = 槽(b*8, b*8+1)；Lv1 只用 b=0 */
    ore_slot = batch * 8;
    fuel_slot = batch * 8 + 1;
    /* 机器 +0x2B8 = 指针 → 3×8=24 项 Item* backing 数组（qol 布局），
     * 不是 vector（+0x2C0/+0x2C8 不是 end/cap）。 */
    uintptr_t ovec = g + 0x2B8;
    uintptr_t oback = 0;
    rdptr(ovec, &oback);
    if (nchests <= 0) return;

    /* 该批次已启动则跳过 */
    uint64_t t0 = 0, ct = 0;
    rd8(g + 0x288 + batch * 8, &t0);
    rd8(g + 0x2A0 + batch * 8, &ct);
    if (t0 != 0 || ct != 0) {
        return;
    }
    /* mod 台账：装过且成品还没取走 -> 不重复装。
     * 若批次已完全空闲（无时间、无待取成品、槽空）说明台账
     * 过期（玩家手动取走输入/游戏重置），清台账继续装，修复"误跳过
     * 没装满的机器"。 */
    if (find_load(g, batch)) {
        uint64_t pend = 0;
        map_get_u64(g + 0x58,
                    key_itemid(batch),
                    &pend);
        int slot_empty = 1;
        if (oback && oback > 0x10000 && oback < 0x7FFF00000000ULL) {
            uintptr_t occ = 0;
            if (rdptr(oback + ore_slot * 8, &occ) && occ)
                slot_empty = 0;
        }
        static int dbg_stale = 0;
        if (slot_empty && t0 == 0 && ct == 0 &&
            (pend == 0 || pend >= 0x100000000ULL)) {
            if (!dbg_stale) {
                dbg_stale = 1;
                log_direct("[au] clear stale load mid=%llu b=%d",
                        (unsigned long long)mid, batch);
            }
            clear_load(g, batch);
        } else {
            static int dbg_findload = 0;
            if (dbg_findload < 5) {
                dbg_findload++;
                log_direct("[au] load skip find_load mid=%llu#%d b=%d"
                        " pend=%llu slotEmpty=%d t0=%llu ct=%llu",
                        (unsigned long long)mid, machine_seq(g, mid), batch,
                        (unsigned long long)pend, slot_empty,
                        (unsigned long long)t0, (unsigned long long)ct);
            }
            return;
        }
    }
    /* 该批次已有待取成品（itemIDN 非 0）则不再装料，防止堆叠 */
    {
        uint64_t pend = 0;
        if (map_get_u64(g + 0x58,
                        key_itemid(batch),
                        &pend) && pend != 0) {
            /* 指针标记（>= 2^32）是空槽空闲标记，不是待取成品 */
            if (pend >= 0x100000000ULL) {
            } else {
                static int dbg_pend = 0;
                if (dbg_pend < 5) {
                    dbg_pend++;
                    log_direct("[au] load skip itemIDN mid=%llu#%d b=%d"
                            " pend=%llu",
                            (unsigned long long)mid, machine_seq(g, mid), batch,
                            (unsigned long long)pend);
                }
                return;
            }
        }
    }
    /* qol 选槽：itemValN 也必须为 0（itemIDN 已清但数量残留=异常态） */
    {
        uint32_t pval = 0;
        if (map_get_u32(g + 0x18,
                        key_itemval(batch),
                        &pval) && pval != 0) {
            static int dbg_pval = 0;
            if (dbg_pval < 5) {
                dbg_pval++;
                log_direct("[au] load skip itemValN mid=%llu#%d b=%d pval=%llu",
                        (unsigned long long)mid, machine_seq(g, mid), batch,
                        (unsigned long long)pval);
            }
            return;
        }
    }
    /* 机器原料槽已有物品则视为已占用（防重复装料；backing 语义） */
    {
        if (!oback || oback <= 0x10000 ||
            oback >= 0x7FFF00000000ULL) {
            log_direct("[au] machine itemList backing invalid");
            return;
        }
        uintptr_t occ;
        if (rdptr(oback + ore_slot * 8, &occ) && occ) {
            static int dbg_occ = 0;
            if (dbg_occ < 5) {
                dbg_occ++;
                log_direct("[au] load skip backing-occ mid=%llu#%d b=%d",
                        (unsigned long long)mid, machine_seq(g, mid), batch);
            }
            return;
        }
    }
    /* qol（ProductionPrepareIdleMachineForInput）inProc 一致性：
     * 兄弟槽在跑（时间字段非 0）则 inProc 必须=1，否则必须=0；
     * 不一致说明机器状态机异常，拒绝装料避免进一步污染。 */
    {
        int nb = machine_slot_limit(g);
        int sibling_running = 0;
        for (int b2 = 0; b2 < nb; b2++) {
            if (b2 == batch) continue;
            uint64_t t2 = 0, c2 = 0;
            rd8(g + 0x288 + b2 * 8, &t2);
            rd8(g + 0x2A0 + b2 * 8, &c2);
            if (t2 != 0 && c2 != 0) { sibling_running = 1; break; }
        }
        uint32_t inproc = 0;
        map_get_u32(g + 0x18, KEY_INPROC, &inproc);
        if ((sibling_running && inproc == 0) ||
            (!sibling_running && inproc != 0)) {
            static int dbg_inproc = 0;
            if (dbg_inproc < 5) {
                dbg_inproc++;
                log_direct("[au] load skip inproc mid=%llu#%d b=%d"
                        " sibling=%d inproc=%llu",
                        (unsigned long long)mid, machine_seq(g, mid), batch,
                        sibling_running, (unsigned long long)inproc);
            }
            return;
        }
    }

    /* qol 装料原料选择（ProductionBuildInputPlan）：从所有相连箱子收集
     * 同物品堆，按 anchor rank 顺序排序，跨堆凑足数量（多源合并） */
    for (int i = 0; i < g_recipe_n; i++) {
        Recipe* r = &g_recipes[i];
        if (r->machine != mid) continue;
        IngSrc osrcs[16], fsrcs[16];
        int on = collect_sources(chests, nchests, r->input, osrcs, 16);
        if (on == 0) {
            static int dbg_nosrc = 0;
            if (dbg_nosrc < 5) {
                dbg_nosrc++;
                log_direct("[au] load skip no-source mid=%llu#%d b=%d in=%llu"
                        " chests=%d",
                        (unsigned long long)mid, machine_seq(g, mid), batch,
                        (unsigned long long)r->input, nchests);
            }
            continue;
        }
        {
            int anchor = (int)osrcs[0].rank;   /* qol anchor：箱子第一个被接受物品 */
            sort_sources_by_rank(osrcs, on, anchor);
        }
        {
            int ore_total = 0;
            for (int k = 0; k < on; k++) ore_total += (int)osrcs[k].cnt;
            if (ore_total < r->input_cnt) {
                static int dbg_ore = 0;
                if (dbg_ore < 5) {
                    dbg_ore++;
                    log_direct("[au] load skip ore-short mid=%llu#%d b=%d"
                            " in=%llu have=%d need=%d",
                            (unsigned long long)mid, machine_seq(g, mid), batch,
                            (unsigned long long)r->input, ore_total,
                            r->input_cnt);
                }
                continue;
            }
        }
        int fn = 0;
        if (r->fuel) {
            fn = collect_sources(chests, nchests, r->fuel, fsrcs, 16);
            if (fn == 0) {
                static int dbg_nofuel = 0;
                if (dbg_nofuel < 5) {
                    dbg_nofuel++;
                    log_direct("[au] load skip no-fuel mid=%llu#%d b=%d fuel=%llu",
                            (unsigned long long)mid, machine_seq(g, mid), batch,
                            (unsigned long long)r->fuel);
                }
                continue;
            }
            {
                int anchor2 = (int)fsrcs[0].rank;
                sort_sources_by_rank(fsrcs, fn, anchor2);
            }
            {
                int fuel_total = 0;
                for (int k = 0; k < fn; k++)
                    fuel_total += (int)fsrcs[k].cnt;
                if (fuel_total < r->fuel_cnt) {
                    static int dbg_fuel = 0;
                    if (dbg_fuel < 5) {
                        dbg_fuel++;
                        log_direct("[au] load skip fuel-short mid=%llu#%d b=%d"
                                " fuel=%llu have=%d need=%d",
                                (unsigned long long)mid, machine_seq(g, mid), batch,
                                (unsigned long long)r->fuel, fuel_total,
                                r->fuel_cnt);
                    }
                    continue;
                }
            }
        }

        if (dry) {
            /* worker：只读扫描通过，排队给主线程执行 */
            add_lpending(g, mid, batch, chests, nchests);
            return;
        }

        /* ---- qol 装料完整移植 ----
         * （ProductionCreateInputIngredients + ProductionAttemptInputTransfer
         * 后半段，反汇编 0x26ED50 + qol 源码双向核对）
         * 顺序：克隆（refcount=0，AddRef 建交易 owned）→ duration → start
         * → itemIDN（原生 notify）→ itemValN（原生 notify）→ 扣箱子（源堆
         * 耗尽 detach）→ 写 backing（AddRef 槽持有）→ 最后 inProc=1
         * （原生 notify）→ release owned（intrusive release）。 */
        uint64_t now;
        if (!game_time(&now)) return;

        /* 1) 克隆原料/燃料：0xFF830 构造后 refcount=0（0x10F390），
         * AddRef 建立交易本地 owned 引用；rank 继承箱子源堆品质（+0x280）。 */
        uintptr_t ore_clone = make_item_native_18(
            r->input, (uint32_t)r->input_cnt);
        uintptr_t fuel_clone = 0;
        if (!ore_clone) {
            log_direct("[au] clone ore failed mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }
        if (!ref_add(ore_clone)) {
            log_direct("[au] ref_add ore failed");
            return;
        }
        {
            uint32_t rank = 0;
            for (int k = 0; k < on; k++)
                if (osrcs[k].rank > rank) rank = osrcs[k].rank;
            wr4(ore_clone + 0x280, rank);
            /* qol 克隆验证：itemId / rank / count 三字段 */
            {
                uint32_t ccnt = 0, crank = 0;
                if (!item_count(ore_clone, &ccnt) ||
                    ccnt != (uint32_t)r->input_cnt) {
                    intrusive_release(ore_clone);
                    log_direct("[au] clone ore count FAIL mid=%llu b=%d",
                            (unsigned long long)mid, batch);
                    return;
                }
                rd4(ore_clone + 0x280, &crank);
                if ((int)crank != (int)rank) {
                    intrusive_release(ore_clone);
                    log_direct("[au] clone ore rank FAIL mid=%llu b=%d",
                            (unsigned long long)mid, batch);
                    return;
                }
            }
        }
        if (r->fuel && fn > 0) {
            fuel_clone = make_item_native_18(r->fuel,
                                             (uint32_t)r->fuel_cnt);
            if (!fuel_clone || !ref_add(fuel_clone)) {
                intrusive_release(ore_clone);   /* 释放 owned，回滚 */
                log_direct("[au] clone fuel failed mid=%llu b=%d",
                        (unsigned long long)mid, batch);
                return;
            }
            uint32_t rank = 0;
            for (int k = 0; k < fn; k++)
                if (fsrcs[k].rank > rank) rank = fsrcs[k].rank;
            wr4(fuel_clone + 0x280, rank);
            {
                uint32_t ccnt = 0, crank = 0;
                if (!item_count(fuel_clone, &ccnt) ||
                    ccnt != (uint32_t)r->fuel_cnt) {
                    intrusive_release(fuel_clone);
                    intrusive_release(ore_clone);
                    return;
                }
                rd4(fuel_clone + 0x280, &crank);
                if ((int)crank != (int)rank) {
                    intrusive_release(fuel_clone);
                    intrusive_release(ore_clone);
                    return;
                }
            }
        }

        /* 配方运行时解析：产出数量 + 真实时长（不信任配置——
         * 蜂箱每朵花时长不同，配置全错） */
        if (r->output_cnt == 0) {
            uint32_t cnt = 1;
            uint64_t dur = r->duration;
            if (lookup_recipe(r->input, r->output, &cnt, &dur)) {
                r->output_cnt = (int)cnt;
                r->recipe_dur = dur;
            } else {
                r->output_cnt = 1;
            }
        }
        if (r->output_cnt <= 0 || r->output_cnt > 999)
            r->output_cnt = 1;
        uint64_t use_dur = r->recipe_dur ? r->recipe_dur : r->duration;

        /* 2) 状态写入（qol 顺序：先 duration 再 start，防中间态被判完成；
         * duration 用运行时配方真实时长） */
        wr8(g + 0x2A0 + batch * 8, use_dur);
        wr8(g + 0x288 + batch * 8, now);
        /* 3) itemIDN / itemValN：原生 setStatus（写 map + 事件通知）。
         * itemValN = 配方产出数量（qol 写 recipe->outputCount） */
        if (!status_set_u64(g,
                            key_itemid(batch),
                            r->output)) {
            wr8(g + 0x2A0 + batch * 8, 0);
            wr8(g + 0x288 + batch * 8, 0);
            intrusive_release(ore_clone);
            if (r->fuel && fuel_clone) intrusive_release(fuel_clone);
            log_direct("[au] status itemIDN FAIL mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }
        /* qol：notify 后 storage 可能被观察者重建，重读 backing 指针 */
        rdptr(g + 0x2B8, &oback);
        if (!oback || oback <= 0x10000 || oback >= 0x7FFF00000000ULL) {
            wr8(g + 0x2A0 + batch * 8, 0);
            wr8(g + 0x288 + batch * 8, 0);
            status_set_u64(g,
                           key_itemid(batch),
                           0);
            intrusive_release(ore_clone);
            if (r->fuel && fuel_clone) intrusive_release(fuel_clone);
            log_direct("[au] backing invalid after itemIDN notify"
                    " mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }
        if (!status_set_int(g,
                            key_itemval(batch),
                            r->output_cnt)) {
            wr8(g + 0x2A0 + batch * 8, 0);
            wr8(g + 0x288 + batch * 8, 0);
            status_set_u64(g,
                           key_itemid(batch),
                           0);
            intrusive_release(ore_clone);
            if (r->fuel && fuel_clone) intrusive_release(fuel_clone);
            log_direct("[au] status itemValN FAIL mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }
        rdptr(g + 0x2B8, &oback);
        if (!oback || oback <= 0x10000 || oback >= 0x7FFF00000000ULL) {
            wr8(g + 0x2A0 + batch * 8, 0);
            wr8(g + 0x288 + batch * 8, 0);
            status_set_u64(g,
                           key_itemid(batch),
                           0);
            status_set_int(g,
                           key_itemval(batch),
                           0);
            intrusive_release(ore_clone);
            if (r->fuel && fuel_clone) intrusive_release(fuel_clone);
            log_direct("[au] backing invalid after itemValN notify"
                    " mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }

        /* 4) 扣箱子多源（qol ApplyInputSources：adjust -amount；
         * 源耗尽 detach 槽+释放槽引用） */
        apply_sources(osrcs, on, r->input_cnt);
        if (r->fuel && fn > 0)
            apply_sources(fsrcs, fn, r->fuel_cnt);

        /* 5) 写机器 backing（AddRef 槽持有 + 写入） */
        if (!ref_add(ore_clone)) {
            intrusive_release(ore_clone);
            return;
        }
        wr8(oback + ore_slot * 8, ore_clone);
        {
            uintptr_t rb = 0;
            if (!rdptr(oback + ore_slot * 8, &rb) || rb != ore_clone) {
                wr8(oback + ore_slot * 8, 0);
                intrusive_release(ore_clone);
                intrusive_release(ore_clone);   /* slot + owned 两份 */
                log_direct("[au] backing ore readback FAIL mid=%llu b=%d",
                        (unsigned long long)mid, batch);
                return;
            }
        }
        if (r->fuel && fuel_clone) {
            if (!ref_add(fuel_clone)) return;
            wr8(oback + fuel_slot * 8, fuel_clone);
            {
                uintptr_t rb = 0;
                if (!rdptr(oback + fuel_slot * 8, &rb) || rb != fuel_clone) {
                    wr8(oback + fuel_slot * 8, 0);
                    wr8(oback + ore_slot * 8, 0);
                    intrusive_release(fuel_clone);
                    intrusive_release(fuel_clone);
                    intrusive_release(ore_clone);   /* 回滚 ore 槽持有 */
                    intrusive_release(ore_clone);
                    log_direct("[au] backing fuel readback FAIL mid=%llu b=%d",
                            (unsigned long long)mid, batch);
                    return;
                }
            }
        }

        /* 6) 最后 inProc=1（原生 notify） */
        if (!status_set_int(g, KEY_INPROC, 1)) {
            wr8(g + 0x2A0 + batch * 8, 0);
            wr8(g + 0x288 + batch * 8, 0);
            status_set_u64(g,
                           key_itemid(batch),
                           0);
            status_set_int(g,
                           key_itemval(batch),
                           0);
            wr8(oback + ore_slot * 8, 0);
            if (r->fuel && fuel_clone)
                wr8(oback + fuel_slot * 8, 0);
            intrusive_release(ore_clone);
            intrusive_release(ore_clone);
            if (r->fuel && fuel_clone) {
                intrusive_release(fuel_clone);
                intrusive_release(fuel_clone);
            }
            log_direct("[au] status inProc FAIL mid=%llu b=%d",
                    (unsigned long long)mid, batch);
            return;
        }

        /* 7) release 交易本地 owned 引用（槽持有保留 1，qol 语义） */
        intrusive_release(ore_clone);
        if (r->fuel && fuel_clone) intrusive_release(fuel_clone);

        log_direct("[au] LOAD (qol) mid=%llu#%d batch=%d in=%llu x%d(stack %u)"
                " fuel=%llu x%d -> out=%llu dur=%llu(cfg %llu) now=%llu",
                (unsigned long long)mid, machine_seq(g, mid), batch,
                (unsigned long long)r->input, r->input_cnt,
                (unsigned)osrcs[0].cnt,
                (unsigned long long)r->fuel, r->fuel_cnt,
                (unsigned long long)r->output,
                (unsigned long long)use_dur,
                (unsigned long long)r->duration, (unsigned long long)now);
        add_load(g, batch, r->output, now);
        return;
    }
}

/* ---- 取料：成品移入相连箱子 ---- */

/* 在机器容器里找 dataID==want 的物品槽（qol 布局）。
 * 机器 +0x2B8 是一个指针，指向固定 3×8=24 个 Item* 的 backing 数组
 * （qol：GimmickProcessPopItem loads exactly one pointer from status+0x2B8
 *  and indexes its fixed 3 x 8 backing array；+0x2C0/+0x2C8 不是 vector
 *  end/cap）。成品在 backing[slot*8+0..7] 中，直接按指针索引，
 * 不再把 +0x2B8 当 vector（vec_slots）读。
 * 返回槽地址（0=没找到），*how 记录命中布局。 */

/* 把机器槽里剩余原料退回相连箱子（防止卡料/重复装料） */

/* 该 dataID 是不是 mid 这台机器某配方的成品（且不是原料/燃料） */


/* 无台账时收遗留成品：容器里出现该机器配方成品（且非原料）就搬箱子。
 * 覆盖：mod 重启后机器里已有烧完的成品、或游戏自己加工完的机器。 */

/* 把机器 pslot 槽的成品 it 搬进相连箱子（优先已有同种物品的堆，再空位） */

/* 扫机器容器（布局 A + B），把配方成品搬进箱子 */

/* 测试开关 ForceFinish=1：批次已建立（rec 存在）时把开始时间提前，
 * 游戏下一 tick 会判定完成并走自己的结算入容器，用于快速验证取料闭环。
 * 只在 mod 台账登记的批次上生效。 */
static void force_finish(uintptr_t g, int batch)
{
    uint32_t flg = 0;
    rd4(g + 0x224, &flg);
    uintptr_t arr = 0, rec = 0;
    if (!rdptr(g + (flg ? 0x258 : 0x240), &arr) || !arr) return;
    if (!rdptr(arr + (uintptr_t)batch * 24, &rec) || !rec) return;
    uint64_t t0 = 0, ct = 0, now;
    rd8(g + 0x288 + batch * 8, &t0);
    rd8(g + 0x2A0 + batch * 8, &ct);
    if (t0 == 0 || ct == 0) return;
    if (!game_time(&now)) return;
    if (now >= t0 + ct) return;   /* 已到时间，不需要强制 */
    uint64_t nt = now >= ct ? now - ct : 0;
    if (g_diag_round < 20)
        log_direct("[au] force-finish batch=%d t0=%llu ct=%llu -> %llu",
                batch, (unsigned long long)t0, (unsigned long long)ct,
                (unsigned long long)nt);
    wr8(g + 0x288 + batch * 8, nt);
}

/* ---- 取料检测（已废弃，仅保留编译期引用） ---- */

/* ---- 箱子/机器扫描上下文 ---- */
/* 所有机器（连通组洪泛让所有机器都中继，
 * 星露谷式"机器相邻即连接"，修复研磨机隔着熔炉连不到箱子） */

static int is_machine_did(uint64_t did)
{
    for (int i = 0; i < g_machine_n; i++)
        if (g_machines[i] == did) return 1;
    return 0;
}

/* 地板连接已取消，不参与收集。 */
static int scan_all_cb(uintptr_t g, void* ctx)
{
    (void)ctx;
    uint64_t did;
    if (!obj_dataid(g, &did)) return 0;
    uint32_t ix, iy;
    if (!rd4(g + 0xF0, &ix) || !rd4(g + 0xF4, &iy)) return 0;
    float cx, cy;
    memcpy(&cx, &ix, 4);
    memcpy(&cy, &iy, 4);
    if (is_machine_did(did)) {
        if (g_all_machine_n < (int)(sizeof(g_all_machines) /
                                    sizeof(g_all_machines[0]))) {
            g_all_machines[g_all_machine_n].g = g;
            g_all_machines[g_all_machine_n].x = cx;
            g_all_machines[g_all_machine_n].y = cy;
            g_all_machines[g_all_machine_n].did = did;
            g_all_machine_n++;
        }
        return 0;
    }
    /* 箱子系列 241000xxx（含 10/20/30/40 格等各级箱子） */
    if (did >= 241000000 && did <= 241000999) {
        if (g_all_n < MAX_MACHINE) {
            g_all_chests[g_all_n].g = g;
            g_all_chests[g_all_n].x = cx;
            g_all_chests[g_all_n].y = cy;
            g_all_n++;
        }
    }
    return 0;
}

/* 邻格判定：8 方向 1 格（切比雪夫距离 <= g_tile） */
static int adjacent(float ax, float ay, float bx, float by)
{
    float dx = ax - bx;
    float dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= (float)g_tile && dy <= (float)g_tile;
}

/* 星露谷式连通组：所有机器/箱子参与洪泛中继
 * （机器相邻即连接，修复不同类型机器隔着不连通的问题） */
static void build_group(float mx, float my, uintptr_t* machines, int nmach)
{
    static float mx_[MAX_MACHINE * 4], my_[MAX_MACHINE * 4];
    static char inm[MAX_MACHINE * 4], inc[MAX_MACHINE];
    (void)machines;
    (void)nmach;
    int nm = g_all_machine_n;
    g_scan_n = 0;
    if (nm <= 0 || g_all_n <= 0) return;
    memset(inm, 0, nm);
    memset(inc, 0, g_all_n);
    int seed = -1;
    for (int i = 0; i < nm; i++) {
        mx_[i] = g_all_machines[i].x;
        my_[i] = g_all_machines[i].y;
        if (mx_[i] == mx && my_[i] == my)
            seed = i;
    }
    if (seed < 0) return;
    inm[seed] = 1;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < nm; i++) {
            if (inm[i]) continue;
            for (int j = 0; j < nm; j++) {
                if (inm[j] && adjacent(mx_[i], my_[i], mx_[j], my_[j])) {
                    inm[i] = 1;
                    changed = 1;
                    break;
                }
            }
            if (inm[i]) continue;
            for (int j = 0; j < g_all_n; j++) {
                if (inc[j] &&
                    adjacent(mx_[i], my_[i],
                             g_all_chests[j].x, g_all_chests[j].y)) {
                    inm[i] = 1;
                    changed = 1;
                    break;
                }
            }
        }
        for (int i = 0; i < g_all_n; i++) {
            if (inc[i]) continue;
            for (int j = 0; j < nm; j++) {
                if (inm[j] &&
                    adjacent(g_all_chests[i].x, g_all_chests[i].y,
                             mx_[j], my_[j])) {
                    inc[i] = 1;
                    changed = 1;
                    break;
                }
            }
            if (inc[i]) continue;
            for (int j = 0; j < g_all_n; j++) {
                if (inc[j] &&
                    adjacent(g_all_chests[i].x, g_all_chests[i].y,
                             g_all_chests[j].x, g_all_chests[j].y)) {
                    inc[i] = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }
    /* g_scan_chests 容量 MAX_MACHINE，越界写会破坏相邻
     * static 锁变量（2026-08-29 锁死取证后加） */
    for (int i = 0; i < g_all_n && g_scan_n < MAX_MACHINE; i++)
        if (inc[i])
            g_scan_chests[g_scan_n++] = g_all_chests[i].g;
}

/* 处理单台机器（装料/取料/残留自愈）。调用顺序由 worker 每轮
 * order_machines_bfs 决定。
 * 连接语义由 build_group 单独判定，与此处顺序无关。 */
static void process_machine(uintptr_t g, uint64_t mid)
{
    uint32_t ix, iy;
    if (!rd4(g + 0xF0, &ix) || !rd4(g + 0xF4, &iy)) return;
    float mx, my;
    memcpy(&mx, &ix, 4);
    memcpy(&my, &iy, 4);
    build_group(mx, my, NULL, 0);
    if (g_scan_n <= 0) return;
    if (has_stale_slots(g)) add_cleanp(g, mid, 0);
    if (idle_residue(g)) add_cleanp(g, mid, 1);
    int nb = machine_slot_limit(g);
    if (g_load_enabled) {
        for (int b = 0; b < nb; b++) {
            load_batch(g, mid, b, g_scan_chests, g_scan_n, 1);
            if (g_force_finish) force_finish(g, b);
        }
    }
    if (g_collect_enabled && !fault_isolated(g)) {
        for (int b = 0; b < nb; b++)
            if (batch_ready(g, b))
                add_pending(g, mid, b, g_scan_chests, g_scan_n);
    }
}

/* 多源 BFS 处理顺序——以所有箱子为起点，机器按到最近箱子
 * 的跳数逐层扩散（水流式：箱子→机器1→机器2→…）。邻接规则与
 * build_group 相同（adjacent，切比雪夫 <= g_tile）；同层保持
 * g_all_machines 原顺序，结果稳定；连不到箱子的机器排最后
 * （process_machine 内 g_scan_n<=0 会直接返回，无副作用）。 */
static void order_machines_bfs(int* order, int* pn)
{
    static char vm[MAX_MACHINE * 4];
    static int q[MAX_MACHINE * 4];
    int nm = g_all_machine_n, nc = g_all_n;
    *pn = 0;
    if (nm <= 0) return;
    if (nc <= 0) {
        for (int i = 0; i < nm; i++) order[i] = i;
        *pn = nm;
        return;
    }
    memset(vm, 0, nm);
    int head = 0, tail = 0;
    /* 第 1 层：与任一箱子相邻的机器 */
    for (int i = 0; i < nm; i++) {
        if (vm[i]) continue;
        for (int j = 0; j < nc; j++) {
            if (adjacent(g_all_machines[i].x, g_all_machines[i].y,
                         g_all_chests[j].x, g_all_chests[j].y)) {
                vm[i] = 1;
                q[tail++] = i;
                order[(*pn)++] = i;
                break;
            }
        }
    }
    /* 后续层：机器与机器相邻即连接，逐层外扩 */
    while (head < tail) {
        int cur = q[head++];
        float cx = g_all_machines[cur].x;
        float cy = g_all_machines[cur].y;
        for (int i = 0; i < nm; i++) {
            if (vm[i]) continue;
            if (adjacent(cx, cy, g_all_machines[i].x,
                         g_all_machines[i].y)) {
                vm[i] = 1;
                q[tail++] = i;
                order[(*pn)++] = i;
            }
        }
    }
    /* 不可达机器兜底（保持原拜访顺序，不参与装料取料） */
    for (int i = 0; i < nm; i++)
        if (!vm[i]) order[(*pn)++] = i;
}

static DWORD WINAPI worker_thread(LPVOID param)
{
    (void)param;
    Sleep(400);
    /* 路径/配置初始化已由基座在 mod_init 完成；这里只跑主循环 */
    while (1) {
        Sleep((DWORD)g_poll_ms);
        g_diag_round++;
        if (get_mtime() != g_cfg_mtime)
            read_config();
        uintptr_t root, s;
        if (!g_a_clock || !rdptr(g_a_clock, &root)) continue;
        if (!rdptr(root + 0x208, &s)) continue;
        if (g_diag_round % 30 == 0) {
            log_direct("[au] heartbeat round=%d machines=%d collected=%d"
                    " chests=%d",
                    g_diag_round, g_machine_n, g_all_machine_n, g_all_n);
        }
        g_all_machine_n = 0;
        g_all_n = 0;
        walk_gimmicks(s, scan_all_cb, NULL);
        if (g_diag_round < 3) {
            for (int mi = 0; mi < g_all_machine_n; mi++) {
                uintptr_t gm = g_all_machines[mi].g;
                uint64_t mid = 0;
                obj_dataid(gm, &mid);
                int nb2 = machine_slot_limit(gm);
                for (int b = 0; b < nb2; b++) {
                    uint64_t t0 = 0, ct = 0, idn = 0, val = 0;
                    rd8(gm + 0x288 + b * 8, &t0);
                    rd8(gm + 0x2A0 + b * 8, &ct);
                    map_get_u64(gm + 0x58,
                                key_itemid(b),
                                &idn);
                    map_get_u64(gm + 0x18,
                                key_itemval(b),
                                &val);
                    log_direct("[au] SLOTDIAG g=%p mid=%llu b=%d t0=%llu"
                            " ct=%llu idn=%llu val=%llu",
                            (void*)gm, (unsigned long long)mid, b,
                            (unsigned long long)t0, (unsigned long long)ct,
                            (unsigned long long)idn,
                            (unsigned long long)val);
                }
            }
        }
        static int order_arr[MAX_MACHINE * 4];
        int order_n = 0;
        order_machines_bfs(order_arr, &order_n);
        for (int i = 0; i < order_n; i++) {
            int mi = order_arr[i];
            if (mi < 0 || mi >= g_all_machine_n) continue;
            __try {
                process_machine(g_all_machines[mi].g,
                                g_all_machines[mi].did);
            } __except (1) {
                /* single-machine exception, skip */
            }
        }
    }
    return 0;
}

/* ---- 游戏主线程执行（系统消息钩子 WH_GETMESSAGE） ----
 * 不 patch 游戏代码（inline hook 导致游戏打不开）：
 * SetWindowsHookEx(WH_GETMESSAGE) 挂到游戏主线程，游戏消息循环
 * 每处理一条消息就在主线程回调 mod——与 QoL"游戏主动调用→拦截"
 * 同一类安全机制，取料才能走玩家同款路径（TLS/分配器可用）。 */


static void automate_tick_main(void)
{
    static uint64_t last_ms = 0;
    uint64_t now = GetTickCount64();
    /* 主线程只做轻量取料（300ms 限频，响应快不卡帧） */
    if (now - last_ms < 300) return;
    last_ms = now;
    /* 主线程执行装料（0xFF830 构造物品必须主线程） */
    __try {
        /* 先清伪槽（半截/无效槽），再装料，避免新装到残留槽上 */
        process_clean_pending();
        process_load_pending();
    } __except (1) {
        /* 单轮装料异常不拖垮 */
    }
    /* 处理 worker 标记的完成批次（主线程创建物品才安全）。
     * 每 tick 最多 4 批 + 8ms 预算，剩余留队列
     * 下次 tick 继续，跨日高峰摊开不冻结。 */
#define COLLECT_MAX_PER_TICK 16
#define COLLECT_BUDGET_US    16000
    PendingRec local[COLLECT_MAX_PER_TICK];
    int take = 0;
    lock_wait(&g_pending_lock, "pending_proc");
    __try {
        int n = g_pending_n;
        take = n > COLLECT_MAX_PER_TICK ? COLLECT_MAX_PER_TICK : n;
        for (int i = 0; i < take; i++)
            local[i] = g_pending[i];
        if (n > take)
            memmove(g_pending, g_pending + take,
                    (size_t)(n - take) * sizeof(PendingRec));
        g_pending_n = n - take;
    } __except (1) {
        take = 0;
    }
    InterlockedExchange(&g_pending_lock, 0);
    LARGE_INTEGER freq = { 0 };
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0 = { 0 };
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < take; i++) {
        PendingRec* p = &local[i];
        __try {
            /* 传 pending 快照，不碰 worker 正在写的全局 g_scan_* */
            collect_product(p->g, p->mid, p->batch,
                            p->chests, p->nchests);
        } __except (1) {
            /* 单批次取料异常不拖垮游戏 */
        }
        if (freq.QuadPart > 0) {
            LARGE_INTEGER t1 = { 0 };
            QueryPerformanceCounter(&t1);
            LONGLONG us = (t1.QuadPart - t0.QuadPart) * 1000000LL /
                          freq.QuadPart;
            if (us >= COLLECT_BUDGET_US) break;
        }
    }
}

/* ---- dinput8 宿主版：窗口过程替换 + 定时器（等游戏窗口出现后安装） ---- */
#define AUTOMATE_TIMER_ID 0xA1

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
#ifdef AUTOMATE_MOD
        /* 独立 mod 版：日志放 mod 自己的目录（Mods\Automate_v0.4.0\） */
        GetModuleFileNameA(hinst, g_log_path, MAX_PATH);
        {
            char* slash = strrchr(g_log_path, '\\');
            if (slash) *(slash + 1) = 0;
        }
        lstrcatA(g_log_path, "automate.log");
        log_init(); /* 打开日志（截断旧文件；早于 mod_init 与 addr_init） */
        /* DllMain 最小化：建线程交给基座调 mod_init（loader lock 已释放） */
#elif defined(DINPUT8_PROXY)
        /* dinput8 宿主版：DllMain 最小化——loader lock 下 CreateThread
         * + 线程执行文件/loader 操作会死锁（rundll32/游戏启动挂起）。
         * 初始化延迟到游戏首次调用 dinput8 导出（DirectInput8Create） */
        GetModuleFileNameA(hinst, g_log_path, MAX_PATH);
        {
            char* slash = strrchr(g_log_path, '\\');
            if (slash) *(slash + 1) = 0;
        }
        lstrcatA(g_log_path, "automate.log");
#else
        GetModuleFileNameA(hinst, g_log_path, MAX_PATH);
        {
            char* slash = strrchr(g_log_path, '\\');
            if (slash) *(slash + 1) = 0;
        }
        lstrcatA(g_log_path, "automate.log");
        log_init(); /* 打开日志（截断旧文件） */
        HANDLE h = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
#endif
    }
    return TRUE;
}

/* ==================================================================
 * 7 生命周期：基座契约入口（mod_init / mod_tick）
 * ================================================================== */

/* ---- 基座契约入口 ---- */

__declspec(dllexport) void mod_init(void)
{
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    /* 日志已由 DllMain 的 log_init() 截断打开；
     * 这里不能再 DeleteFileA，否则会删掉正在写的日志文件 */
    addr_init();
    if (!g_a_clock || !g_a_alloc || !g_a_map_find || !g_a_item_ctor ||
        !g_a_native_load || !g_a_native_collect || !g_a_output_helper ||
        !g_a_u64_map_find || !g_a_event_lookup || !g_a_event_notify ||
        !g_a_ui_check_jz ||
        !g_a_map_getobj || !g_a_mapA || !g_a_gimmickMgr) {
        log_direct("[au] FATAL: addrsig MISS, mod disabled");
        return;
    }
    if (!g_a_recipe_table || !g_a_recipeMgr)
        log_direct("[au] recipeMgr MISS: itemValN falls back to 1");
    lstrcpyA(g_cfg_path, g_log_path);
    {
        char* slash = strrchr(g_cfg_path, '\\');
        if (slash) *(slash + 1) = 0;
    }
    lstrcatA(g_cfg_path, "automate.txt");
    read_config();
    log_direct("[au] Automate v0.5.4 loaded; base=%p", (void*)g_base);
    /* 装料/检测在后台线程（不卡主线程），取料交主线程 mod_tick */
    HANDLE h = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

__declspec(dllexport) void mod_tick(void)
{
    __try {
        automate_tick_main();
    } __except (1) {
        /* 自动物流异常不拖垮游戏 */
    }
}
