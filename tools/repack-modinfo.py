#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
repack-modinfo.py —— 在 .modinfo 段内重新分配空间，让 vermagic 能变长

原理：.modinfo 是一串 \0 分隔的 key=value。总大小固定，但每一项的分配可以调。
      description 通常很长（70+ 字节），而且对加载毫无影响。
      把它缩短，把省下来的空间给 vermagic。

这样不用改 ELF 段结构、不用改任何偏移，纯粹是段内字节重排。
"""
import os, sys, re, struct, hashlib, argparse

def find_section(data, want):
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3e)[0]
    sh_str = e_shoff + e_shstrndx * e_shentsize
    str_off = struct.unpack_from('<Q', data, sh_str + 0x18)[0]
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        no = struct.unpack_from('<I', data, sh)[0]
        off, size = struct.unpack_from('<QQ', data, sh + 0x18)
        a = str_off + no
        e = data.find(b'\x00', a)
        if e > 0 and data[a:e] == want.encode():
            return off, size
    return None, None

def parse_modinfo(blob):
    items = []
    cur = b''
    for b in blob:
        if b == 0:
            if cur: items.append(cur)
            cur = b''
        else:
            cur += bytes([b])
    if cur: items.append(cur)
    return items

def rebuild(items, target_size, new_vermagic, desc_max=8, drop_srcversion=False):
    out = []
    for it in items:
        if it.startswith(b'vermagic='):
            out.append(b'vermagic=' + new_vermagic)
        elif it.startswith(b'description='):
            d = it[len(b'description='):]
            if len(d) > desc_max:
                d = d[:desc_max]
            out.append(b'description=' + d)
        elif it.startswith(b'srcversion=') and drop_srcversion:
            continue   # 空间不够时丢弃（不影响加载）
        else:
            out.append(it)
    body = b'\x00'.join(out) + b'\x00'
    if len(body) > target_size:
        return None, '超长 %d > %d' % (len(body), target_size)
    body = body + b'\x00' * (target_size - len(body))
    return body, 'ok'

def kill_versions(data):
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3e)[0]
    sh_str = e_shoff + e_shstrndx * e_shentsize
    str_off = struct.unpack_from('<Q', data, sh_str + 0x18)[0]
    nd = bytearray(data); n = 0
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        no = struct.unpack_from('<I', data, sh)[0]
        a = str_off + no
        e = data.find(b'\x00', a)
        if e < 0: continue
        if data[a:e] == b'__versions':
            nd[a:a+10] = b'ZZversions'; n += 1
    return bytes(nd), n

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--template', required=True)
    ap.add_argument('--uname-r', required=True)
    ap.add_argument('--flags', default=' SMP preempt mod_unload modversions aarch64')
    ap.add_argument('--out', default='')
    a = ap.parse_args()

    data = open(a.template, 'rb').read()
    off, size = find_section(data, '.modinfo')
    if off is None: sys.exit('找不到 .modinfo')
    print('模板: %s' % os.path.basename(a.template))
    print('  .modinfo off=%d size=%d' % (off, size))

    items = parse_modinfo(data[off:off+size])
    print('  原条目 %d 个:' % len(items))
    for it in items:
        print('     %s' % it.decode('utf-8', errors='replace')[:76])

    new_vm = a.uname_r.strip().encode() + a.flags.encode()
    print()
    print('  目标 vermagic 长度 %d 字节' % len(new_vm))
    # 逐级腾空间：先缩 description，再丢 srcversion
    body, st = rebuild(items, size, new_vm, desc_max=8, drop_srcversion=False)
    if body is None:
        print('  第一轮(%s)，丢弃 srcversion 重试' % st)
        body, st = rebuild(items, size, new_vm, desc_max=8, drop_srcversion=True)
    if body is None:
        body, st = rebuild(items, size, new_vm, desc_max=1, drop_srcversion=True)
    print('  重排结果: %s' % st)
    if body is None:
        print('  .modinfo 总容量 %d 字节，装不下 %d 字节的 vermagic' % (size, len(new_vm)))
        sys.exit(1)

    nd = bytearray(data)
    nd[off:off+size] = body
    nd, nver = kill_versions(bytes(nd))
    print('  CRC 段处理: 改了 %d 个' % nver)

    outp = a.out or (os.path.splitext(a.template)[0] + '-adapt.ko')
    open(outp, 'wb').write(nd)
    m = re.search(rb'vermagic=([^\x00]+)', nd)
    print()
    print('产物: %s  %d B' % (outp, len(nd)))
    print('  vermagic[%d]: %s' % (len(m.group(1)), m.group(1).decode()))

if __name__ == "__main__":
    main()
