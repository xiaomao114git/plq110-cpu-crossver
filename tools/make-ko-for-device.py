#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make-ko-for-device.py —— 为任意设备生成适配的 cpuinfo_redirect.ko

原理：不重新编译，直接改 .ko 里的两处：
  1. vermagic 字符串  → 目标设备的 uname -r + 从 config 推断的标志段
  2. __versions 段名  → 改成 ZZversions，让内核 check_version 找不到而跳过 CRC 校验

限制：
  - 目标内核必须与模板同一 GKI 分支（布局才一致）
  - 新 vermagic 不能超过模板里的原长度
  - 改串解决不了 CFI 和 vendor hook 缺失，仍需实测
"""
import os, sys, re, struct, argparse, hashlib

def find_vermagic(data):
    m = re.search(rb'vermagic=([^\x00]+)', data)
    return m if m else None

def patch_vermagic(data, uts, flags):
    m = find_vermagic(data)
    if not m:
        return data, 'FAIL: 模板里没有 vermagic 字符串'
    old = m.group(1); off = m.start(1)
    new = uts + flags
    if len(new) > len(old):
        return data, 'FAIL: 需要 %d 字节，模板只有 %d 字节' % (len(new), len(old))
    nd = bytearray(data)
    nd[off:off+len(old)] = new + b'\x00' * (len(old) - len(new))
    return bytes(nd), 'OK (%d/%d 字节)' % (len(new), len(old))

def kill_versions(data):
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return data, 'FAIL: 不是 ELF64'
    try:
        e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
        e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
        e_shnum = struct.unpack_from('<H', data, 0x3c)[0]
        e_shstrndx = struct.unpack_from('<H', data, 0x3e)[0]
        sh_str = e_shoff + e_shstrndx * e_shentsize
        str_off = struct.unpack_from('<Q', data, sh_str + 0x18)[0]
    except Exception as e:
        return data, 'FAIL: ELF 解析出错 %s' % e
    nd = bytearray(data); found = 0
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        if sh + 0x20 > len(data): break
        no = struct.unpack_from('<I', data, sh)[0]
        a = str_off + no
        if a >= len(data): continue
        e = data.find(b'\x00', a)
        if e < 0: continue
        if data[a:e] == b'__versions':
            nd[a:a+10] = b'ZZversions'
            found += 1
    if found:
        return bytes(nd), 'OK (改了 %d 个)' % found
    return data, 'SKIP (模板本来就没有 __versions，可能已 --no-crc)'

# 从目标设备的 config.gz 推断 vermagic 标志段
def guess_flags(config_path):
    smp = preempt = unload = modver = True
    arch = b' aarch64'
    if config_path and os.path.exists(config_path):
        import gzip
        try:
            txt = gzip.open(config_path, 'rt', errors='replace').read()
        except Exception:
            txt = open(config_path, 'r', errors='replace').read()
        def has(k):
            return any(l.strip() == k + '=y' for l in txt.splitlines())
        def notset(k):
            return any(l.strip() == '# ' + k + ' is not set' for l in txt.splitlines())
        smp = has('CONFIG_SMP')
        preempt = has('CONFIG_PREEMPT')
        unload = has('CONFIG_MODULE_UNLOAD')
        modver = has('CONFIG_MODVERSIONS')
    parts = []
    if smp: parts.append('SMP')
    if preempt: parts.append('preempt')
    if unload: parts.append('mod_unload')
    if modver: parts.append('modversions')
    return b' ' + ' '.join(parts).encode() + arch

def main():
    ap = argparse.ArgumentParser(description='为设备生成适配的 .ko')
    ap.add_argument('--template', required=True, help='模板 .ko 路径')
    ap.add_argument('--uname-r', required=True, help="目标设备 uname -r，如 6.6.89-android15-8-...")
    ap.add_argument('--config', default='', help='目标设备 config.gz（可选，用于推断标志段）')
    ap.add_argument('--out', default='', help='输出路径')
    a = ap.parse_args()

    data = open(a.template, 'rb').read()
    m = find_vermagic(data)
    if not m:
        sys.exit('模板里没有 vermagic')
    print('模板: %s' % os.path.basename(a.template))
    print('  原 vermagic[%d]: %s' % (len(m.group(1)), m.group(1).decode()))
    print()

    flags = guess_flags(a.config)
    print('推断标志段: %s' % flags.decode())
    print()

    uts = a.uname_r.strip().encode()
    d, r1 = patch_vermagic(data, uts, flags)
    print('1) 改 vermagic : %s' % r1)
    if r1.startswith('FAIL'):
        sys.exit(1)
    d, r2 = kill_versions(d)
    print('2) 处理 CRC   : %s' % r2)

    outp = a.out or (os.path.splitext(a.template)[0] + '-for-' + re.sub(r'[^A-Za-z0-9._-]', '_', a.uname_r)[:40] + '.ko')
    open(outp, 'wb').write(d)
    h = hashlib.sha256(d).hexdigest()
    print()
    print('产物: %s' % outp)
    print('  %d B  sha256 %s' % (len(d), h[:32]))
    mm = find_vermagic(d)
    print('  vermagic: %s' % (mm.group(1).decode() if mm else '?'))

if __name__ == "__main__":
    main()
