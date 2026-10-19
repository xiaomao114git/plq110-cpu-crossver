#!/usr/bin/env python3
"""把 .ko 的 __versions 节做成"存在但为空"，复刻 cpuinfo_spoof 的跨版本加载手法。

为什么这样做
------------
内核 kernel/module/version.c 的 check_version 对 __versions 节有三种分支：

  1. 节不存在       -> find_sec 返回 0 -> try_to_force_load() -> 未开 FORCE_LOAD 则 -ENOEXEC
  2. 节存在且非空   -> 逐符号比对 CRC -> 任一不匹配则 -ENOEXEC
  3. 节存在但为空   -> versindex 非 0，num_versions = 0/sizeof = 0
                       -> 循环不执行 -> pr_warn_once -> return 1 放行

而 same_magic() 里 has_crcs 取的就是 info->index.vers，非 0 即真，
于是 vermagic 比较退化为"只比第一个空格之后的部分"：

  模块: ' SMP preempt mod_unload modversions aarch64'
  设备: ' SMP preempt mod_unload modversions aarch64'

所以方案 3 同时达成两件事：跳过 CRC 逐符号比对 + 放宽 vermagic 主串。
代价是放弃加载期 ABI 校验 —— 结构真不匹配时不会拒绝，而是运行期出问题。

用法
----
  python strip_versions.py <ki_guard.ko>            # 就地改成空节
  python strip_versions.py <ki_guard.ko> --check    # 只报告，不改动
  python strip_versions.py <ki_guard.ko> --keep     # 保留 CRC 表（不做处理）

退出码：0 成功；1 校验失败；2 用法错误
"""
import struct
import sys
from pathlib import Path

SHF_ALLOC = 0x2
SHDR_FMT = "<IIQQQQIIQQ"
SHDR_SIZE = 64
SH_SIZE_OFF = 32          # Elf64_Shdr 中 sh_size 的字节偏移


def parse(d):
    if d[:4] != b"\x7fELF" or d[4] != 2:
        raise SystemExit("不是 ELF64")
    hdr = struct.unpack_from("<16sHHIQQQIHHHHHH", d)
    shoff, shentsize, shnum, shstrndx = hdr[6], hdr[11], hdr[12], hdr[13]

    entries = []
    for i in range(shnum):
        off = shoff + i * shentsize
        entries.append((off, list(struct.unpack_from(SHDR_FMT, d, off))))

    stroff = entries[shstrndx][1][4]
    strsize = entries[shstrndx][1][5]
    names = d[stroff:stroff + strsize]

    def name_of(o):
        return names[o:].split(b"\0", 1)[0].decode("ascii", "replace")

    return hdr, shoff, shentsize, entries, name_of


def find(entries, name_of, target):
    for off, e in entries:
        if name_of(e[0]) == target:
            return off, e
    return None, None


def report(ko: Path):
    d = ko.read_bytes()
    hdr, shoff, shentsize, entries, name_of = parse(d)
    off, e = find(entries, name_of, "__versions")
    tm_off, tm = find(entries, name_of, ".gnu.linkonce.this_module")
    print(f"  file          : {ko.name}  {len(d):,} bytes")
    print(f"  this_module   : {tm[5] if tm else 'MISSING'}")
    if e is None:
        print("  __versions    : 不存在  -> 会走 try_to_force_load，需要 FORCE_LOAD")
    else:
        print(f"  __versions    : sh_flags=0x{e[2]:x}  SHF_ALLOC={'是' if e[2] & SHF_ALLOC else '否'}  "
              f"sh_size={e[5]}")
        if e[5] == 0 and (e[2] & SHF_ALLOC):
            print("                  -> 已是空节，走放行分支")
        elif e[5] == 0:
            print("                  -> size=0 但无 SHF_ALLOC，find_sec 找不到，等同不存在")
        else:
            print("                  -> 非空，会逐符号比对 CRC")
    return d, hdr, shoff, shentsize, entries, name_of, off, e


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ko = Path(argv[1])
    if not ko.is_file():
        print(f"no such file: {ko}")
        return 2

    mode = "strip"
    if "--check" in argv[2:]:
        mode = "check"
    elif "--keep" in argv[2:]:
        mode = "keep"

    print("=== 处理前 ===")
    d, hdr, shoff, shentsize, entries, name_of, off, e = report(ko)

    if e is None:
        print("\n__versions 不存在。若目标内核未开 CONFIG_MODULE_FORCE_LOAD，加载会被拒。")
        print("解决：正常编译（不要动 Module.symvers），让 modpost 生成 __versions。")
        return 1

    if mode == "check":
        ok = (e[5] == 0 and (e[2] & SHF_ALLOC))
        print(f"\n--check: {'已是目标状态' if ok else '需要处理'}")
        return 0 if ok else 1

    if mode == "keep":
        print("\n--keep: 保留 CRC 表，不做处理。")
        return 0

    if e[5] == 0:
        print("\n已是空节，无需处理。")
        if not (e[2] & SHF_ALLOC):
            print("但缺 SHF_ALLOC，补上它（否则 find_sec 找不到该节）。")
            newflags = e[2] | SHF_ALLOC
            buf = bytearray(d)
            struct.pack_into("<Q", buf, off + 8, newflags)
            ko.write_bytes(bytes(buf))
            print("已补 SHF_ALLOC。")
        return 0

    # 关键：只改节表里 __versions 那一项的 sh_size，节体字节原样保留在文件中，
    # 不移动任何 sh_offset —— 这样其他所有内容字节不变。
    buf = bytearray(d)
    struct.pack_into("<Q", buf, off + SH_SIZE_OFF, 0)
    if not (e[2] & SHF_ALLOC):
        struct.pack_into("<Q", buf, off + 8, e[2] | SHF_ALLOC)
        print("  同时补上 SHF_ALLOC")

    # 自校验：除该节表项外，全文件字节必须一致
    diff = [i for i in range(len(d)) if d[i] != buf[i]]
    expect = set(range(off + SH_SIZE_OFF, off + SH_SIZE_OFF + 8))
    if not (e[2] & SHF_ALLOC):
        expect |= set(range(off + 8, off + 16))
    if not set(diff) <= expect:
        print(f"内部错误：改动了预期之外的 {len(set(diff) - expect)} 个字节，放弃写入")
        return 1

    orig_size = e[5]
    ko.write_bytes(bytes(buf))
    print(f"\n已写入：__versions sh_size {orig_size} -> 0（SHF_ALLOC 保留）")
    print(f"  改动字节数：{len(diff)}（全部位于节表项内）")

    print("\n=== 处理后 ===")
    report(ko)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
