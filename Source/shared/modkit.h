/* modkit.h - 各 mod 共用的基础工具层
 * ------------------------------------------------------------------
 * 内容：
 *   日志   LogKit / logkit_open|write|queue|flush|close
 *          - 句柄复用；热路径只写内存环，不产生文件 IO
 *          - 环形缓冲满或抢锁失败时丢弃该行并计数，绝不阻塞
 *   内存   mk_read / mk_readable / mk_write / mk_seal_exec / mk_alloc_near
 *   Hook   HookPoint（声明式钩子点）+ HookTxn（patch/stub/rollback 事务）
 *          hook_make_jmp32 / hook_make_jmp_abs 生成重定向指令
 *   档案   GameProfile：addrsig 解析结果 → 名称到地址映射
 *          （地址一律运行时解析，源码里的 0x…… 只是日志标签）
 *
 * 依赖：addrsig.h（同一目录）。各 mod 编译时加 -I..\shared 并链接
 *       addrsig.c 与 modkit.c。
 */
#ifndef MODKIT_H
#define MODKIT_H

#include <windows.h>
#include <stdint.h>
#include "addrsig.h"

/* 通用日志函数指针：组件用它解耦具体日志实现 */
typedef void (*LogFn)(const char* fmt, ...);

/* ======================= 日志 ======================= */
#define LOGKIT_SLOT_COUNT 64
#define LOGKIT_LINE_MAX   512

typedef struct {
    HANDLE          file;
    char            path[MAX_PATH];
    char            tag[16];
    char            ring[LOGKIT_SLOT_COUNT * LOGKIT_LINE_MAX];
    int             slot_len[LOGKIT_SLOT_COUNT];
    volatile LONG   head;      /* 已发布槽位数（生产侧推进） */
    volatile LONG   tail;      /* 已落盘槽位数（消费侧推进） */
    volatile LONG   dropped;   /* 丢弃的行数 */
    volatile LONG   lock;
    int             opened;
} LogKit;

void logkit_open(LogKit* lg, const char* path, const char* tag, BOOL truncate);
void logkit_close(LogKit* lg);
void logkit_write(LogKit* lg, const char* fmt, ...);   /* 直写（初始化/安装期） */
void logkit_queue(LogKit* lg, const char* fmt, ...);   /* 入环（热路径） */
int  logkit_flush(LogKit* lg);                          /* 批量落盘，返回行数 */

/* ======================= 内存 ======================= */
BOOL      mk_read(void* dst, const void* src, size_t n);
BOOL      mk_readable(const void* p, size_t size);
BOOL      mk_write(void* dst, const void* src, size_t n);
BOOL      mk_seal_exec(uintptr_t p, size_t n);
uintptr_t mk_alloc_near(uintptr_t target, size_t size);

/* ======================= Hook ======================= */
typedef struct {
    const char*          name;    /* 日志用 */
    uintptr_t            addr;    /* 运行时可写地址 */
    const unsigned char* expect;  /* 期望原文 */
    int                  len;
} HookPoint;

#define HOOKTXN_MAX_PATCH 8
#define HOOKTXN_MAX_STUB  4

typedef struct {
    const char* name;
    int         n_patch;
    struct {
        uintptr_t     addr;
        int           len;
        unsigned char orig[16];
        int           attempted;
    } patch[HOOKTXN_MAX_PATCH];
    int       n_stub;
    uintptr_t stub[HOOKTXN_MAX_STUB];
} HookTxn;

void      hook_set_logger(LogFn logf);
BOOL      hook_point_verify(HookPoint* p);
void      hook_txn_begin(HookTxn* t, const char* name);
uintptr_t hook_txn_stub(HookTxn* t, uintptr_t near_target, size_t size);
uintptr_t hook_txn_alloc(HookTxn* t, size_t size);
BOOL      hook_txn_write(HookTxn* t, uintptr_t addr, const void* bytes, int len);
BOOL      hook_txn_commit(HookTxn* t);
void      hook_txn_rollback(HookTxn* t);
void      hook_make_jmp32(unsigned char* out, int len,
                          uintptr_t from, uintptr_t to);
void      hook_make_jmp_abs(unsigned char* out, uintptr_t to);

/* ======================= 版本档案 ======================= */
typedef struct {
    const char* name;   /* 与 AddrSig.name 对应 */
    uintptr_t   addr;
    uint32_t    rva;
    int         hit;
} ProfEntry;

typedef struct {
    const char*    mod;
    uintptr_t      base;
    const AddrSig* sigs;
    int            n_sigs;
    AddrRes*       res;
    ProfEntry*     entries;
    int            n_entries;
} GameProfile;

void      profile_set_logger(LogFn logf);
int       profile_load(GameProfile* p);       /* 返回签名命中数 */
uintptr_t profile_addr(const GameProfile* p, const char* name);
uint32_t  profile_rva(const GameProfile* p, const char* name);
int       profile_hit(const GameProfile* p, const char* name);

#endif
