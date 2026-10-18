# adapt-new-device.sh

给**任意一台** Android 设备产出可加载的 `cpuinfo_redirect.ko`。

## 只需要两样东西

| 材料 | 用途 | 怎么拿 |
|---|---|---|
| `uname -r` | 构造 `LOCALVERSION`，让 vermagic 精确匹配 | `adb shell uname -r` |
| `vmlinux.btf` | 推导 `proc_dir_entry` 偏移（PROC 后端必需） | `adb shell su -c 'dd if=/sys/kernel/btf/vmlinux of=/sdcard/vmlinux.btf'` |

**不需要**：OEM 完整源码树、`Module.symvers`、厂商固件包、设备参与编译。

## 用法

```bash
# 采集（在目标设备上）
adb shell uname -r > uname_r.txt
adb shell su -c 'dd if=/sys/kernel/btf/vmlinux of=/sdcard/vmlinux.btf'
adb pull /sdcard/vmlinux.btf

# 编译（任意 x86_64 Linux / WSL2 / CI）
./tools/adapt-new-device.sh "$(cat uname_r.txt)" vmlinux.btf

# 产物
ls dist/cpuinfo_redirect-<uname-r>.ko
```

## 它自动处理的事

1. **解析版本** → 选 GKI 分支（5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18）
2. **clone 源码**（或复用 `--tree` 指定的）
3. **打五个补丁**：
   - `elf_compat.h`（ELF 宏互递归）
   - `sorttable.c` 注入
   - 摘 `asn1_compiler`（bionic 头污染）
   - `modpost.h` 补 `ELF_ST_*` + 41 个 ARM 重定位常量
   - `ld-version.sh` shebang
4. **判定后端**：源码树有 `trace/hooks/cpuinfo.h` 走 RVH，否则 `proc_ops`
5. **生成偏移表**（PROC 后端，从 BTF）
6. **`--no-crc` 编译**：藏 `Module.symvers`，产出空 `__versions`，绕开 CRC 校验
7. **修补 `.mod.c`** 的空初始化（`= ;` 语法错误）后重链
8. **校验 vermagic** 前缀是否等于目标 `uname -r`

## 选项

```
--out DIR        产物目录（默认 dist）
--tree DIR       复用已有源码树，不重新 clone
--keep-crc       保留 CRC（产物只在同固件上可用，一般不用）
--force-rvh      强制 RVH（源码树没 hook 时报错）
```

## 退出码与错误

| 情况 | 行为 |
|---|---|
| 5.5–5.9 | 直接报错——Android GKI 从 5.10 开始，没有对应分支 |
| 其他未知版本 | 报错并列出支持的版本 |
| PROC 后端缺 BTF | 报错并提示如何从设备 dd 出来 |
| clone 失败 | 提示改用 `github.com/aosp-mirror/kernel_common` |

## 部署与验证

脚本跑完会打印：

```bash
adb push dist/cpuinfo_redirect-6.6.89-xxx.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/cpuinfo_redirect-6.6.89-xxx.ko'
adb shell 'grep -m1 "CPU implementer" /proc/cpuinfo'   # 0x48=Kirin生效 0x51=未生效
adb shell su -c 'dmesg | grep cpuinfo_redirect | tail -3'
```

## 已知限制

**PROC 后端在未见过的内核上仍有崩溃风险**。`gen_offsets.py` 从 BTF 推导的偏移是准的，
但代码只用到 `CR_PDE_PROCOPS` 和 `CR_POPS_*` 两组——如果目标内核这两组的语义有变（例如
`proc_ops` 增加新字段导致后续偏移前移），产物会加载成功但读写错误位置。

**建议**：首次部署前先在测试设备上 `insmod` 并立刻读 `/proc/cpuinfo`，确认无 oops 再长期使用。
