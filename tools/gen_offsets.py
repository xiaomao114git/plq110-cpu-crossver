# -*- coding: utf-8 -*-
"""gen_offsets.py - 从目标内核的 BTF 推导 proc_dir_entry/proc_ops 布局，生成 cr_offsets.h

用法:
    python gen_offsets.py --btf vmlinux.btf --out cr_offsets.h
    python gen_offsets.py --device            # 自动从 adb 设备取 BTF

为什么需要它: proc_dir_entry 是内核私有结构, 各版本字段布局不同。原先的实现把
偏移写死成 6.6.89 的实测值, 换内核就必须手工重算, 而且写错会导致内核 oops。
BTF 里带着目标内核的真实布局, 直接读它就能生成正确的偏移表 —— 换版本只需重新生成。

注意: 只读 BTF, 不执行任何内核代码; 生成的 .h 仅含常量。
"""
import argparse, os, struct, subprocess, sys

MAGIC = 0xeb9f


def find_string(strings, off):
    if off <= 0 or off >= len(strings):
        return ""
    end = strings.find(b"\0", off)
    return strings[off:end].decode("utf-8", "replace") if end > 0 else ""


def parse_btf(data):
    magic, version, flags, hlen, toff, tlen, soff, slen = struct.unpack_from(
        "<HBBIIIII", data, 0)
    if magic != MAGIC:
        raise SystemExit("不是 BTF: magic=%#x" % magic)
    strings = data[hlen + soff: hlen + soff + slen]
    pos, end, tid = hlen + toff, hlen + toff + tlen, 1
    structs = {}
    while pos < end:
        name_off, info, size = struct.unpack_from("<III", data, pos)
        kind = (info >> 24) & 31
        vlen = info & 0xFFFF
        kflag = info >> 31
        pos += 12
        if kind in (4, 5):                      # STRUCT / UNION
            tname = find_string(strings, name_off)
            members = []
            for i in range(vlen):
                mn, mt, mo = struct.unpack_from("<III", data, pos + i * 12)
                bits = (mo & 0xFFFFFF) if kflag else mo
                members.append((find_string(strings, mn), bits // 8,
                                (mo >> 24) if kflag else 0))
            structs[tname] = {"size": size, "members": members}
            pos += 12 * vlen
        elif kind in (6, 13):                   # ENUM / FUNC_PROTO
            pos += 8 * vlen
        elif kind in (15, 18):                  # DATASEC / ENUM64
            pos += 12 * vlen
        elif kind == 1:                         # INT
            pos += 4
        elif kind == 3:                         # ARRAY
            pos += 12
        elif kind in (14, 19):                  # VAR / DECL_TAG
            pos += 4
        tid += 1
    return structs


def resolve(structs, struct_name, field, anon_offset=None):
    """取字段字节偏移。BTF 里匿名 union 成员名为空，用 anon_offset 指定。"""
    st = structs.get(struct_name)
    if not st:
        return None, "BTF 中没有 %s" % struct_name
    for name, off, _ in st["members"]:
        if name == field:
            return off, None
    if anon_offset is not None:
        for name, off, _ in st["members"]:
            if not name and off == anon_offset:
                return off, None
    return None, "%s 中找不到 %s" % (struct_name, field)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--btf", help="vmlinux BTF 文件路径")
    ap.add_argument("--device", action="store_true", help="从 adb 设备自动取 BTF")
    ap.add_argument("--out", default="cr_offsets.h")
    ap.add_argument("--print-only", action="store_true")
    a = ap.parse_args()

    if a.device:
        tmp = "/data/local/tmp/vmlinux.btf"
        subprocess.run(["adb", "shell",
                        "su -c 'dd if=/sys/kernel/btf/vmlinux of=%s 2>/dev/null; chmod 0666 %s'"
                        % (tmp, tmp)], check=True)
        local = "vmlinux.btf"
        subprocess.run(["adb", "pull", tmp, local], check=True)
        a.btf = local
    if not a.btf or not os.path.exists(a.btf):
        raise SystemExit("需要 --btf 或 --device")

    structs = parse_btf(open(a.btf, "rb").read())
    pde = structs.get("proc_dir_entry")
    pops = structs.get("proc_ops")
    if not pde or not pops:
        raise SystemExit("BTF 缺 proc_dir_entry 或 proc_ops")

    # 5.6+ 的 proc_dir_entry 用匿名 union 承载 proc_ops。该 union 里 proc_ops 紧跟
    # proc_iops（指针，8 字节）之后，所以优先由 proc_iops 推导；proc_iops 缺失时
    # 退回"第一个匿名成员"——两种内核布局下都只落在 proc_ops 上。
    # （proc_dir_entry 里另有 write 的匿名 union，位于 proc_dops 之后，不能选中它。）
    mem = dict((n, o) for n, o, _ in pde["members"])
    if "proc_iops" in mem:
        proc_ops_off = mem["proc_iops"] + 8
    else:
        anon = sorted(o for n, o, _ in pde["members"] if not n)
        if not anon:
            raise SystemExit("proc_dir_entry 里找不到 proc_ops 落点，布局异常")
        proc_ops_off = anon[0]
    if proc_ops_off in mem.values():
        raise SystemExit("推导出的 proc_ops 偏移与具名字段冲突: %d" % proc_ops_off)

    def need(field, anon_off=None):
        v, err = resolve(structs, "proc_dir_entry", field, anon_off)
        if err:
            raise SystemExit(err)
        return v

    def need_pop(field):
        v, err = resolve(structs, "proc_ops", field)
        if err:
            raise SystemExit(err)
        return v

    table = [
        ("CR_PDE_SIZE",      pde["size"]),
        ("CR_PDE_PROCOPS",   proc_ops_off),
        ("CR_PDE_PARENT",    need("parent")),
        ("CR_PDE_SUBDIR",    need("subdir")),
        ("CR_PDE_SUBDIRNODE", need("subdir_node")),
        ("CR_PDE_NAME",      need("name")),
        ("CR_PDE_MODE",      need("mode")),
        ("CR_PDE_FLAGS",     need("flags")),
        ("CR_PDE_NAMELEN",   need("namelen")),
        ("CR_POPS_SIZE",     pops["size"]),
        ("CR_POPS_OPEN",     need_pop("proc_open")),
        ("CR_POPS_READ",     need_pop("proc_read")),
        ("CR_POPS_READITER", need_pop("proc_read_iter")),
        ("CR_POPS_WRITE",    need_pop("proc_write")),
        ("CR_POPS_LSEEK",    need_pop("proc_lseek")),
        ("CR_POPS_RELEASE",  need_pop("proc_release")),
    ]

    w = max(len(k) for k, _ in table)
    lines = [
        "/* 生成文件 —— 不要手改。",
        " * 来源: %s" % os.path.basename(a.btf),
        " * 生成: tools/gen_offsets.py",
        " *",
        " * 这些偏移取自目标内核自己的 BTF，不同内核版本会不同。",
        " * 换内核后必须重新生成，否则模块会按错误布局读写内核内存。",
        " */",
        "#ifndef CR_OFFSETS_H",
        "#define CR_OFFSETS_H",
        "",
    ]
    for k, v in table:
        lines.append("#define %-*s %du" % (w, k, v))
    lines += ["", "#endif /* CR_OFFSETS_H */", ""]
    text = "\n".join(lines)

    if a.print_only:
        sys.stdout.write(text)
        return
    open(a.out, "w", encoding="utf-8", newline="\n").write(text)
    print("已生成 %s（%d 项）" % (a.out, len(table)))
    for k, v in table:
        print("  %-18s %d" % (k, v))


if __name__ == "__main__":
    main()
