/* modkit.c - 见 modkit.h */
#include "modkit.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==================================================================
 * 内存：读取 / 写入 / 近距离分配
 * ================================================================== */
BOOL mk_read(void* dst, const void* src, size_t n)
{
    SIZE_T rd = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &rd) && rd == n;
}

BOOL mk_readable(const void* p, size_t size)
{
    uintptr_t start = (uintptr_t)p;
    if (!p || size == 0 || start < 0x10000 || start > 0x7FFFFFFFFFFFULL)
        return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return FALSE;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (start + size >= start && start + size <= regionEnd);
}

BOOL mk_write(void* dst, const void* src, size_t n)
{
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(dst, src, n);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    VirtualProtect(dst, n, old, &old);
    return TRUE;
}

BOOL mk_seal_exec(uintptr_t p, size_t n)
{
    DWORD old = 0;
    return VirtualProtect((void*)p, n, PAGE_EXECUTE_READ, &old);
}

uintptr_t mk_alloc_near(uintptr_t target, size_t size)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t range = 0x7FFF0000ull;
    uintptr_t minimum = target > range ? target - range :
        (uintptr_t)info.lpMinimumApplicationAddress;
    uintptr_t maximum = target + range;
    uintptr_t sysMax = (uintptr_t)info.lpMaximumApplicationAddress;
    if (maximum > sysMax) maximum = sysMax;
    uintptr_t address = minimum & ~(granularity - 1);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((void*)address, &region, sizeof(region)) == 0) break;
        uintptr_t regionBase = (uintptr_t)region.BaseAddress;
        uintptr_t regionEnd = regionBase + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = (regionBase + granularity - 1) &
                                  ~(granularity - 1);
            if (candidate >= minimum && candidate + size <= regionEnd &&
                candidate + size <= maximum) {
                void* allocated = VirtualAlloc((void*)candidate, size,
                                               MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE);
                if (allocated) {
                    int64_t disp = (int64_t)(uintptr_t)allocated -
                                   (int64_t)(target + 5);
                    if (disp >= INT32_MIN && disp <= INT32_MAX)
                        return (uintptr_t)allocated;
                    VirtualFree(allocated, 0, MEM_RELEASE);
                }
            }
        }
        if (regionEnd <= address) break;
        address = regionEnd;
    }
    return 0;
}

/* ==================================================================
 * 日志：句柄复用 + 环形缓冲
 * ================================================================== */
static char* logkit_slot(LogKit* lg, LONG index)
{
    return lg->ring +
           (size_t)(index & (LOGKIT_SLOT_COUNT - 1)) * LOGKIT_LINE_MAX;
}

void logkit_open(LogKit* lg, const char* path, const char* tag, BOOL truncate)
{
    memset(lg, 0, sizeof(*lg));
    if (path) lstrcpynA(lg->path, path, MAX_PATH);
    if (tag)  lstrcpynA(lg->tag, tag, (int)sizeof(lg->tag));
    lg->file = CreateFileA(lg->path,
                           truncate ? GENERIC_WRITE : FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    lg->opened = (lg->file != INVALID_HANDLE_VALUE);
}

static void logkit_put(LogKit* lg, const char* line, int len)
{
    DWORD w = 0;
    if (!lg->opened || len <= 0) return;
    WriteFile(lg->file, line, (DWORD)len, &w, NULL);
    WriteFile(lg->file, "\r\n", 2, &w, NULL);
}

void logkit_close(LogKit* lg)
{
    if (!lg->opened) return;
    logkit_flush(lg);
    CloseHandle(lg->file);
    lg->file = INVALID_HANDLE_VALUE;
    lg->opened = 0;
}

void logkit_write(LogKit* lg, const char* fmt, ...)
{
    char buf[LOGKIT_LINE_MAX];
    va_list ap;
    if (!lg->opened) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logkit_put(lg, buf, (int)strlen(buf));
}

void logkit_queue(LogKit* lg, const char* fmt, ...)
{
    char buf[LOGKIT_LINE_MAX];
    va_list ap;
    LONG head, slot;
    size_t len;

    if (!lg->opened) return;
    /* 热路径不阻塞：抢不到锁直接丢弃该行 */
    if (InterlockedCompareExchange(&lg->lock, 1, 0) != 0) {
        InterlockedIncrement(&lg->dropped);
        return;
    }
    head = lg->head;
    if (head - lg->tail >= LOGKIT_SLOT_COUNT) {
        InterlockedIncrement(&lg->dropped);
        InterlockedExchange(&lg->lock, 0);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    len = strlen(buf);
    if (len > LOGKIT_LINE_MAX - 1) len = LOGKIT_LINE_MAX - 1;
    slot = head & (LOGKIT_SLOT_COUNT - 1);
    memcpy(logkit_slot(lg, head), buf, len);
    lg->slot_len[slot] = (int)len;
    InterlockedIncrement(&lg->head);
    InterlockedExchange(&lg->lock, 0);
}

int logkit_flush(LogKit* lg)
{
    int lines = 0;
    if (!lg->opened) return 0;
    while (lg->tail != lg->head) {
        LONG t = lg->tail;
        int len = lg->slot_len[t & (LOGKIT_SLOT_COUNT - 1)];
        if (len > 0) {
            logkit_put(lg, logkit_slot(lg, t), len);
            lines++;
        }
        lg->slot_len[t & (LOGKIT_SLOT_COUNT - 1)] = 0;
        lg->tail = t + 1;
    }
    if (lg->dropped) {
        char buf[128];
        LONG d = InterlockedExchange(&lg->dropped, 0);
        snprintf(buf, sizeof(buf), "[%s] log dropped %ld line(s)",
                 lg->tag, (long)d);
        logkit_put(lg, buf, (int)strlen(buf));
    }
    return lines;
}

/* ==================================================================
 * Hook：校验 / stub 分配 / 事务式补丁
 * ================================================================== */
static LogFn s_hook_log = NULL;

void hook_set_logger(LogFn logf)
{
    s_hook_log = logf;
}

static void hook_log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!s_hook_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_hook_log("%s", buf);
}

BOOL hook_point_verify(HookPoint* p)
{
    unsigned char buf[32];
    if (!p || !p->addr || !p->expect || p->len <= 0 ||
        p->len > (int)sizeof(buf))
        return FALSE;
    if (!mk_read(buf, (const void*)p->addr, (size_t)p->len) ||
        memcmp(buf, p->expect, (size_t)p->len) != 0) {
        hook_log("[hookkit] %s 原文校验失败 addr=%p",
                 p->name ? p->name : "?", (void*)p->addr);
        return FALSE;
    }
    return TRUE;
}

void hook_txn_begin(HookTxn* t, const char* name)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
}

uintptr_t hook_txn_stub(HookTxn* t, uintptr_t near_target, size_t size)
{
    uintptr_t addr;
    if (!t) return 0;
    addr = mk_alloc_near(near_target, size);
    if (!addr) {
        hook_log("[hookkit] %s stub 分配失败 near=%p size=%u",
                 t->name ? t->name : "?", (void*)near_target, (unsigned)size);
        return 0;
    }
    if (t->n_stub >= HOOKTXN_MAX_STUB) {
        VirtualFree((void*)addr, 0, MEM_RELEASE);
        hook_log("[hookkit] %s stub 登记表已满", t->name ? t->name : "?");
        return 0;
    }
    t->stub[t->n_stub++] = addr;
    return addr;
}

uintptr_t hook_txn_alloc(HookTxn* t, size_t size)
{
    void* p;
    if (!t) return 0;
    p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        hook_log("[hookkit] %s trampoline 分配失败 size=%u",
                 t->name ? t->name : "?", (unsigned)size);
        return 0;
    }
    if (t->n_stub >= HOOKTXN_MAX_STUB) {
        VirtualFree(p, 0, MEM_RELEASE);
        hook_log("[hookkit] %s stub 登记表已满", t->name ? t->name : "?");
        return 0;
    }
    t->stub[t->n_stub++] = (uintptr_t)p;
    return (uintptr_t)p;
}

BOOL hook_txn_write(HookTxn* t, uintptr_t addr, const void* bytes, int len)
{
    int i;
    if (!t || !addr || !bytes || len <= 0 || len > 16) return FALSE;
    if (t->n_patch >= HOOKTXN_MAX_PATCH) {
        hook_log("[hookkit] %s patch 登记表已满", t->name ? t->name : "?");
        return FALSE;
    }
    i = t->n_patch;
    if (!mk_read(t->patch[i].orig, (const void*)addr, (size_t)len)) {
        hook_log("[hookkit] %s 原字节读取失败 addr=%p",
                 t->name ? t->name : "?", (void*)addr);
        return FALSE;
    }
    t->patch[i].addr = addr;
    t->patch[i].len = len;
    t->patch[i].attempted = 0;
    t->n_patch++;   /* 先登记再写入，写入中途失败也能回滚 */
    if (!mk_write((void*)addr, bytes, (size_t)len)) {
        hook_log("[hookkit] %s 写入失败 addr=%p", t->name ? t->name : "?",
                 (void*)addr);
        return FALSE;
    }
    t->patch[i].attempted = 1;
    return TRUE;
}

BOOL hook_txn_commit(HookTxn* t)
{
    if (!t) return FALSE;
    /* 成功路径：补丁保留，stub 归属调用方，不再由事务释放 */
    t->n_patch = 0;
    t->n_stub = 0;
    return TRUE;
}

void hook_txn_rollback(HookTxn* t)
{
    int i;
    if (!t) return;
    for (i = t->n_patch - 1; i >= 0; i--) {
        if (!t->patch[i].attempted) continue;
        mk_write((void*)t->patch[i].addr, t->patch[i].orig,
                 (size_t)t->patch[i].len);
    }
    for (i = 0; i < t->n_stub; i++)
        VirtualFree((void*)t->stub[i], 0, MEM_RELEASE);
    t->n_patch = 0;
    t->n_stub = 0;
}

void hook_make_jmp32(unsigned char* out, int len, uintptr_t from, uintptr_t to)
{
    int32_t rel;
    if (!out || len < 5) return;
    memset(out, 0x90, (size_t)len);
    out[0] = 0xE9;
    rel = (int32_t)((int64_t)to - (int64_t)(from + 5));
    memcpy(out + 1, &rel, 4);
}

void hook_make_jmp_abs(unsigned char* out, uintptr_t to)
{
    if (!out) return;
    out[0] = 0xFF;
    out[1] = 0x25;
    out[2] = out[3] = out[4] = out[5] = 0;
    memcpy(out + 6, &to, 8);
}

/* ==================================================================
 * 版本档案：addrsig 结果 → 名称到地址
 * ================================================================== */
static LogFn s_prof_log = NULL;

void profile_set_logger(LogFn logf)
{
    s_prof_log = logf;
}

static void profile_resolve_log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!s_prof_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_prof_log("%s", buf);
}

int profile_load(GameProfile* p)
{
    int hits, i, j;
    if (!p || !p->sigs || p->n_sigs <= 0 || !p->res) return 0;
    hits = addrsig_resolve(p->sigs, p->n_sigs, p->base, p->res,
                           profile_resolve_log);
    for (i = 0; i < p->n_entries; i++) {
        p->entries[i].addr = 0;
        p->entries[i].rva = 0;
        p->entries[i].hit = 0;
        for (j = 0; j < p->n_sigs; j++) {
            if (!p->res[j].hit || !p->res[j].name) continue;
            if (strcmp(p->res[j].name, p->entries[i].name) != 0) continue;
            p->entries[i].addr = p->res[j].addr;
            p->entries[i].rva = p->res[j].rva;
            p->entries[i].hit = 1;
            break;
        }
    }
    return hits;
}

uintptr_t profile_addr(const GameProfile* p, const char* name)
{
    int i;
    if (!p || !p->entries || !name) return 0;
    for (i = 0; i < p->n_entries; i++) {
        if (p->entries[i].name && strcmp(p->entries[i].name, name) == 0)
            return p->entries[i].addr;
    }
    return 0;
}

uint32_t profile_rva(const GameProfile* p, const char* name)
{
    int i;
    if (!p || !p->entries || !name) return 0;
    for (i = 0; i < p->n_entries; i++) {
        if (p->entries[i].name && strcmp(p->entries[i].name, name) == 0)
            return p->entries[i].rva;
    }
    return 0;
}

int profile_hit(const GameProfile* p, const char* name)
{
    int i;
    if (!p || !p->entries || !name) return 0;
    for (i = 0; i < p->n_entries; i++) {
        if (p->entries[i].name && strcmp(p->entries[i].name, name) == 0)
            return p->entries[i].hit;
    }
    return 0;
}
