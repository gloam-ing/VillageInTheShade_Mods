/* addrsig.c - 运行时地址签名解析实现（见 addrsig.h） */
#include "addrsig.h"
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char* s, unsigned char* out, int max)
{
    int n = 0;
    if (!s) return -1;
    while (s[0] && s[1] && n < max) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        s += 2;
    }
    if (s[0]) return -1;   /* 奇数长度 */
    return n;
}

/* 取主模块 .text 段的运行时 VA 与大小 */
static int pe_text(uintptr_t base, uintptr_t* va, size_t* size)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    WORD n = nt->FileHeader.NumberOfSections;
    for (WORD i = 0; i < n; i++) {
        if (memcmp(sh[i].Name, ".text", 5) == 0) {
            *va = base + sh[i].VirtualAddress;
            *size = sh[i].Misc.VirtualSize ? sh[i].Misc.VirtualSize
                                           : sh[i].SizeOfRawData;
            return 1;
        }
    }
    return 0;
}

/* 掩码扫描：返回命中数，首个命中写 *out_idx */
static int scan_masked(const unsigned char* blob, size_t len,
                       const unsigned char* pat, const unsigned char* mask,
                       size_t plen, size_t* out_idx)
{
    int cnt = 0;
    if (plen == 0 || len < plen) return 0;
    for (size_t i = 0; i + plen <= len; i++) {
        size_t j = 0;
        for (; j < plen; j++)
            if (mask[j] && blob[i + j] != pat[j]) break;
        if (j == plen) {
            cnt++;
            if (cnt == 1 && out_idx) *out_idx = i;
            if (cnt > 1) return cnt;
        }
    }
    return cnt;
}

int addrsig_resolve(const AddrSig* sigs, int n, uintptr_t base,
                    AddrRes* out, void (*logf)(const char* fmt, ...))
{
    uintptr_t tva = 0;
    size_t tsize = 0;
    int ok = 0;
    int i;

    for (i = 0; i < n; i++) {
        out[i].name = sigs[i].name;
        out[i].hit = 0;
        out[i].rva = 0;
        out[i].addr = 0;
    }
    if (!base || !pe_text(base, &tva, &tsize)) {
        if (logf) logf("[addrsig] .text 不可用 base=%p", (void*)base);
        return 0;
    }
    const unsigned char* blob = (const unsigned char*)tva;

    for (i = 0; i < n; i++) {
        const AddrSig* s = &sigs[i];
        unsigned char pat[512], mask[512];
        int plen = hex_decode(s->pat_hex, pat, sizeof(pat));
        int mlen = hex_decode(s->mask_hex, mask, sizeof(mask));
        if (plen <= 0 || mlen != plen || (size_t)plen > tsize) {
            if (logf) logf("[addrsig] 签名无效 %s", s->name);
            continue;
        }
        size_t idx = 0;
        int cnt = scan_masked(blob, tsize, pat, mask, (size_t)plen, &idx);
        if (cnt != 1) {
            if (logf) logf("[addrsig] MISS %s (%d 处命中)", s->name, cnt);
            continue;
        }
        uint32_t text_rva = (uint32_t)(tva - base);
        uint32_t rva = 0;
        if (s->kind == ADDRSIG_FUNC) {
            if ((int)idx < s->off) continue;
            rva = text_rva + (uint32_t)idx - (uint32_t)s->off;
        } else {
            int32_t disp = 0;
            memcpy(&disp, blob + idx + s->off, 4);
            uint32_t insn_rva = text_rva + (uint32_t)idx + (uint32_t)(s->off - 3);
            rva = insn_rva + 7 + (uint32_t)disp;
        }
        out[i].hit = 1;
        out[i].rva = rva;
        out[i].addr = base + rva;
        ok++;
    }
    if (logf && ok > 0)
        logf("[addrsig] %d/%d OK", ok, n);
    return ok;
}
