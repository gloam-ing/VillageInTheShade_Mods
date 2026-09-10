/* addrsig.h - 运行时地址签名解析（跨游戏版本自动定位）
 * ------------------------------------------------------------------
 * 思路：函数入口 / 关键指令的字节特征在版本间基本不变，但里面
 * RIP 相对位移（lea/mov [rip+disp]、call rel32、jmp rel32）会随
 * 编译器重排变化。签名构建时把这些位移全部置通配（mask=0），
 * 只保留操作码 + ModRM + 寄存器编码。
 *
 * 两种签名：
 *   ADDRSIG_FUNC     - 函数入口附近唯一字节窗口；off = 窗口起点到
 *                      函数入口的偏移（窗口起点在函数内时为正）。
 *   ADDRSIG_RIPGLOBAL- .text 内一条 RIP 相对引用某全局/虚表的指令
 *                      及其唯一上下文；off = disp32 在窗口内的偏移。
 *                      运行时重算 disp 目标 = 新版本里的全局 RVA。
 *
 * 用法（mod_init / DllMain 开头）：
 *   static const AddrSig sigs[] = { ... };
 *   static AddrRes res[sizeof(sigs)/sizeof(sigs[0])];
 *   addrsig_resolve(sigs, n, base, res, logf);
 * 之后用 res[i].addr（= base + rva）或 .rva 驱动逻辑。
 * 签名缺失时 hit=0，mod 应降级/禁用对应功能并打日志，绝不硬用旧值。
 *
 * 本文件为各 mod 共用副本。
 */
#ifndef ADDRSIG_H
#define ADDRSIG_H

#include <stdint.h>
#include <windows.h>

typedef enum { ADDRSIG_FUNC = 0, ADDRSIG_RIPGLOBAL = 1 } AddrSigKind;

typedef struct {
    const char* name;     /* 日志用 */
    AddrSigKind kind;
    const char* pat_hex;  /* 小写十六进制字节 */
    const char* mask_hex; /* 与 pat_hex 等长；'0'=通配，其余=必须相等 */
    int off;              /* FUNC: window_off；RIPGLOBAL: disp_off */
} AddrSig;

typedef struct {
    const char* name;
    int hit;              /* 1=唯一命中 */
    uint32_t rva;         /* 解析出的 RVA（相对 base） */
    uintptr_t addr;       /* base + rva */
} AddrRes;

/* 解析全部签名；logf 可为 NULL。返回命中数。 */
int addrsig_resolve(const AddrSig* sigs, int n, uintptr_t base,
                    AddrRes* out, void (*logf)(const char* fmt, ...));

#endif
