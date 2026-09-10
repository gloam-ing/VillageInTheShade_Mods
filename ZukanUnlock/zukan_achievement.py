# -*- coding: utf-8 -*-
"""图鉴补成就工具（Village in the Shade v1.08.1）——无第三方依赖版。

原理：
  全图鉴 Steam 成就在“游戏原生新增图鉴条目”时判定。改存档把图鉴插满不会
  触发；本工具把图鉴插满后刻意保留木材（dataID 230000）不点亮（已点亮则
  删除），玩家进游戏砍树获得木材，让原生新增路径跑一次即补发成就。

格式（NIS SER）：
  文件 = YKCMP_V1 容器（type 8 = 原始 LZ4 block）-> SER 树。
  SER 记录: [type u8][name 偏移 u32][size u32][内容]
    type 2=object(子记录), 3=map(+u32 count), 4=pointer(+u32 addr),
    0/5=叶子。名字偏移相对头部 strtab 基址（raw+0x0C）。

用法:
  python zukan_achievement.py <存档路径> [lz4.dll路径]
  （打包为 EXE 后双击运行，按提示拖入存档即可；压缩库随 EXE 内置。）

写回前自动备份（同目录 .zukan_ach_<时间戳>），写完重解析验证。
"""
import ctypes
import os
import struct
import sys
import time
import zlib

WOOD_ID = 230000
ITEM_BIN = 'item_ids.bin'      # zlib(全部物品 dataID u32 小端, 升序)


# ---------- LZ4 block 解压（纯 Python） ----------
def lz4_decompress(src):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        mlen = (token & 15) + 4
        if (token & 15) == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        pos = len(dst) - off
        for _ in range(mlen):
            dst.append(dst[pos])
            pos += 1
    return bytes(dst)


# ---------- 压缩（优先内置 lz4，其次 lz4.dll） ----------
def make_compress():
    try:
        import lz4.block

        def via_lz4(src):
            return lz4.block.compress(bytes(src), mode='high_compression',
                                      store_size=False)
        return via_lz4
    except ImportError:
        pass
    path = None
    if len(sys.argv) > 2:
        path = sys.argv[2]
    if not path:
        path = os.environ.get('LZ4DLL')
    if not path:
        here = resource_dir()
        cand = os.path.join(here, 'lz4.dll')
        if os.path.isfile(cand):
            path = cand
    if not path or not os.path.isfile(path):
        sys.exit('缺少 LZ4 压缩支持：请 pip install lz4，'
                 '或提供游戏目录 lz4.dll（第二参数/环境变量 LZ4DLL）。')
    lib = ctypes.CDLL(path)
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int

    def via_dll(src):
        if not hasattr(lib, 'LZ4_compress_HC'):
            raise RuntimeError('lz4.dll 缺少 LZ4_compress_HC 导出')
        fn = lib.LZ4_compress_HC
        fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                       ctypes.c_int, ctypes.c_int, ctypes.c_int]
        fn.restype = ctypes.c_int
        src = bytes(src)
        cap = lib.LZ4_compressBound(len(src))
        buf = ctypes.create_string_buffer(cap)
        n = fn(src, buf, len(src), cap, 12)
        if n <= 0:
            raise RuntimeError('LZ4_compress_HC failed')
        return buf.raw[:n]
    return via_dll


# ---------- 容器与 SER 树 ----------
def load_raw(path):
    data = open(path, 'rb').read()
    if data[:8] != b'YKCMP_V1':
        raise RuntimeError('不是 YKCMP_V1 存档')
    typ, csize, dsize = struct.unpack_from('<III', data, 8)
    if typ != 8:
        raise RuntimeError('不支持的压缩类型 %d' % typ)
    return bytearray(lz4_decompress(data[0x14:csize], )[:dsize])


def resource_dir():
    if getattr(sys, 'frozen', False):
        return getattr(sys, '_MEIPASS', os.path.dirname(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def load_item_ids():
    here = resource_dir()
    with open(os.path.join(here, ITEM_BIN), 'rb') as f:
        raw = zlib.decompress(f.read())
    n = len(raw) // 4
    return sorted(struct.unpack('<%dI' % n, raw))


def parse_ser(raw, pos, end, out, depth=0):
    while pos < end:
        typ = raw[pos]
        nid, size = struct.unpack_from('<II', raw, pos + 1)
        if typ > 5 or size > end - pos:
            return
        hdr = pos + 9
        rec = {'type': typ, 'off': pos, 'size': size}
        if typ == 2:
            rec['children'] = []
            parse_ser(raw, hdr, hdr + size, rec['children'], depth + 1)
            pos = hdr + size
        elif typ in (1, 3):
            count = struct.unpack_from('<I', raw, hdr)[0]
            rec['count'] = count
            body = hdr + 4
            nxt = body + size
            if typ == 3:
                rec['children'] = []
                parse_ser(raw, body, nxt, rec['children'], depth + 1)
            else:
                rec['data'] = raw[body:nxt]
            pos = nxt
        elif typ == 4:
            addr = struct.unpack_from('<I', raw, hdr)[0]
            rec['addr'] = addr
            body = hdr + 4
            nxt = body + size
            if nxt > body:
                rec['children'] = []
                parse_ser(raw, body, nxt, rec['children'], depth + 1)
            pos = nxt
        else:
            rec['data'] = raw[hdr:hdr + size]
            pos = hdr + size
        out.append(rec)


def parse_root(raw):
    stream_len = struct.unpack_from('<I', raw, 0x15)[0]
    out = []
    parse_ser(raw, 0x19, 0x19 + stream_len, out)
    return out


def find_rec(recs, name):
    for r in recs:
        if r.get('name') == name:
            return r
    for r in recs:
        for c in r.get('children', []):
            hit = find_rec([c], name)
            if hit:
                return hit
    return None


def rec_name(raw, nid):
    strtab = struct.unpack_from('<I', raw, 0x0C)[0]
    base = strtab + nid
    end = raw.find(b'\0', base, base + 256)
    if end < 0:
        return ''
    return raw[base:end].decode('utf-8', 'replace')


def name_recs(raw, recs):
    for r in recs:
        r['name'] = rec_name(raw, struct.unpack_from('<I', raw, r['off'] + 1)[0])
        for c in r.get('children', []):
            name_recs(raw, [c])


def collect_t4(recs, out):
    for r in recs:
        if r['type'] == 4:
            out.append((r['off'], r.get('addr', 0)))
        for c in r.get('children', []):
            collect_t4([c], out)


def insert_missing(raw, enc, missing):
    if not missing:
        return
    total, strtab = struct.unpack_from('<II', raw, 8)
    strtab_len = total - strtab
    start_idx = enc.get('count', 0)
    name_blob = bytearray()
    nk_off, nv_off = [], []
    off = strtab_len
    for i in range(len(missing)):
        nk = ('%dk' % (start_idx + i)).encode('ascii')
        nv = ('%dv' % (start_idx + i)).encode('ascii')
        nk_off.append(off)
        name_blob += nk + b'\0'
        off += len(nk) + 1
        nv_off.append(off)
        name_blob += nv + b'\0'
        off += len(nv) + 1
    delta2 = len(name_blob)
    raw[total:total] = name_blob
    del raw[len(raw) - delta2:]

    blob = bytearray()
    for j, kid in enumerate(missing):
        blob += struct.pack('<BII', 0, nk_off[j], 8) + struct.pack('<Q', kid)
        blob += struct.pack('<BII', 0, nv_off[j], 1) + b'\x01'
    delta = len(blob)
    s0 = enc['off'] + 13 + enc['size']
    if raw[-delta:] != b'\0' * delta:
        raise RuntimeError('解压缓冲尾部空间不足')
    t4 = []
    collect_t4(parse_root(raw), t4)
    struct.pack_into('<I', raw, enc['off'] + 5, enc['size'] + delta)
    struct.pack_into('<I', raw, enc['off'] + 9,
                     enc.get('count', 0) + len(missing))
    total2, _ = struct.unpack_from('<II', raw, 8)
    struct.pack_into('<II', raw, 8, total2 + delta, strtab + delta)
    sl = struct.unpack_from('<I', raw, 0x15)[0]
    struct.pack_into('<I', raw, 0x15, sl + delta)
    raw[s0:s0] = blob
    del raw[len(raw) - delta:]
    for offp, addr in t4:
        noff = offp + delta if offp >= s0 else offp
        if addr != 0xffffffff and addr >= s0:
            struct.pack_into('<I', raw, noff + 9, addr + delta)


def remove_one(raw, enc, did):
    ch = enc.get('children', [])
    pair = None
    for i in range(0, len(ch) - 1, 2):
        k, v = ch[i], ch[i + 1]
        if k.get('type') == 0 and k.get('size') == 8 and \
                v.get('type') == 0 and v.get('size') == 1:
            kid = struct.unpack('<Q', raw[k['off'] + 9:k['off'] + 17])[0]
            if kid == did:
                pair = (k, v)
                break
    if pair is None:
        return False
    k, v = pair
    s_del = k['off']
    e_del = v['off'] + 9 + v['size']
    delta = s_del - e_del
    t4 = []
    collect_t4(parse_root(raw), t4)
    struct.pack_into('<I', raw, enc['off'] + 5, enc['size'] + delta)
    struct.pack_into('<I', raw, enc['off'] + 9,
                     enc.get('count', 0) - 1)
    total, strtab = struct.unpack_from('<II', raw, 8)
    struct.pack_into('<II', raw, 8, total + delta, strtab + delta)
    sl = struct.unpack_from('<I', raw, 0x15)[0]
    struct.pack_into('<I', raw, 0x15, sl + delta)
    for offp, addr in t4:
        noff = offp + delta if offp >= e_del else offp
        if addr != 0xffffffff and addr >= e_del:
            struct.pack_into('<I', raw, noff + 9, addr + delta)
    removed = bytes(raw[s_del:e_del])
    del raw[s_del:e_del]
    raw += b'\0' * len(removed)
    return True


def map_stats(raw, enc):
    ch = enc.get('children', [])
    cnt = okv = 0
    has_wood = False
    for i in range(0, len(ch) - 1, 2):
        k, v = ch[i], ch[i + 1]
        if k.get('type') == 0 and k.get('size') == 8 and \
                v.get('type') == 0 and v.get('size') == 1:
            cnt += 1
            kid = struct.unpack('<Q', raw[k['off'] + 9:k['off'] + 17])[0]
            if kid == WOOD_ID:
                has_wood = True
            if raw[v['off'] + 9] == 1:
                okv += 1
    return cnt, okv, has_wood


def pack_raw(path, raw):
    comp = make_compress()(bytes(raw))
    blob = b'YKCMP_V1' + struct.pack('<III', 8, len(comp) + 0x14, len(raw)) + comp
    size = os.path.getsize(path)
    if len(blob) > size:
        raise RuntimeError('压缩后超过原文件大小')
    blob += b'\0' * (size - len(blob))
    with open(path, 'wb') as f:
        f.write(blob)


def main():
    ids = load_item_ids()
    if len(sys.argv) < 2:
        path = input('请把存档文件拖到窗口后回车: ').strip().strip('"')
    else:
        path = sys.argv[1].strip().strip('"')
    if not os.path.isfile(path):
        sys.exit('存档不存在: %s' % path)

    bak = path + '.zukan_ach_' + time.strftime('%Y%m%d_%H%M%S')
    with open(path, 'rb') as f:
        orig = f.read()
    with open(bak, 'wb') as f:
        f.write(orig)

    raw = load_raw(path)
    root = parse_root(raw)
    name_recs(raw, root)
    enc = find_rec(root, 'encyclopediaReleaseMap_')
    if enc is None:
        raise RuntimeError('encyclopediaReleaseMap_ not found')

    have = set()
    ch = enc.get('children', [])
    for i in range(0, len(ch) - 1, 2):
        k = ch[i]
        if k.get('type') == 0 and k.get('size') == 8:
            have.add(struct.unpack('<Q', raw[k['off'] + 9:k['off'] + 17])[0])

    missing = sorted(set(ids) - have - {WOOD_ID})
    if missing:
        insert_missing(raw, enc, missing)
        root = parse_root(raw)
        name_recs(raw, root)
        enc = find_rec(root, 'encyclopediaReleaseMap_')
    if remove_one(raw, enc, WOOD_ID):
        root = parse_root(raw)
        name_recs(raw, root)
        enc = find_rec(root, 'encyclopediaReleaseMap_')

    cnt, okv, has_wood = map_stats(raw, enc)
    expect = len(ids) - 1
    if has_wood or okv != cnt or cnt != expect:
        raise RuntimeError('验证失败: cnt=%d okv=%d wood=%s 期望=%d'
                           % (cnt, okv, has_wood, expect))
    pack_raw(path, raw)
    print('完成: 图鉴 %d/%d（仅木材未点亮），备份 %s'
          % (cnt, expect + 1, os.path.basename(bak)))
    print('下一步: 进游戏读档 -> 砍树获得木材 -> Steam 成就应弹出。')


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print('错误: %s' % e)
    if os.environ.get('ZUKAN_NO_PAUSE') != '1':
        input('按回车退出...')
