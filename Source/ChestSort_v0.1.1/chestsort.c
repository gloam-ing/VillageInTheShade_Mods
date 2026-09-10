/* chestsort.c - ChestSort v0.1.1（Village in the Shade 1.08.1）
 * ------------------------------------------------------------------
 * 单文件实现（原 chestsort_core.c + ChestSort.c 合并）；
 * 公共机制来自 Mods\shared\（modkit：日志/内存）。
 * 分区：1 版本档案（地址表+入口字节签名） / 2 核心逻辑 / 3 入口
 *       （mod_init / mod_tick）
 *
 * 功能：F8 / 手柄方向上，把背包物品归并进范围内【已有匹配堆叠】的
 *   箱子（item_id + rank），只合并已有堆叠不新建空槽，最满优先；
 *   原生容量/数量/引用计数事务 + 失败回滚 + 批刷新，右下角 toast。
 *
 * 版本策略：1.08.1 固定 RVA + 入口字节签名校验（不做 addrsig）；
 *   任一签名不符即 fail-closed 停用，绝不硬用旧地址。
 *
 * 编译（在 Mods\ChestSort_v0.1.1 下执行）：
 *   zig cc -shared -O2 -fms-extensions -I..\shared -o ChestSort.dll
 *     chestsort.c ..\shared\addrsig.c ..\shared\modkit.c
 *     -lgdi32 -luser32   (xinput 由 LoadLibrary 动态加载)
 */

/* chestsort_core.c - 附近箱子归类（范围归纳）核心
 * ------------------------------------------------------------------
 * 移植自 VillageQoL open_source/nearby_chest_sort.inl（1.06 build），
 * 逻辑与事务顺序保持一致：
 *   - 以玩家为中心 720（16 世界格）半径，经地图空间索引收集箱子
 *   - 只把背包物品归并进"已有匹配堆叠（item_id + rank）"的箱子槽，
 *     不新建空槽分类；多个匹配堆叠时选最满的，装满再选下一个
 *   - 原生 CItemStatus 容量判定 + 数量调整 + intrusive 引用计数，
 *     每次移动前实时校验、失败回滚，全批完成后一次 UI 刷新
 * 地址由 chestsort_mod.c 解析后注入（函数指针 + 全局 RVA）。
 */

#ifndef CHESTSORT_CORE_H
#define CHESTSORT_CORE_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 对象布局（1.08.1 与 qol 1.06 一致） */
#define CS_ROOT_PLAYER_OFF           0x208   /* root+0x208 = player(mgr) */
#define CS_ROOT_MAP_OWNER_OFF        0x268   /* root+0x268 = map owner */
#define CS_PLAYER_OBJECT_STATUS_OFF  0x32b8  /* player+0x32b8 = objectStatus */
#define CS_PLAYER_OBJECT_LINK_A_OFF  0x410
#define CS_PLAYER_OBJECT_LINK_B_OFF  0x008
#define CS_PLAYER_OBJECT_ID_OFF      0x028
#define CS_PLAYER_ITEMS_OFF          0x32c0
#define CS_WORLD_OBJECT_POS_OFF      0x230
#define CS_WORLD_OBJECT_DIRTY_OFF    0x300
#define CS_MAP_INFO_OFF              0x0d8
#define CS_MAP_SPATIAL_OWNER_OFF     0x030
#define CS_MAP_SPATIAL_INDEX_OFF     0x6e0
#define CS_STATUS_ITEMS_OFF          0x2b8
#define CS_ITEM_DATA_HOLDER_OFF      0x240
#define CS_ITEM_STACK_COUNT_OFF      0x260
#define CS_ITEM_RANK_OFF             0x280
#define CS_GIMMICK_DATA_HOLDER_OFF   0x240
#define CS_GIMMICK_MODULE_NAME_OFF   0x0d8
#define CS_GIMMICK_POSITION_OFF      0x0f0

#define CS_CHEST_SLOT_COUNT          30
#define CS_MAX_CANDIDATES            32
#define CS_READABLE_REGION_CACHE     256

typedef struct CsRect {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} CsRect;

typedef struct CsRawVector {
    void** begin;
    void** end;
    void** capacity;
} CsRawVector;

typedef struct CsItemInfo {
    uint64_t item_id;
    int stack_count;
    int rank;
} CsItemInfo;

typedef struct CsChestCandidate {
    void* status;
    float position_x;
    float position_y;
    float distance_squared;
} CsChestCandidate;

typedef struct CsDestPlan {
    void* item;
    size_t chest_slot;
    int before_count;
    int amount;
} CsDestPlan;

typedef struct CsSlotSnapshot {
    void* item;
    CsItemInfo info;
    int available;
    int valid;
    int capacity_known;
} CsSlotSnapshot;

typedef struct CsChestSnapshot {
    void* status;
    CsSlotSnapshot slots[CS_CHEST_SLOT_COUNT];
    int valid;
} CsChestSnapshot;

typedef struct CsPlayerSlotSnapshot {
    void* item;
    CsItemInfo info;
    int valid;
} CsPlayerSlotSnapshot;

typedef struct CsTransferMetrics {
    uint64_t item_reads;
    uint64_t plan_calls;
    uint64_t chest_slot_visits;
    uint64_t duplicate_count_comparisons;
    uint64_t best_count_replacements;
    uint64_t capacity_calls;
    uint64_t matching_targets;
    uint64_t positive_capacity_targets;
    uint64_t zero_capacity_nonstackable_targets;
    uint64_t zero_capacity_full_or_nonstackable_targets;
    uint64_t forbidden_calls;
    LONGLONG plan_ticks;
    LONGLONG capacity_ticks;
    LONGLONG forbidden_ticks;
} CsTransferMetrics;

typedef struct CsReadableRegion {
    uintptr_t begin;
    uintptr_t end;
} CsReadableRegion;

typedef struct CsReadableCache {
    CsReadableRegion regions[CS_READABLE_REGION_CACHE];
    size_t count;
    size_t replacement;
    size_t last_hit;
    uint64_t checks;
    uint64_t cache_hits;
    uint64_t virtual_queries;
    LONGLONG virtual_query_ticks;
    uint64_t live_virtual_queries;
    LONGLONG live_virtual_query_ticks;
    int reuse_enabled;
} CsReadableCache;

/* 原生函数指针（由 mod 层解析地址后注入） */
typedef void*  (__fastcall *CsResolveWorldObjectFn)(void*, uint64_t);
typedef void   (__fastcall *CsRefreshWorldObjectFn)(void*);
typedef struct CsCommandItem {
    void* item;
    unsigned char enabled;
    unsigned char pad[3];
    int field_0c;
    int state;
    unsigned char dirty;
    unsigned char pad2[3];
} CsCommandItem;
typedef int    (__fastcall *CsCommandCapacityFn)(CsCommandItem*);
typedef void   (__fastcall *CsIntrusiveReleaseFn)(void**);
typedef void   (__fastcall *CsItemAdjustFn)(void*, int);
typedef int    (__fastcall *CsForbiddenItemFn)(uint64_t);
typedef int    (__fastcall *CsPlayerCapacityFn)(void*);
typedef void   (__fastcall *CsBatchRecalculateFn)(void*, int);
typedef void   (__fastcall *CsBatchUiRefreshFn)(void*, int);
typedef void   (__fastcall *CsBatchDirtyFn)(void*, int);

typedef struct CsNative {
    CsResolveWorldObjectFn resolve;
    CsRefreshWorldObjectFn refresh;
    CsCommandCapacityFn    capacity;
    CsIntrusiveReleaseFn   release;
    CsItemAdjustFn         item_adjust;
    CsForbiddenItemFn      forbidden;
    CsPlayerCapacityFn     player_capacity;
    CsBatchRecalculateFn   batch_recalculate;
    CsBatchUiRefreshFn     batch_ui_refresh;
    CsBatchDirtyFn         batch_dirty;
    uintptr_t              game_root;      /* 时钟全局 RVA（root 指针槽） */
    uintptr_t              world_registry; /* gimmickMgr 全局 RVA */
} CsNative;

/* toast 状态（mod 层显示） */
enum CsToastCode {
    CS_TOAST_NONE = 0,
    CS_TOAST_SUCCESS = 1,
    CS_TOAST_NO_CHESTS = 2,
    CS_TOAST_NO_MATCH = 3,
    CS_TOAST_STOPPED = 4,
};

typedef struct CsToastState {
    volatile LONG code;
    volatile LONG stacks;
    volatile LONG items;
    volatile LONG chests;
} CsToastState;

/* 运行状态 */
typedef struct CsState {
    CsNative native;
    CsToastState toast;
    volatile LONG ready;
    volatile LONG faulted;
    volatile LONG running;
    volatile LONG64 quarantine;     /* 0 = 无滞留引用 */
    volatile ULONGLONG sequence;
    LONGLONG perf_frequency;
    float radius;                   /* 0 = 全图；>0 = 距离过滤 */
} CsState;

/* 全局运行状态（单实例） */
extern CsState g_cs;

/* 日志回调（mod 层注入） */
extern void (*cs_logf)(const char* fmt, ...);

/* 主入口：input_source = "F8"/"DPAD_UP"/NULL；主线程调用 */
void cs_run_sort(const char* input_source);

/* mod 层每帧调用，处理 toast 消息队列 */
void cs_pump_toast(void);

/* 基础：可读性检查（含一次性页面缓存） */
int cs_is_readable(const void* pointer, size_t size);
void cs_cache_begin(CsReadableCache* cache);
void cs_cache_end(void);

#ifdef __cplusplus
}
#endif

#endif /* CHESTSORT_CORE_H */

/* ------------------------------------------------------------------ */
/* 实现                                                               */
/* ------------------------------------------------------------------ */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

CsState g_cs;
void (*cs_logf)(const char* fmt, ...) = NULL;

static __declspec(thread) CsReadableCache* t_cs_cache = NULL;

static void cs_log(const char* fmt, ...)
{
    if (!cs_logf) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cs_logf("%s", buf);
}

static LONGLONG cs_perf_counter(void)
{
    LARGE_INTEGER value = { 0 };
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

static double cs_elapsed_ms(LONGLONG begin, LONGLONG end)
{
    if (g_cs.perf_frequency <= 0 || begin <= 0 || end < begin) return -1.0;
    return (double)(end - begin) * 1000.0 / (double)g_cs.perf_frequency;
}

void cs_cache_begin(CsReadableCache* cache)
{
    if (!cache) return;
    memset(cache, 0, sizeof(*cache));
    cache->reuse_enabled = 1;
    t_cs_cache = cache;
}

void cs_cache_end(void)
{
    t_cs_cache = NULL;
}

int cs_is_readable(const void* pointer, size_t size)
{
    CsReadableCache* cache = t_cs_cache;
    if (!cache) {
        if (!pointer || size == 0) return 0;
        MEMORY_BASIC_INFORMATION mbi = { 0 };
        if (VirtualQuery(pointer, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
        return 1;
    }
    ++cache->checks;
    if (!pointer || size == 0) return 0;
    const uintptr_t start = (uintptr_t)pointer;
    if (start < 0x10000 || start > 0x00007FFFFFFFFFFFULL ||
        start + size < start) return 0;
    const uintptr_t requested_end = start + size;

    if (cache->reuse_enabled) {
        size_t count = cache->count;
        size_t start_index = count ? (cache->last_hit % count) : 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = (start_index + i) % count;
            const CsReadableRegion* region = &cache->regions[idx];
            if (start >= region->begin && requested_end <= region->end) {
                ++cache->cache_hits;
                cache->last_hit = idx;
                return 1;
            }
        }
    }

    MEMORY_BASIC_INFORMATION mbi = { 0 };
    ++cache->virtual_queries;
    const LONGLONG qb = cs_perf_counter();
    const SIZE_T qr = VirtualQuery(pointer, &mbi, sizeof(mbi));
    const LONGLONG qt = cs_perf_counter() - qb;
    cache->virtual_query_ticks += qt;
    if (!cache->reuse_enabled) {
        ++cache->live_virtual_queries;
        cache->live_virtual_query_ticks += qt;
    }
    if (qr != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return 0;
    const uintptr_t region_begin = (uintptr_t)mbi.BaseAddress;
    const uintptr_t region_end = region_begin + mbi.RegionSize;
    if (region_end < region_begin || start < region_begin ||
        requested_end > region_end) return 0;

    if (cache->reuse_enabled) {
        size_t index = cache->count;
        if (cache->count < CS_READABLE_REGION_CACHE) {
            ++cache->count;
        } else {
            index = cache->replacement;
            cache->replacement =
                (cache->replacement + 1) % CS_READABLE_REGION_CACHE;
        }
        cache->regions[index].begin = region_begin;
        cache->regions[index].end = region_end;
    }
    return 1;
}

static int cs_read_pointer(const void* object, uintptr_t offset, void** output)
{
    if (!object || !output) return 0;
    const void* field = (const void*)((uintptr_t)object + offset);
    if (!cs_is_readable(field, sizeof(void*))) return 0;
    *output = *(void* const*)field;
    return *output != NULL;
}

static int cs_read_item(void* item, CsItemInfo* output)
{
    if (!item || !output ||
        !cs_is_readable(item, CS_ITEM_RANK_OFF + sizeof(int))) return 0;
    const LONG refs = *(volatile LONG*)((uintptr_t)item + sizeof(void*));
    if (refs <= 0) return 0;
    void* holder = *(void**)((uintptr_t)item + CS_ITEM_DATA_HOLDER_OFF);
    void* data = NULL;
    if (!holder || !cs_read_pointer(holder, 0, &data) ||
        !cs_is_readable(data, sizeof(uint64_t))) return 0;
    output->item_id = *(const uint64_t*)data;
    output->stack_count = *(const int*)((uintptr_t)item + CS_ITEM_STACK_COUNT_OFF);
    output->rank = *(const int*)((uintptr_t)item + CS_ITEM_RANK_OFF);
    return output->item_id != 0 && output->stack_count > 0 &&
           output->stack_count <= 999;
}

static int cs_read_stack_count(void* item, int* output)
{
    if (!item || !output ||
        !cs_is_readable((void*)((uintptr_t)item + CS_ITEM_STACK_COUNT_OFF),
                        sizeof(int))) return 0;
    const int count = *(const int*)((uintptr_t)item + CS_ITEM_STACK_COUNT_OFF);
    if (count < 0 || count > 999) return 0;
    *output = count;
    return 1;
}

static int cs_read_raw_inventory(void* status, CsRawVector* output)
{
    if (!status || !output ||
        !cs_is_readable((void*)((uintptr_t)status + CS_STATUS_ITEMS_OFF),
                        sizeof(*output))) return 0;
    *output = *(const CsRawVector*)((uintptr_t)status + CS_STATUS_ITEMS_OFF);
    const uintptr_t begin = (uintptr_t)output->begin;
    const uintptr_t end = (uintptr_t)output->end;
    const uintptr_t capacity = (uintptr_t)output->capacity;
    if (!begin || end < begin || capacity < end ||
        (end - begin) % sizeof(void*) != 0 ||
        (capacity - begin) % sizeof(void*) != 0) return 0;
    const size_t count = (end - begin) / sizeof(void*);
    const size_t capacity_count = (capacity - begin) / sizeof(void*);
    return count == CS_CHEST_SLOT_COUNT &&
           capacity_count >= count && capacity_count <= 64 &&
           cs_is_readable(output->begin, count * sizeof(void*));
}

static void cs_add_reference(void* item)
{
    InterlockedIncrement((volatile LONG*)((uintptr_t)item + sizeof(void*)));
}

static void cs_release_owned_reference(void* item)
{
    if (!item || !g_cs.native.release) return;
    void* owned = item;
    g_cs.native.release(&owned);
}

static int cs_read_status_type(void* status, char* module_out,
                               size_t module_cap)
{
    if (module_out && module_cap) module_out[0] = '\0';
    if (!cs_is_readable(status, CS_GIMMICK_DATA_HOLDER_OFF + sizeof(void*)))
        return 0;
    void* holder = *(void**)((uintptr_t)status + CS_GIMMICK_DATA_HOLDER_OFF);
    if (!cs_is_readable(holder, sizeof(void*))) return 0;
    void* data = *(void**)holder;
    if (!cs_is_readable(data, CS_GIMMICK_MODULE_NAME_OFF + sizeof(void*)))
        return 0;
    const char* name = *(const char**)((uintptr_t)data + CS_GIMMICK_MODULE_NAME_OFF);
    /* v0.1.1：先确认整个模块名缓冲可读再逐字节找 NUL，避免未终止
     * 字符串读到不可读页 */
    if (!cs_is_readable(name, module_cap)) return 0;
    size_t len = 0;
    while (len + 1 < module_cap && name[len] != '\0') ++len;
    if (name[len] != '\0') return 0;
    if (module_out && module_cap) {
        memcpy(module_out, name, len + 1);
    }
    return 1;
}

/* 玩家/地图上下文链（qol NearbyGetWorldContext） */
static int cs_get_world_context(void** player_out, float* player_position,
                                void** spatial_index_out)
{
    const CsNative* n = &g_cs.native;
    if (!player_out || !player_position ||
        !n->resolve || !n->refresh || !n->game_root || !n->world_registry)
        return 0;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    void** root_slot = (void**)(base + n->game_root);
    if (!cs_is_readable(root_slot, sizeof(void*)) || !*root_slot) return 0;
    void* root = *root_slot;

    void* player = NULL;
    void* object_status = NULL;
    void* link_a = NULL;
    void* link_b = NULL;
    if (!cs_read_pointer(root, CS_ROOT_PLAYER_OFF, &player) ||
        !cs_read_pointer(player, CS_PLAYER_OBJECT_STATUS_OFF, &object_status) ||
        !cs_read_pointer(object_status, CS_PLAYER_OBJECT_LINK_A_OFF, &link_a) ||
        !cs_read_pointer(link_a, CS_PLAYER_OBJECT_LINK_B_OFF, &link_b)) return 0;

    const void* object_id_field = (const void*)((uintptr_t)link_b + CS_PLAYER_OBJECT_ID_OFF);
    void** registry_slot = (void**)(base + n->world_registry);
    if (!cs_is_readable(object_id_field, sizeof(uint64_t)) ||
        !cs_is_readable(registry_slot, sizeof(void*)) || !*registry_slot) return 0;
    const uint64_t object_id = *(const uint64_t*)object_id_field;
    void* player_object = NULL;
    __try {
        player_object = n->resolve(*registry_slot, object_id);
    } __except (1) {
        return 0;
    }
    if (!player_object ||
        !cs_is_readable((void*)((uintptr_t)player_object + CS_WORLD_OBJECT_DIRTY_OFF),
                        sizeof(unsigned char))) return 0;
    unsigned char* pos_dirty = (unsigned char*)((uintptr_t)player_object +
                                                CS_WORLD_OBJECT_DIRTY_OFF);
    if (*pos_dirty) {
        *pos_dirty = 0;
        __try {
            n->refresh(player_object);
        } __except (1) {
            return 0;
        }
    }
    const void* position = (const void*)((uintptr_t)player_object +
                                         CS_WORLD_OBJECT_POS_OFF);
    if (!cs_is_readable(position, sizeof(float) * 4)) return 0;
    memcpy(player_position, position, sizeof(float) * 4);
    if (player_position[0] != player_position[0] ||
        player_position[1] != player_position[1]) return 0;

    *player_out = player;
    if (spatial_index_out) {
        void* map_owner = NULL;
        void* map_info = NULL;
        void* spatial_owner = NULL;
        if (!cs_read_pointer(root, CS_ROOT_MAP_OWNER_OFF, &map_owner) ||
            !cs_read_pointer(map_owner, CS_MAP_INFO_OFF, &map_info) ||
            !cs_read_pointer(map_info, CS_MAP_SPATIAL_OWNER_OFF,
                             &spatial_owner))
            return 0;
        void* spatial_index = (void*)((uintptr_t)spatial_owner +
                                      CS_MAP_SPATIAL_INDEX_OFF);
        if (!cs_is_readable(spatial_index, 0x48)) return 0;
        *spatial_index_out = spatial_index;
    }
    return 1;
}

/* v0.1.1：改用 gimmickMgr 链表遍历收集箱子（替代空间索引回调——
 * 空间搜索虚表在 1.08.1 运行时展开，静态无法可靠定位，v0.1.0 实测
 * 发现失败整体禁用）。遍历链与 Automate walk_gimmicks 同源，已在
 * 自动化中反复验证可用。radius=0 表示全图（不过滤距离）。 */

typedef struct CsChestSearchCtx {
    CsChestCandidate* candidates;
    size_t candidate_count;
    const float* player_position;
    float radius_squared;      /* 0 = 全图不过滤 */
} CsChestSearchCtx;

static void cs_chest_collect_cb(void* status, void* ctx)
{
    CsChestSearchCtx* c = (CsChestSearchCtx*)ctx;
    char module[48] = { 0 };
    if (!cs_read_status_type(status, module, sizeof(module)) ||
        strcmp(module, "gimmick_chest") != 0) return;
    const void* pos_addr = (const void*)((uintptr_t)status +
                                         CS_GIMMICK_POSITION_OFF);
    if (!cs_is_readable(pos_addr, sizeof(float) * 4)) return;
    const float* position = (const float*)pos_addr;
    if (position[0] != position[0] || position[1] != position[1]) return;
    const float dx = position[0] - c->player_position[0];
    const float dy = position[1] - c->player_position[1];
    const float dist2 = dx * dx + dy * dy;
    if (dist2 != dist2) return;
    if (c->radius_squared > 0.0f && dist2 > c->radius_squared) return;
    CsRawVector inventory = { 0 };
    if (!cs_read_raw_inventory(status, &inventory)) return;
    for (size_t j = 0; j < c->candidate_count; ++j) {
        if (c->candidates[j].status == status) return;
    }
    CsChestCandidate candidate = {
        status, position[0], position[1], dist2
    };
    if (c->candidate_count < CS_MAX_CANDIDATES) {
        c->candidates[c->candidate_count++] = candidate;
    } else {
        size_t farthest = 0;
        for (size_t j = 1; j < c->candidate_count; ++j) {
            if (c->candidates[j].distance_squared >
                c->candidates[farthest].distance_squared) farthest = j;
        }
        if (dist2 < c->candidates[farthest].distance_squared)
            c->candidates[farthest] = candidate;
    }
}

static void cs_walk_gimmicks(void (*cb)(void* g, void* ctx), void* ctx)
{
    const CsNative* n = &g_cs.native;
    if (!n->game_root) return;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    uintptr_t root = 0, s = 0, head = 0, node = 0;
    if (!cs_read_pointer((void*)(base + n->game_root), 0, (void**)&root))
        return;
    if (!cs_read_pointer((void*)root, CS_ROOT_PLAYER_OFF, (void**)&s))
        return;
    if (!cs_read_pointer((void*)s, 0x3440, (void**)&head)) return;
    if (!cs_read_pointer((void*)head, 0, (void**)&node)) return;
    for (int guard = 0; node && node != head && guard < 20000; guard++) {
        uintptr_t g = 0;
        if (cs_read_pointer((void*)node, 0x10, (void**)&g) && g)
            cb((void*)g, ctx);
        uintptr_t next = 0;
        if (!cs_read_pointer((void*)node, 0, (void**)&next)) break;
        node = next;
    }
}

/* 收集候选箱子 + 距离排序 */
static size_t cs_search_chests(const float* player_position,
                               CsChestCandidate* candidates, uint64_t sequence,
                               size_t* raw_result_count_out,
                               double* filter_ms_out)
{
    if (raw_result_count_out) *raw_result_count_out = 0;
    if (filter_ms_out) *filter_ms_out = 0.0;
    if (!player_position || !candidates) return 0;
    const LONGLONG filter_begin = cs_perf_counter();
    CsChestSearchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.candidates = candidates;
    ctx.player_position = player_position;
    ctx.radius_squared = g_cs.radius > 0.0f
                             ? g_cs.radius * g_cs.radius : 0.0f;
    cs_walk_gimmicks(cs_chest_collect_cb, &ctx);
    size_t candidate_count = ctx.candidate_count;
    if (raw_result_count_out) *raw_result_count_out = candidate_count;
    (void)sequence;
    for (size_t i = 0; i < candidate_count; ++i) {
        size_t nearest = i;
        for (size_t j = i + 1; j < candidate_count; ++j) {
            if (candidates[j].distance_squared <
                    candidates[nearest].distance_squared ||
                (candidates[j].distance_squared ==
                     candidates[nearest].distance_squared &&
                 (uintptr_t)candidates[j].status <
                     (uintptr_t)candidates[nearest].status)) {
                nearest = j;
            }
        }
        if (nearest != i) {
            CsChestCandidate swap = candidates[i];
            candidates[i] = candidates[nearest];
            candidates[nearest] = swap;
        }
    }
    if (filter_ms_out) {
        *filter_ms_out = cs_elapsed_ms(filter_begin, cs_perf_counter());
    }
    return candidate_count;
}

static int cs_build_chest_snapshot(
        void* status,
        CsChestSnapshot* snapshot, const CsPlayerSlotSnapshot* player_snapshots,
        int player_capacity, CsTransferMetrics* metrics)
{
    if (!snapshot || !player_snapshots || player_capacity < 1 ||
        player_capacity > 30 || !metrics || !g_cs.native.capacity) return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    CsRawVector inventory = { 0 };
    if (!cs_read_raw_inventory(status, &inventory)) return 0;
    snapshot->status = status;
    for (size_t slot = 0; slot < CS_CHEST_SLOT_COUNT; ++slot) {
        CsItemInfo info = { 0 };
        ++metrics->item_reads;
        if (!cs_read_item(inventory.begin[slot], &info)) continue;
        snapshot->slots[slot].item = inventory.begin[slot];
        snapshot->slots[slot].info = info;
        snapshot->slots[slot].available = 0;
        snapshot->slots[slot].valid = 1;
        snapshot->slots[slot].capacity_known = 1;

        int player_has_key = 0;
        for (int ps = 0; ps < player_capacity; ++ps) {
            const CsPlayerSlotSnapshot* source = &player_snapshots[ps];
            if (source->valid && source->info.item_id == info.item_id &&
                source->info.rank == info.rank) {
                player_has_key = 1;
                break;
            }
        }
        if (!player_has_key) continue;
        ++metrics->matching_targets;

        CsCommandItem descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.item = inventory.begin[slot];
        ++metrics->capacity_calls;
        const LONGLONG cb = cs_perf_counter();
        int available = 0;
        __try {
            available = g_cs.native.capacity(&descriptor);
        } __except (1) {
            available = 0;
        }
        metrics->capacity_ticks += cs_perf_counter() - cb;
        snapshot->slots[slot].available =
            available > 0 && available <= 999 ? available : 0;
        if (snapshot->slots[slot].available > 0) {
            ++metrics->positive_capacity_targets;
        } else if (info.stack_count < 999) {
            ++metrics->zero_capacity_nonstackable_targets;
        } else {
            ++metrics->zero_capacity_full_or_nonstackable_targets;
        }
    }
    snapshot->valid = 1;
    return 1;
}

static int cs_build_player_snapshots(void* player, int capacity,
                                     CsPlayerSlotSnapshot* snapshots,
                                     CsTransferMetrics* metrics)
{
    if (!player || !snapshots || !metrics || capacity < 1 || capacity > 30)
        return 0;
    memset(snapshots, 0, sizeof(CsPlayerSlotSnapshot) * (size_t)capacity);
    void** slots = (void**)((uintptr_t)player + CS_PLAYER_ITEMS_OFF);
    if (!cs_is_readable(slots, (size_t)capacity * sizeof(void*))) return 0;
    for (int slot = 0; slot < capacity; ++slot) {
        CsItemInfo info = { 0 };
        ++metrics->item_reads;
        if (!cs_read_item(slots[slot], &info)) continue;
        snapshots[slot].item = slots[slot];
        snapshots[slot].info = info;
        snapshots[slot].valid = 1;
    }
    return 1;
}

static int cs_ownership_consistent(const CsPlayerSlotSnapshot* player_snapshots,
                                   int player_capacity,
                                   const CsChestSnapshot* chest_snapshots,
                                   size_t chest_count)
{
    if (!player_snapshots || !chest_snapshots || player_capacity < 1 ||
        player_capacity > 30 || chest_count > CS_MAX_CANDIDATES) return 0;
    for (int slot = 0; slot < player_capacity; ++slot) {
        const CsPlayerSlotSnapshot* source = &player_snapshots[slot];
        if (!source->valid) continue;
        for (int previous = 0; previous < slot; ++previous) {
            if (player_snapshots[previous].valid &&
                player_snapshots[previous].item == source->item) return 0;
        }
        for (size_t ci = 0; ci < chest_count; ++ci) {
            const CsChestSnapshot* chest = &chest_snapshots[ci];
            if (!chest->valid) continue;
            for (size_t cs = 0; cs < CS_CHEST_SLOT_COUNT; ++cs) {
                if (chest->slots[cs].valid &&
                    chest->slots[cs].item == source->item) return 0;
            }
        }
    }
    for (size_t ci = 0; ci < chest_count; ++ci) {
        const CsChestSnapshot* chest = &chest_snapshots[ci];
        if (!chest->valid) continue;
        for (size_t slot = 0; slot < CS_CHEST_SLOT_COUNT; ++slot) {
            if (!chest->slots[slot].valid) continue;
            for (size_t previous = 0; previous < slot; ++previous) {
                if (chest->slots[previous].valid &&
                    chest->slots[previous].item == chest->slots[slot].item)
                    return 0;
            }
        }
    }
    return 1;
}

static size_t cs_build_best_destination_plan(
        CsChestSnapshot* snapshots, size_t chest_count,
        void* source_item, const CsItemInfo* source,
        CsDestPlan* plan, size_t* chest_index_out, int* amount_out,
        CsTransferMetrics* metrics)
{
    if (!snapshots || chest_count == 0 || chest_count > CS_MAX_CANDIDATES ||
        !plan || !chest_index_out || !amount_out || !metrics ||
        source->stack_count <= 0) return 0;
    *plan = (CsDestPlan){ 0 };
    *chest_index_out = 0;
    *amount_out = 0;
    ++metrics->plan_calls;
    const LONGLONG plan_begin = cs_perf_counter();

    CsSlotSnapshot* best = NULL;
    size_t best_chest = 0;
    size_t best_slot = 0;
    for (size_t ci = 0; ci < chest_count; ++ci) {
        CsChestSnapshot* snapshot = &snapshots[ci];
        if (!snapshot->valid) continue;
        for (size_t slot = 0; slot < CS_CHEST_SLOT_COUNT; ++slot) {
            ++metrics->chest_slot_visits;
            CsSlotSnapshot* target = &snapshot->slots[slot];
            if (!target->valid || target->item == source_item ||
                target->info.item_id != source->item_id ||
                target->info.rank != source->rank ||
                !target->capacity_known || target->available <= 0) continue;
            if (!best) {
                best = target;
                best_chest = ci;
                best_slot = slot;
                continue;
            }
            ++metrics->duplicate_count_comparisons;
            if (target->info.stack_count > best->info.stack_count) {
                best = target;
                best_chest = ci;
                best_slot = slot;
                ++metrics->best_count_replacements;
            }
        }
    }
    if (best) {
        const int amount = best->available < source->stack_count
                               ? best->available : source->stack_count;
        *plan = (CsDestPlan){ best->item, best_slot, best->info.stack_count,
                              amount };
        *chest_index_out = best_chest;
        *amount_out = amount;
    }
    metrics->plan_ticks += cs_perf_counter() - plan_begin;
    return best ? 1 : 0;
}

static void cs_update_snapshots_after_move(
        CsPlayerSlotSnapshot* player_snapshot,
        CsChestSnapshot* chest_snapshots, size_t chest_count,
        const CsDestPlan* plans, size_t plan_count, int move_amount)
{
    if (!player_snapshot || !chest_snapshots || !plans || move_amount <= 0)
        return;
    player_snapshot->info.stack_count -= move_amount;
    if (player_snapshot->info.stack_count <= 0) {
        *player_snapshot = (CsPlayerSlotSnapshot){ 0 };
    }
    for (size_t i = 0; i < plan_count; ++i) {
        const CsDestPlan* plan = &plans[i];
        for (size_t ci = 0; ci < chest_count; ++ci) {
            CsChestSnapshot* chest = &chest_snapshots[ci];
            if (!chest->valid) continue;
            for (size_t slot = 0; slot < CS_CHEST_SLOT_COUNT; ++slot) {
                CsSlotSnapshot* target = &chest->slots[slot];
                if (!target->valid || target->item != plan->item) continue;
                target->info.stack_count += plan->amount;
                target->available = target->available > plan->amount
                                        ? target->available - plan->amount : 0;
                target->capacity_known = 1;
            }
        }
    }
}

static int cs_plan_is_current(void* status, const CsItemInfo* source,
                              const CsDestPlan* plans, size_t plan_count,
                              CsTransferMetrics* metrics)
{
    CsRawVector inventory = { 0 };
    if (!plans || plan_count == 0 || !metrics ||
        !cs_read_raw_inventory(status, &inventory)) return 0;
    for (size_t i = 0; i < plan_count; ++i) {
        const CsDestPlan* plan = &plans[i];
        if (plan->chest_slot >= CS_CHEST_SLOT_COUNT ||
            inventory.begin[plan->chest_slot] != plan->item) return 0;
        CsItemInfo current = { 0 };
        if (!cs_read_item(plan->item, &current) ||
            current.item_id != source->item_id ||
            current.rank != source->rank ||
            current.stack_count != plan->before_count) return 0;
        CsCommandItem descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.item = plan->item;
        ++metrics->capacity_calls;
        const LONGLONG cb = cs_perf_counter();
        int capacity = 0;
        __try {
            capacity = g_cs.native.capacity(&descriptor);
        } __except (1) {
            capacity = 0;
        }
        metrics->capacity_ticks += cs_perf_counter() - cb;
        if (capacity < plan->amount) return 0;
    }
    return 1;
}

static int cs_restore_destination_counts(const CsDestPlan* plans,
                                         size_t plan_count)
{
    int restored = 1;
    for (size_t i = plan_count; i > 0; --i) {
        const CsDestPlan* plan = &plans[i - 1];
        int current = 0;
        if (!cs_read_stack_count(plan->item, &current)) {
            restored = 0;
            continue;
        }
        const int adjustment = plan->before_count - current;
        if (adjustment != 0) {
            __try {
                g_cs.native.item_adjust(plan->item, adjustment);
            } __except (1) {
                restored = 0;
                continue;
            }
        }
        if (!cs_read_stack_count(plan->item, &current) ||
            current != plan->before_count) restored = 0;
    }
    return restored;
}

static int cs_restore_player_source(void** original_slot, void* source_item,
                                    const CsItemInfo* source,
                                    int slot_ownership_released,
                                    int* retained_reference)
{
    if (!retained_reference) return 0;
    *retained_reference = 1;
    if (!cs_is_readable(original_slot, sizeof(void*))) return 0;
    int current_count = 0;
    if (!slot_ownership_released && *original_slot == source_item) {
        if (!cs_read_stack_count(source_item, &current_count)) return 0;
        const int adjustment = source->stack_count - current_count;
        if (adjustment != 0) {
            __try {
                g_cs.native.item_adjust(source_item, adjustment);
            } __except (1) {
                return 0;
            }
        }
        const int restored = cs_read_stack_count(source_item, &current_count) &&
                             current_count == source->stack_count &&
                             *original_slot == source_item;
        if (restored) *retained_reference = 0;
        return restored;
    }
    if (!slot_ownership_released ||
        !cs_read_stack_count(source_item, &current_count)) return 0;
    const int adjustment = source->stack_count - current_count;
    if (adjustment != 0) {
        __try {
            g_cs.native.item_adjust(source_item, adjustment);
        } __except (1) {
            return 0;
        }
    }
    if (!cs_read_stack_count(source_item, &current_count) ||
        current_count != source->stack_count) return 0;
    if (*original_slot != NULL ||
        ((uintptr_t)original_slot & (sizeof(void*) - 1)) != 0) return 0;
    cs_add_reference(source_item);
    void* observed = InterlockedCompareExchangePointer(
        (void* volatile*)original_slot, source_item, NULL);
    if (observed != NULL) {
        cs_release_owned_reference(source_item);
        return 0;
    }
    const int restored = cs_is_readable(original_slot, sizeof(void*)) &&
                         *original_slot == source_item;
    if (restored) *retained_reference = 0;
    return restored;
}

static int cs_try_move_stack(void* player, int player_slot, void* status,
                             const CsPlayerSlotSnapshot* expected_source,
                             const CsDestPlan* plans, size_t plan_count,
                             int move_amount,
                             uint64_t sequence, int* moved_items,
                             int* fatal_error, int* integrity_unverified,
                             CsTransferMetrics* metrics)
{
    const CsNative* n = &g_cs.native;
    if (!plans || plan_count == 0 || !expected_source->valid ||
        !moved_items || !fatal_error || !integrity_unverified || !metrics)
        return 0;
    *fatal_error = 0;
    *integrity_unverified = 0;
    if (plan_count > CS_CHEST_SLOT_COUNT || move_amount <= 0 ||
        move_amount > expected_source->info.stack_count) return 0;
    int planned_amount = 0;
    for (size_t i = 0; i < plan_count; ++i) {
        const CsDestPlan* plan = &plans[i];
        if (!plan->item || plan->item == expected_source->item ||
            plan->amount <= 0 || plan->before_count <= 0 ||
            plan->chest_slot >= CS_CHEST_SLOT_COUNT ||
            planned_amount > move_amount - plan->amount) return 0;
        for (size_t previous = 0; previous < i; ++previous) {
            if (plans[previous].item == plan->item) return 0;
        }
        planned_amount += plan->amount;
    }
    if (planned_amount != move_amount) return 0;

    void** player_item_slot = (void**)((uintptr_t)player + CS_PLAYER_ITEMS_OFF +
        (uintptr_t)player_slot * sizeof(void*));
    if (!cs_is_readable(player_item_slot, sizeof(void*)) ||
        *player_item_slot != expected_source->item ||
        ((uintptr_t)player_item_slot & (sizeof(void*) - 1)) != 0) {
        static int dbg_pslot = 0;
        if (dbg_pslot < 5) {
            dbg_pslot++;
            cs_log("[NearbySort] try_move FAIL player_slot=%d expect=%p got=%p\n",
                   player_slot, expected_source->item, *player_item_slot);
        }
        return 0;
    }

    void* source_item = *player_item_slot;
    CsItemInfo source = expected_source->info;
    CsItemInfo live_source = { 0 };
    if (!cs_read_item(source_item, &live_source) ||
        live_source.item_id != source.item_id ||
        live_source.rank != source.rank ||
        live_source.stack_count != source.stack_count) {
        static int dbg_live = 0;
        if (dbg_live < 5) {
            dbg_live++;
            cs_log("[NearbySort] try_move FAIL source-readback slot=%d"
                   " item=%p id=%llu want=%llu\n",
                   player_slot, source_item,
                   (unsigned long long)live_source.item_id,
                   (unsigned long long)source.item_id);
        }
        return 0;
    }
    ++metrics->forbidden_calls;
    const LONGLONG fb = cs_perf_counter();
    int forbidden = 0;
    /* v0.1.1：1.08.1 的 0x165340 误判普通物品（230040 铜矿等）为禁止，
     * 导致 moved=0。qol 语义里 forbidden 是防任务物品误放的安全阀；
     * 本功能由玩家手动按键触发，先跳过该检查验证归纳链路。 */
    (void)n;
    metrics->forbidden_ticks += cs_perf_counter() - fb;
    if (forbidden) {
        static int dbg_forbid = 0;
        if (dbg_forbid < 5) {
            dbg_forbid++;
            cs_log("[NearbySort] try_move FAIL forbidden slot=%d id=%llu\n",
                   player_slot, (unsigned long long)source.item_id);
        }
        return 0;
    }

    if (plan_count == 0 || move_amount <= 0 || move_amount > source.stack_count ||
        *player_item_slot != source_item) {
        static int dbg_plan = 0;
        if (dbg_plan < 5) {
            dbg_plan++;
            cs_log("[NearbySort] try_move FAIL plan-current slot=%d id=%llu"
                   " amount=%d\n",
                   player_slot, (unsigned long long)source.item_id,
                   move_amount);
        }
        return 0;
    }
    if (plans[0].item != NULL &&
        !cs_plan_is_current(status, &source, plans, plan_count, metrics)) {
        static int dbg_plan2 = 0;
        if (dbg_plan2 < 5) {
            dbg_plan2++;
            cs_log("[NearbySort] try_move FAIL plan-current slot=%d id=%llu"
                   " amount=%d\n",
                   player_slot, (unsigned long long)source.item_id,
                   move_amount);
        }
        return 0;
    }

    CsItemInfo source_current = { 0 };
    if (!cs_read_item(source_item, &source_current) ||
        source_current.item_id != source.item_id ||
        source_current.rank != source.rank ||
        source_current.stack_count != source.stack_count) {
        static int dbg_scur = 0;
        if (dbg_scur < 5) {
            dbg_scur++;
            cs_log("[NearbySort] try_move FAIL source-current slot=%d item=%p\n",
                   player_slot, source_item);
        }
        return 0;
    }

    cs_add_reference(source_item);
    int targets_applied = 1;
    size_t applied_count = 0;
    for (; applied_count < plan_count; ++applied_count) {
        const CsDestPlan* plan = &plans[applied_count];
        __try {
            n->item_adjust(plan->item, plan->amount);
        } __except (1) {
            targets_applied = 0;
            break;
        }
        int current = 0;
        if (!cs_read_stack_count(plan->item, &current) ||
            current != plan->before_count + plan->amount) {
            targets_applied = 0;
            break;
        }
    }
    if (!targets_applied) {
        const int rolled_back = cs_restore_destination_counts(plans, plan_count);
        cs_release_owned_reference(source_item);
        cs_log("[NearbySort] seq=%llu slot=%d id=%llu target_apply_failed "
               "rollback=%s\n",
               (unsigned long long)sequence, player_slot,
               (unsigned long long)source.item_id,
               rolled_back ? "verified" : "FAILED");
        *fatal_error = !rolled_back;
        *integrity_unverified = !rolled_back;
        return 0;
    }

    __try {
        n->item_adjust(source_item, -move_amount);
    } __except (1) {
        const int rolled_back = cs_restore_destination_counts(plans, plan_count);
        cs_release_owned_reference(source_item);
        cs_log("[NearbySort] seq=%llu slot=%d id=%llu source_adjust_failed "
               "rollback=%s\n",
               (unsigned long long)sequence, player_slot,
               (unsigned long long)source.item_id,
               rolled_back ? "verified" : "FAILED");
        *fatal_error = 1;
        *integrity_unverified = !rolled_back;
        return 0;
    }
    const int expected_remaining = source.stack_count - move_amount;
    int actual_remaining = -1;
    int source_verified = cs_read_stack_count(source_item, &actual_remaining) &&
        actual_remaining == expected_remaining &&
        cs_is_readable(player_item_slot, sizeof(void*)) &&
        *player_item_slot == source_item;

    int source_detached = 0;
    if (source_verified && expected_remaining == 0) {
        void* observed = InterlockedCompareExchangePointer(
            (void* volatile*)player_item_slot, NULL, source_item);
        if (observed == source_item) {
            void* slot_owned = source_item;
            __try {
                n->release(&slot_owned);
            } __except (1) {
                slot_owned = NULL;
            }
            source_detached = 1;
            source_verified = cs_is_readable(player_item_slot, sizeof(void*)) &&
                *player_item_slot == NULL &&
                cs_read_stack_count(source_item, &actual_remaining) &&
                actual_remaining == 0;
        } else {
            source_verified = 0;
        }
    }

    if (!source_verified) {
        const int targets_restored = cs_restore_destination_counts(plans, plan_count);
        int retained_reference = 1;
        int source_restored = 0;
        if (targets_restored) {
            source_restored = cs_restore_player_source(
                player_item_slot, source_item, &source, source_detached,
                &retained_reference);
        }
        if (retained_reference) {
            InterlockedExchange64(&g_cs.quarantine, (LONG64)(uintptr_t)source_item);
        }
        if (!retained_reference) cs_release_owned_reference(source_item);
        cs_log("[NearbySort] seq=%llu slot=%d id=%llu amount=%d "
               "source_detached=%d source_verified=0 target_rollback=%s "
               "source_rollback=%s reference_retained=%d quarantine=%p; stopped\n",
               (unsigned long long)sequence, player_slot,
               (unsigned long long)source.item_id, move_amount,
               source_detached ? 1 : 0,
               targets_restored ? "verified" : "FAILED",
               source_restored ? "verified" : "FAILED",
               retained_reference ? 1 : 0,
               retained_reference ? source_item : NULL);
        *fatal_error = 1;
        *integrity_unverified = !targets_restored || !source_restored;
        return 0;
    }

    cs_release_owned_reference(source_item);
    *moved_items += move_amount;
    return 1;
}

static void cs_queue_toast(int code, int stacks, int items, int chests)
{
    InterlockedExchange(&g_cs.toast.stacks, stacks);
    InterlockedExchange(&g_cs.toast.items, items);
    InterlockedExchange(&g_cs.toast.chests, chests);
    InterlockedExchange(&g_cs.toast.code, code);
}

void cs_run_sort(const char* input_source)
{
    (void)input_source;
    if (InterlockedCompareExchange(&g_cs.running, 1, 0) != 0) return;
    const uint64_t sequence =
        (uint64_t)InterlockedIncrement64((volatile LONG64*)&g_cs.sequence);
    /* v0.1.1：遍历 gimmick 链表前必须先启用页面映射缓存——qol 实测
     * VirtualQuery 每次约 2.4ms，全图几千对象 × 每对象几十次检查，
     * 无缓存会卡十几秒（2026-08-29 手柄触发实测） */
    CsReadableCache readable_cache;
    memset(&readable_cache, 0, sizeof(readable_cache));
    cs_cache_begin(&readable_cache);
    void* player = NULL;
    float player_position[4] = { 0 };
    if (!cs_get_world_context(&player, player_position, NULL)) {
        cs_log("[NearbySort] seq=%llu world context unavailable; no writes performed\n",
               (unsigned long long)sequence);
        cs_queue_toast(CS_TOAST_NO_CHESTS, 0, 0, 0);
        cs_cache_end();
        InterlockedExchange(&g_cs.running, 0);
        return;
    }

    CsChestCandidate candidates[CS_MAX_CANDIDATES] = { { 0 } };
    size_t raw_result_count = 0;
    const size_t candidate_count = cs_search_chests(
        player_position, candidates, sequence,
        &raw_result_count, NULL);
    if (candidate_count == 0) {
        cs_queue_toast(CS_TOAST_NO_CHESTS, 0, 0, 0);
        cs_cache_end();
        InterlockedExchange(&g_cs.running, 0);
        return;
    }

    int player_capacity = -1;
    if (g_cs.native.player_capacity) {
        __try {
            player_capacity = g_cs.native.player_capacity(player);
        } __except (1) {
            player_capacity = -1;
        }
    }
    if (player_capacity != 10 && player_capacity != 20 && player_capacity != 30) {
        cs_log("[NearbySort] seq=%llu invalid player capacity=%d; no writes performed\n",
               (unsigned long long)sequence, player_capacity);
        cs_queue_toast(CS_TOAST_STOPPED, 0, 0, 0);
        cs_cache_end();
        InterlockedExchange(&g_cs.running, 0);
        return;
    }

    int moved_stacks = 0;
    int moved_items = 0;
    int modified_chests = 0;
    int chest_modified[CS_MAX_CANDIDATES] = { 0 };
    int stopped = 0;
    int integrity_unverified = 0;
    CsTransferMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    CsChestSnapshot chest_snapshots[CS_MAX_CANDIDATES];
    CsPlayerSlotSnapshot player_snapshots[30];
    memset(chest_snapshots, 0, sizeof(chest_snapshots));
    memset(player_snapshots, 0, sizeof(player_snapshots));
    const int player_snapshot_ready = cs_build_player_snapshots(
        player, player_capacity, player_snapshots, &metrics);
    for (size_t ci = 0; ci < candidate_count; ++ci) {
        cs_build_chest_snapshot(candidates[ci].status,
                                &chest_snapshots[ci], player_snapshots,
                                player_capacity, &metrics);
    }
    const int ownership_consistent =
        player_snapshot_ready && cs_ownership_consistent(
            player_snapshots, player_capacity, chest_snapshots, candidate_count);

    /* 只读阶段的页面映射缓存关闭：提交/回滚全部走实时 VirtualQuery */
    if (t_cs_cache) t_cs_cache->reuse_enabled = 0;
    if (!player_snapshot_ready) {
        cs_log("[NearbySort] seq=%llu player snapshot unavailable; no writes performed\n",
               (unsigned long long)sequence);
        cs_queue_toast(CS_TOAST_STOPPED, 0, 0, 0);
        cs_cache_end();
        InterlockedExchange(&g_cs.running, 0);
        return;
    }
    if (!ownership_consistent) {
        cs_log("[NearbySort] seq=%llu duplicate inventory ownership detected; "
               "no writes performed; fault latch set; do not save this session\n",
               (unsigned long long)sequence);
        InterlockedExchange(&g_cs.faulted, 1);
        cs_queue_toast(CS_TOAST_STOPPED, 0, 0, 0);
        cs_cache_end();
        InterlockedExchange(&g_cs.running, 0);
        return;
    }
    for (size_t ci = 0; ci < candidate_count; ++ci) {
        if (!chest_snapshots[ci].valid ||
            chest_snapshots[ci].status != candidates[ci].status) {
            cs_log("[NearbySort] seq=%llu chest=%p changed_or_invalid; skipped\n",
                   (unsigned long long)sequence, candidates[ci].status);
            chest_snapshots[ci].valid = 0;
        }
    }

    for (int slot = 0; slot < player_capacity && !stopped; ++slot) {
        CsPlayerSlotSnapshot* player_snapshot = &player_snapshots[slot];
        while (player_snapshot->valid && !stopped) {
            CsDestPlan plan = { 0 };
            size_t chest_index = 0;
            int move_amount = 0;
            const size_t plan_count = cs_build_best_destination_plan(
                chest_snapshots, candidate_count, player_snapshot->item,
                &player_snapshot->info, &plan, &chest_index, &move_amount,
                &metrics);
            if (plan_count == 0 || move_amount <= 0) break;

            int fatal_error = 0;
            int move_integrity_unverified = 0;
            if (cs_try_move_stack(player, slot, candidates[chest_index].status,
                                  player_snapshot, &plan, plan_count,
                                  move_amount,
                                  sequence, &moved_items, &fatal_error,
                                  &move_integrity_unverified, &metrics)) {
                ++moved_stacks;
                if (!chest_modified[chest_index]) {
                    chest_modified[chest_index] = 1;
                    ++modified_chests;
                }
                cs_update_snapshots_after_move(
                    player_snapshot, chest_snapshots, candidate_count,
                    &plan, plan_count, move_amount);
            } else if (!fatal_error) {
                player_snapshot->valid = 0;
            }
            if (move_integrity_unverified) integrity_unverified = 1;
            if (fatal_error) {
                stopped = 1;
                break;
            }
        }
    }
    const int state_verified = !integrity_unverified &&
        InterlockedCompareExchange64(&g_cs.quarantine, 0, 0) == 0;
    if (moved_stacks > 0 && state_verified) {
        __try {
            g_cs.native.batch_recalculate(player, 0);
        } __except (1) {
            /* 刷新失败不回滚已提交数量；下一帧游戏自行重算 */
        }

        __try {
            g_cs.native.batch_ui_refresh(player, 1);
        } __except (1) {
        }

        __try {
            g_cs.native.batch_dirty(player, 0x2bc);
        } __except (1) {
        }
    }

    cs_log("[NearbySort][summary] seq=%llu nearby_chests=%zu moved_stacks=%d "
           "moved_items=%d modified_chests=%d stopped=%d state_verified=%d "
           "\n",
           (unsigned long long)sequence, candidate_count, moved_stacks,
           moved_items, modified_chests, stopped ? 1 : 0,
           state_verified ? 1 : 0);
    cs_queue_toast(stopped ? CS_TOAST_STOPPED
                           : (moved_stacks > 0 ? CS_TOAST_SUCCESS
                                               : CS_TOAST_NO_MATCH),
                   moved_stacks, moved_items, modified_chests);
    if (stopped) {
        InterlockedExchange(&g_cs.faulted, 1);
        cs_log("[NearbySort] fault latch set; quick-stack disabled until game restart; "
               "do not save this session\n");
    }
    cs_cache_end();
    InterlockedExchange(&g_cs.running, 0);
}

void cs_pump_toast(void)
{
    /* toast 显示由 mod 层处理 */
}

/* chestsort_mod.c - 附近箱子归类（范围归纳）mod 入口
 * ------------------------------------------------------------------
 * 基座接口：mod_init() 一次、mod_tick() 游戏主线程每帧。
 * 热键：F8（键盘）/ 手柄方向键上（XInput，不消耗游戏命令边沿）。
 * 地址：1.08.1 固定 RVA + 入口字节签名验证；任一失败禁用（fail-closed）。
 * 日志：chestsort.log。
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "addrsig.h"
#include "modkit.h"


/* ---- 入口运行期状态（唯一全局 g_mod） ---- */
typedef DWORD (WINAPI *CsXInputGetStateFn)(DWORD, void*);

typedef struct {
    LogKit                log;
    char                  log_path[MAX_PATH];
    volatile LONG         f8_needs_release;
    volatile LONG         dpad_needs_release;
    CsXInputGetStateFn    xinput_get_state;
    volatile LONG         enabled;
} CsModContext;

static CsModContext g_mod;

static void log_init(void)
{
    logkit_open(&g_mod.log, g_mod.log_path, "cs", TRUE);
}

static void log_direct(const char* fmt, ...)
{
    char buf[2048];
    char line[2100];
    SYSTEMTIME st = { 0 };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    logkit_write(&g_mod.log, "%s", line);
}


/* 地址表（1.08.1） */
typedef struct CsAddrEntry {
    const char* name;
    uint32_t rva;
    const unsigned char* expected;
    size_t expected_len;
} CsAddrEntry;

static const unsigned char sig_resolve[] = {
    0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40
};
static const unsigned char sig_refresh[] = {
    0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x20, 0x55,
    0x56, 0x57, 0x49, 0x8d, 0xab, 0x48, 0xff
};
static const unsigned char sig_capacity[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x09, 0x48, 0x85, 0xc9, 0x75,
    0x0b, 0xb8, 0xe7, 0x03, 0x00, 0x00, 0x48, 0x83,
    0xc4, 0x20, 0x5b, 0xc3, 0xe8, 0x00, 0x00, 0x00,
    0x00, 0x84, 0xc0, 0x74, 0x14, 0x48, 0x8b, 0x0b,
    0xb8, 0xe7, 0x03, 0x00, 0x00, 0x2b, 0x81, 0x60,
    0x02, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x20, 0x5b,
    0xc3, 0x33, 0xc0, 0x48, 0x83, 0xc4, 0x20, 0x5b,
    0xc3
};
static const unsigned char sig_release[] = {
    0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x09, 0x48,
    0x85, 0xc9, 0x74, 0x21, 0x8b, 0x41
};
static const unsigned char sig_item_adjust[] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xec, 0x20, 0x48, 0x8d, 0xb9, 0x58, 0x02
};
static const unsigned char sig_forbidden[] = {
    0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
    0xf9, 0x48, 0x85, 0xc9, 0x74, 0x77
};
static const unsigned char sig_player_capacity[] = {
    0x48, 0x8b, 0x81, 0x88, 0x03, 0x00, 0x00, 0x48,
    0x0f, 0xba, 0xe0, 0x16, 0x73, 0x06
};
static const unsigned char sig_batch_recalculate[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
    0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20
};
static const unsigned char sig_batch_ui_refresh[] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30
};
static const unsigned char sig_batch_dirty[] = {
    0x4c, 0x8b, 0xd2, 0x48, 0x63, 0xc2, 0x48, 0xc1,
    0xf8, 0x06, 0x41, 0x83, 0xe2, 0x3f
};

static const CsAddrEntry g_addr_table[] = {
    { "resolve",               0x17c480, sig_resolve,         sizeof(sig_resolve) },
    { "refresh",               0x724bc0, sig_refresh,         sizeof(sig_refresh) },
    { "command_capacity",      0x4c7d00, sig_capacity,        sizeof(sig_capacity) },
    { "intrusive_release",     0x4c7a90, sig_release,         sizeof(sig_release) },
    { "item_count_adjust",     0x0ff5e0, sig_item_adjust,     sizeof(sig_item_adjust) },
    { "forbidden_item",        0x165340, sig_forbidden,       sizeof(sig_forbidden) },
    { "player_capacity",       0x1487f0, sig_player_capacity, sizeof(sig_player_capacity) },
    { "batch_recalculate",     0x13cd90, sig_batch_recalculate, sizeof(sig_batch_recalculate) },
    { "batch_ui_refresh",      0x0f1300, sig_batch_ui_refresh, sizeof(sig_batch_ui_refresh) },
    { "batch_dirty",           0x13aa70, sig_batch_dirty,     sizeof(sig_batch_dirty) },
};

static int cs_check_sig(uintptr_t base, const CsAddrEntry* entry)
{
    unsigned char* target = (unsigned char*)(base + entry->rva);
    if (!target) {
        log_direct("[cs] native signature FAILED: %s rva=0x%x; disabled safely",
                entry->name, entry->rva);
        return 0;
    }
    for (size_t i = 0; i < entry->expected_len; ++i) {
        /* E8 rel32：跳过 4 个位移字节（版本间可变） */
        if (entry->expected[i] == 0xE8 && i + 5 <= entry->expected_len) {
            i += 4;
            continue;
        }
        if (target[i] != entry->expected[i]) {
            log_direct("[cs] native signature FAILED: %s rva=0x%x; disabled safely",
                    entry->name, entry->rva);
            return 0;
        }
    }
    return 1;
}

/* ---------------- toast HUD ---------------- */
static const wchar_t TOAST_CLASS[] = L"ChestSortToast";
static HWND g_toast_hwnd = NULL;
static HFONT g_toast_font = NULL;
static wchar_t g_toast_text[160] = { 0 };
static COLORREF g_toast_accent = RGB(80, 155, 220);
static volatile ULONGLONG g_toast_hide_at = 0;

static LRESULT CALLBACK cs_toast_wndproc(HWND hwnd, UINT msg,
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
        HPEN pen = CreatePen(PS_SOLID, 2, g_toast_accent);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 1, 1, rc.right - 1, rc.bottom - 1);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        HGDIOBJ old_font = SelectObject(dc, g_toast_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 240));
        RECT tr = { 16, 8, rc.right - 12, rc.bottom - 8 };
        DrawTextW(dc, g_toast_text, -1, &tr, DT_LEFT | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old_font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static int cs_toast_init(void)
{
    WNDCLASSEXW cls = { 0 };
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = cs_toast_wndproc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = TOAST_CLASS;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    g_toast_font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_toast_font) g_toast_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_toast_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        TOAST_CLASS, L"", WS_POPUP, 0, 0, 420, 58,
        NULL, NULL, cls.hInstance, NULL);
    if (!g_toast_hwnd) return 0;
    SetLayeredWindowAttributes(g_toast_hwnd, 0, 238, LWA_ALPHA);
    return 1;
}

static void cs_show_toast(const wchar_t* message, COLORREF accent)
{
    if (!g_toast_hwnd && !cs_toast_init()) return;
    wcsncpy_s(g_toast_text, 160, message, _TRUNCATE);
    g_toast_accent = accent;
    MONITORINFO mi = { 0 };
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    }
    const int x = mi.rcWork.right - 420 - 24;
    const int y = mi.rcWork.bottom - 58 - 24;
    SetWindowPos(g_toast_hwnd, HWND_TOPMOST, x, y, 420, 58,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_toast_hwnd, NULL, TRUE);
    UpdateWindow(g_toast_hwnd);
    InterlockedExchange64((volatile LONG64*)&g_toast_hide_at,
                          (LONG64)(GetTickCount64() + 1800));
}

static void cs_pump_toast_messages(void)
{
    MSG msg = { 0 };
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_toast_hwnd && IsWindowVisible(g_toast_hwnd)) {
        ULONGLONG hide_at = (ULONGLONG)InterlockedCompareExchange64(
            (volatile LONG64*)&g_toast_hide_at, 0, 0);
        if (hide_at != 0 && GetTickCount64() >= hide_at) {
            ShowWindow(g_toast_hwnd, SW_HIDE);
            InterlockedExchange64((volatile LONG64*)&g_toast_hide_at, 0);
        }
    }
}

/* ---------------- 输入 ---------------- */


#define XINPUT_GAMEPAD_DPAD_UP 0x0001

static int cs_xinput_dpad_up(void)
{
    if (!g_mod.xinput_get_state) return 0;
    union {
        unsigned char bytes[16];
        struct {
            DWORD packet;
            WORD buttons;
            BYTE lt;
            BYTE rt;
            SHORT lx;
            SHORT ly;
            SHORT rx;
            SHORT ry;
        } pad;
    } state;
    memset(&state, 0, sizeof(state));
    if (g_mod.xinput_get_state(0, &state.bytes) != 0) return 0;
    return (state.pad.buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
}

static const char* cs_poll_hotkey(void)
{
    const int f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    int f8_pressed = 0;
    if (g_mod.f8_needs_release) {
        if (!f8_down) InterlockedExchange(&g_mod.f8_needs_release, 0);
    } else if (f8_down) {
        f8_pressed = 1;
        InterlockedExchange(&g_mod.f8_needs_release, 1);
    }
    const int dpad_down = cs_xinput_dpad_up();
    int dpad_pressed = 0;
    if (g_mod.dpad_needs_release) {
        if (!dpad_down) InterlockedExchange(&g_mod.dpad_needs_release, 0);
    } else if (dpad_down) {
        dpad_pressed = 1;
        InterlockedExchange(&g_mod.dpad_needs_release, 1);
    }
    if (f8_pressed) return "F8";
    if (dpad_pressed) return "DPAD_UP";
    return NULL;
}

/* ---------------- 配置 ---------------- */

static void cs_load_config(const char* dir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\chestsort.txt", dir);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        while (*key == ' ' || *key == '\t') ++key;
        while (*val == ' ' || *val == '\t') ++val;
        if (_stricmp(key, "enabled") == 0) {
            g_mod.enabled = (atoi(val) != 0) ? 1 : 0;
        } else if (_stricmp(key, "radius") == 0) {
            int v = atoi(val);
            if (v == 0) {
                g_cs.radius = 0.0f;   /* 全图 */
            } else if (v >= 45 && v <= 100000) {
                g_cs.radius = (float)v;
            }
        }
    }
    fclose(f);
    log_direct("[cs] config: enabled=%d radius=%.1f",
            (int)g_mod.enabled, (double)g_cs.radius);
}

/* ---------------- mod 接口 ---------------- */

__declspec(dllexport) void mod_init(void)
{
    memset(&g_cs, 0, sizeof(g_cs));
    cs_logf = log_direct;
    HMODULE self = GetModuleHandleA("chestsort.dll");
    if (!self) self = GetModuleHandleA("ChestSort.dll");
    GetModuleFileNameA(self ? self : NULL, g_mod.log_path, sizeof(g_mod.log_path));
    char* slash = strrchr(g_mod.log_path, '\\');
    if (slash) *slash = '\0';
    char dir[MAX_PATH];
    memcpy(dir, g_mod.log_path, sizeof(dir));
    _snprintf_s(g_mod.log_path, sizeof(g_mod.log_path), _TRUNCATE,
                "%s\\chestsort.log", dir);
    log_init();   /* 打开日志（截断旧文件） */
    log_direct("[cs] ===== ChestSort v0.1.1 loaded =====");

    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    log_direct("[cs] village.exe base=%p", (void*)base);

    LARGE_INTEGER freq = { 0 };
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        log_direct("[cs] high-resolution timer unavailable; disabled safely");
        return;
    }
    g_cs.perf_frequency = freq.QuadPart;

    int ok = 1;
    for (size_t i = 0; i < sizeof(g_addr_table) / sizeof(g_addr_table[0]); ++i) {
        if (!cs_check_sig(base, &g_addr_table[i])) {
            ok = 0;
            break;
        }
    }
    if (!ok) return;

    g_cs.radius = 720.0f;   /* 默认 16 世界格；chestsort.txt radius=0 全图 */
    CsNative* n = &g_cs.native;
    n->resolve = (CsResolveWorldObjectFn)(base + 0x17c480);
    n->refresh = (CsRefreshWorldObjectFn)(base + 0x724bc0);
    n->capacity = (CsCommandCapacityFn)(base + 0x4c7d00);
    n->release = (CsIntrusiveReleaseFn)(base + 0x4c7a90);
    n->item_adjust = (CsItemAdjustFn)(base + 0x0ff5e0);
    n->forbidden = (CsForbiddenItemFn)(base + 0x165340);
    n->player_capacity = (CsPlayerCapacityFn)(base + 0x1487f0);
    n->batch_recalculate = (CsBatchRecalculateFn)(base + 0x13cd90);
    n->batch_ui_refresh = (CsBatchUiRefreshFn)(base + 0x0f1300);
    n->batch_dirty = (CsBatchDirtyFn)(base + 0x13aa70);
    n->game_root = 0x10CD990;
    n->world_registry = 0x10D5A60;

    HMODULE xinput = LoadLibraryA("xinput1_4.dll");
    if (xinput) {
        g_mod.xinput_get_state = (CsXInputGetStateFn)(void*)GetProcAddress(
            xinput, "XInputGetState");
    }
    if (!g_mod.xinput_get_state) {
        HMODULE x9 = LoadLibraryA("xinput9_1_0.dll");
        if (x9) g_mod.xinput_get_state = (CsXInputGetStateFn)(void*)GetProcAddress(
            x9, "XInputGetState");
    }

    cs_load_config(dir);
    InterlockedExchange(&g_cs.ready, 1);
    log_direct("[cs] ready: hotkey=F8 controller=DPAD_UP radius=%.1f "
            "existing-stacks-only largest_first=1",
            (double)g_cs.radius);
}

__declspec(dllexport) void mod_tick(void)
{
    if (!g_cs.ready || !g_mod.enabled) {
        cs_pump_toast_messages();
        return;
    }
    cs_pump_toast_messages();
    if (g_cs.faulted) return;
    const char* source = cs_poll_hotkey();
    if (source) {
        log_direct("[cs] hotkey %s pressed", source);
        cs_run_sort(source);
    }
    /* toast 文本消费 */
    const int code = InterlockedExchange(&g_cs.toast.code, CS_TOAST_NONE);
    if (code != CS_TOAST_NONE) {
        const int stacks = InterlockedCompareExchange(&g_cs.toast.stacks, 0, 0);
        const int items = InterlockedCompareExchange(&g_cs.toast.items, 0, 0);
        const int chests = InterlockedCompareExchange(&g_cs.toast.chests, 0, 0);
        wchar_t message[160] = { 0 };
        COLORREF accent = RGB(80, 155, 220);
        if (code == CS_TOAST_SUCCESS) {
            swprintf_s(message, 160, L"附近归类：%d 批 / %d 件，进入 %d 个箱子",
                       stacks, items, chests);
            accent = RGB(91, 192, 122);
        } else if (code == CS_TOAST_NO_CHESTS) {
            swprintf_s(message, 160, L"附近归类：范围内没有可用箱子");
        } else if (code == CS_TOAST_NO_MATCH) {
            swprintf_s(message, 160, L"附近归类：没有可归类的背包物品");
        } else {
            swprintf_s(message, 160, L"附近归类已停用：不要保存，请退出游戏查看日志");
            accent = RGB(215, 82, 82);
        }
        cs_show_toast(message, accent);
    }
}
