# plq110-cpu-crossver — /proc/cpuinfo 伪装（5.6 – 6.12）

原 `cpuinfo_redirect` 只针对 6.6.89 一个内核编译，README 里也写明了它依赖
`android_rvh_cpuinfo_c_show`——而这个 vendor hook **是 android15-6.6 才引入的**。
所以「移植到 5.5–6.12」并不是改几个 API 调用，而是要在没有这个 hook 的内核上
换一条完全不同的实现路线。本工程就是做这件事。

## 两条后端

| 后端 | 适用 | 机制 | 是否碰内核私有结构 |
|---|---|---|---|
| **RVH** | 6.6+，且 `CONFIG_ANDROID_VENDOR_HOOKS=y` 且内核有 cpuinfo hook | 注册 `android_rvh_cpuinfo_c_show`，改写 seq_file 输出 | **否** |
| **PROC** | 5.6 – 6.5 | 在 init 的 proc 红黑树里找到 `/proc/cpuinfo` 的 `proc_dir_entry`，替换 `proc_ops` 指针 | 是（依赖布局） |

编译期自动二选一：`__has_include(<trace/hooks/cpuinfo.h>)` + `CONFIG_ANDROID_VENDOR_HOOKS`。

## 版本支持矩阵

| 内核 | RVH hook | `struct proc_ops` | 本工程可用后端 | 结论 |
|---|---|---|---|---|
| 5.5 | ✗ | ✗（还是 `file_operations`） | 无 | **不支持**，见下方说明 |
| 5.6 – 6.5 | ✗ | ✓ | PROC | 可用，但需要目标内核 BTF |
| 6.6 – 6.12 | ✓ | ✓ | **RVH** | 推荐，最稳 |

**5.5 为什么不行**：`proc_ops` 结构体是 5.6 引入的，5.5 的 `proc_dir_entry` 里放的是
`file_operations`。本工程没有为那个布局写第三套后端，而且 5.5 早已不在 GKI 支持范围。
**5.5 上请直接用 `cpu-viewctl` 的 bind mount 方案**——它不需要内核模块，反而更稳。

## PROC 后端为什么能跨版本

`proc_dir_entry` 是内核私有结构，字段偏移每个版本都可能不同。原实现把这些偏移
写死成 6.6.89 的实测值，换内核就得手工重算，算错就是内核 oops。

本工程改成**从目标内核自己的 BTF 推导**：

```sh
# 设备必须有 /sys/kernel/btf/vmlinux（CONFIG_DEBUG_INFO_BTF=y）
python tools/gen_offsets.py --device --out src/cr_offsets.h
```

生成的 `cr_offsets.h` 里是纯常量，编译期再用 `BUILD_BUG_ON` 核对一遍。
布局对不上就编译失败，不会带病加载。

在 6.6.89 上实测，BTF 推导值与原先硬编码值**逐项一致**：

```
proc_dir_entry size=176   parent=120  subdir=128  subdir_node=136
                name=160  mode=168    flags=170   namelen=171
proc_ops        proc_open=8  proc_read=16  proc_write=32  proc_release=48
```

## 构建

```sh
# 1. 生成偏移表（换内核后必须重做）
python tools/gen_offsets.py --device --out src/cr_offsets.h

# 2. 用目标内核源码树编译
make KERNEL_SRC=/path/to/kernel ARCH=arm64 \
     CROSS=aarch64-linux-gnu- OUT=build/6.1 \
     LOCALVERSION=-android14-11-xxxx
```

**KERNEL_SRC 必须与设备内核同源**，否则 vermagic 对不上，`insmod` 直接拒绝（`Invalid module format`）。
`LOCALVERSION` 决定 vermagic 里的版本串，值取 `uname -r` 去掉主版本号后的部分，
例如 `6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k` → `-android15-8-g7e1f3c083cc6-abogki467167594-4k`。

## 三道关（沿用原 README 的框架，逐项给出判断方法）

**第一关：有没有 RVH**

```sh
su -c 'grep -c android_rvh_cpuinfo /proc/kallsyms'
```

>0 就用 RVH，等于 0 就落到 PROC。这一关决定走哪条后端，编译期自动判。

**第二关：Module.symvers**

`CONFIG_MODVERSIONS=y` 时模块里每个导入符号都带 CRC，必须和内核一致。
拿到目标内核源码树时它就在 `Module.symvers`；没有源码树时可以从同版本厂商模块的
`__versions` 段反推。

**第三关：布局（仅 PROC 后端）**

`gen_offsets.py` 已经解决。若目标内核没开 `CONFIG_DEBUG_INFO_BTF`，只能退回人工，
用 `pahole -C proc_dir_entry vmlinux` 或从厂商模块反汇编推导——**这就是为什么 RVH 后端更值得优先**。

## 控制接口（两后端一致）

```sh
M=/data/adb/modules/plq110-spoof-suite

# 状态
cat /proc/cpuinfo_redirect

# 写入伪装内容（le64 长度 + 载荷）
printf '%-8s' ... # 或用模块自带的 cpuinfo-write 工具
cat cpuinfo.txt | $M/payload/cpuinfo-write

# 停用（恢复真实输出；彻底移除需重启）
echo OFF > /proc/cpuinfo_redirect
```

## 已知限制

- **没有 `module_exit`**：RVH 不可反注册，PROC 后端替换后卸载会留悬挂指针。
  `OFF` 只停用，移除必须重启。这是刻意的。
- **PROC 后端依赖布局**：内核换了字段顺序（或开了 `CONFIG_RANDSTRUCT_FULL`）就会失败。
  失败是安全的（编译期 `BUILD_BUG_ON` 或运行期找不到条目），不会静默改错内存。
- **5.5 不支持**，理由见矩阵。
- **未在本机构建验证**：本目录只有源码与工具，编译需要目标内核源码树。

## 编译实测（2026-10-18）

在本机 6.6.89 设备上实际编译过，结果：

| 后端 | 产物 | 大小 | vermagic |
|---|---|---|---|
| RVH | `build/cpuinfo_redirect-rvh.ko` | 197,880 B | `6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k SMP preempt mod_unload modversions aarch64` |
| PROC（强制） | `build/cpuinfo_redirect-proc.ko` | 203,840 B | 同上 |

RVH 版已刷入设备并验证：`version=4.0`、`backend=kernel`、`/proc/cpuinfo` 16 线程 / 6827 B。
后端选择经 `readelf` 确认：RVH 版引用 `android_rvh_cpuinfo` 符号 1 处，PROC 版 0 处。

### 编译中修掉的真实缺陷

这两个只有编译才能发现，纯读代码看不出来：

1. **`cr_pde.subdir` 类型错**：原本写成 `void *`，但它是 `rb_root`（子项红黑树根，+128），
   而 `subdir_node` 才是本条目挂父树的节点（`rb_node`，+136）。混用导致 `rb_first()` 类型不匹配。
2. **`cr_pops.read` 声明成 `void *`**：掉回原实现时要直接调用它，`void *` 不可调用。
   改成 `typedef ssize_t (*cr_read_fn)(struct file *, char __user *, size_t, loff_t *)`。

## 在 Android 设备上编译 5.6–6.12 主线的结果（结论：不可行）

试过在手机（Termux + clang 21）上编译主线 5.10，逐项解决后仍卡死：

| 问题 | 处理 | 结果 |
|---|---|---|
| `tar` 缺 `xz` | 用 Termux 的 `xz` | 通过 |
| `ld-version.sh` 用 `#!/usr/bin/awk` | 改 shebang 到 `/system/bin/awk` | 通过 |
| bionic 无 `bcmp` | `-Dbcmp=memcmp` | 通过 |
| `ELF32/64_ST_TYPE` 未定义 | 补宏到 `sorttable.c` 与 `sorttable.h` | **仍失败** |
| `sorttable` 无条件构建 | 关 `CONFIG_BUILDTIME_TABLE_SORT` | **仍构建** |

根因是 **glibc 与 bionic 的不兼容**：内核 host 工具按 glibc 编写，
强行引入内核 uapi 头又会和 bionic 的 `sys/types.h` 冲突（`int64_t` 未定义、`__private` 报错）。
这是 libc 层的问题，不是参数能绕过的。

**结论**：5.10/5.15/6.1/6.6/6.12 的 `.ko` 必须在 x86_64 Linux（或 WSL2 / GitHub Actions）上交叉编译：

```bash
sudo apt install -y build-essential bc flex bison libssl-dev libelf-dev gcc-aarch64-linux-gnu
git clone --depth 1 -b android14-6.1 https://android.googlesource.com/kernel/common kernel-6.1
make -C kernel-6.1 O=out-6.1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig modules_prepare
python tools/gen_offsets.py --btf <目标设备BTF> --out src/cr_offsets.h
make -C out-6.1 M=$PWD/src ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

**并且**：`CONFIG_MODVERSIONS=y` 要求每个导入符号的 CRC 与目标内核一致，
`Module.symvers` 必须来自目标设备的那个具体内核（从 `/vendor/lib/modules/*.ko` 的 `__versions` 段反推，
或从同源内核树生成）。拿不到就装不上——这正是运行时采用三级降级的原因。


## 全版本适配：一设备一编的流水线

「适配所有内核版本」的真相是 **一设备一内核一份产物**，不是一次编译到处跑。
`RT hook驱动.zip` 里那 22 个 prebuilt `.ko` 就是证据——22 个内核的产物对应 22 个不同的目标内核。

### 1. 设备侧采集

```sh
su -c 'sh /sdcard/collect-for-build.sh'
# 产物: /sdcard/kx.tgz —— 回传给编译方
```

采集：`uname -r`、`/proc/config.gz`、`kallsyms`（判 hook 点）、厂商 `.ko`（CRC 来源）、
**`/sys/kernel/btf/vmlinux`**（偏移来源，关键）、boot 分区表。全程只读，
唯一改动是临时把 `kptr_restrict` 置 0 取真地址后立刻还原。

### 2. 编译

```bash
./tools/build-for-device.sh kx.tgz
# 解析内核版本 → 选 GKI 分支 → clone → modules_prepare
# → 提取 CRC → 生成偏移 → 编译 → 校验 vermagic
# 产物: dist/cpuinfo_redirect-<uname-r>.ko
```

脚本自动判断后端：`kallsyms` 里有 `android_rvh_cpuinfo` 走 RVH，没有就加
`-DCR_FORCE_PROC=1` 走 proc_ops。

### 3. CI（没有 Linux 机器时）

把 `kx.tgz` 放进仓库根目录，手动触发 `.github/workflows/build.yml`，下载 artifact。
Ubuntu runner 是 glibc 环境，不会遇到 Android/bionic 那堆 host 工具问题。

### 各步骤的数据来源

| 需要 | 从哪来 |
|---|---|
| vermagic | `uname -r` → `00_env.txt` |
| 页大小 | `CONFIG_ARM64_xxK_PAGES` → `03_config_key.txt` |
| CRC 校验 | 厂商 `.ko` 的 `__versions` 段 → `ko/` |
| 结构体偏移 | `/sys/kernel/btf/vmlinux` → `vmlinux.btf` |
| 后端选择 | `kallsyms` 里的 RVH 符号 → `01_syms.txt` |
| 编译本身 | x86_64 Linux + 交叉工具链 |

### 已知卡点

**CRC 需人工整理**：脚本把 `__versions` 段 dump 到 `device-crc.txt`，但转成标准
`Module.symvers`（`0x<crc>\t<sym>\tvmlinux\tEXPORT_SYMBOL`）目前要手工做——
段内是紧凑的 `{u32 crc, u32 nameoff}` 数组，需配合字符串表解析。
若目标内核 `CONFIG_MODVERSIONS` 未开，这关直接不存在。

**无 BTF 且无 RVH hook** 时 proc_ops 后端会因 `static_assert` 编译失败，
这是刻意设计：宁可编不过，也不要按错误布局读写内核内存。

### 已验证组合

| 设备 | 内核 | 后端 | 状态 |
|---|---|---|---|
| PLQ110 (SM8750) | 6.6.89-android15-8 | RVH | **已刷入，version=4.0，active=1** |
| 同上（强制 PROC） | 同上 | proc_ops | 编译通过，未加载测试 |

### 与 CPU 伪装层的关系

本工程只负责 `/proc/cpuinfo`。CPU 线程数必须与 `/sys/devices/system/cpu/present`
一致：设备是 8 核就伪装 8 条 processor，改成 16 条会与 sysfs 矛盾，
而用 bind mount 去改 sysfs 位图会在 `/proc/mounts` 里留下 `f2fs` 挂载痕迹（实测确认）。


## 构建路径升级：Bazel + 无 CRC

### Bazel（GKI 官方方式）

Android 13 及以后的内核用 Bazel，不再是 `make defconfig`：

```bash
cd kernel-6.6
tools/bazel build //common:kernel_aarch64 //common:kernel_aarch64_modules_prepare
BAZEL_OUT=$(readlink -f bazel-bin/common/kernel_aarch64)
# modules_prepare 的产物是 tar.gz，要解压到输出目录
tar -xzf bazel-bin/common/kernel_aarch64_modules_prepare/modules_prepare_outdir.tar.gz -C "$BAZEL_OUT"

make -C kernel-6.6/common O="$BAZEL_OUT" M=$PWD/src \
     ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
     CONFIG_DEBUG_INFO_BTF_MODULES= LOCALVERSION=... modules -j$(nproc)
```

`build-for-device.sh` 现在会自动判断：内核树里有 `tools/bazel` 就走 Bazel，否则退回 legacy。

### 无 CRC 构建（`--no-crc`）—— 绕开 Module.symvers

**这是适配全版本的关键**。原理：

`CONFIG_MODVERSIONS=y` 时，Kbuild/modpost 会从输出目录的 `Module.symvers` 读符号 CRC
写进 `.ko` 的 `__versions` 段，内核加载时逐个比对。**但内核只对『带 CRC 的符号』做强制校验**——
没有 CRC 的符号只报 `no symbol version for X` 警告，**不拒绝加载**。

于是：隐藏 `Module.symvers` + `KBUILD_MODPOST_WARN=1` → 产出 `__versions` 为空的模块 →
加载门槛只剩 **vermagic**。

```bash
./tools/build-for-device.sh kx.tgz --no-crc
```

脚本内部做的是：

```bash
mv "$OUTD/Module.symvers" "$OUTD/Module.symvers.no_crc_bak.$$"
make ... KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n modules
mv 回来   # 编译完还原
```

**代价**：ABI 不匹配不会在加载期被拒，而是在运行时才可能崩。所以**必须确保源码与目标内核 API 兼容**——
我们这份源码只用了 `proc_create`/`seq_read`/`RCU`/`proc_ops` 这些长期稳定的接口，风险可控。

### 门槛对比

| 方式 | 需要 Module.symvers | 加载成功率 |
|---|---|---|
| 标准 CRC 构建 | **是**（每台设备单独提取） | 高，但拿不到就装不上 |
| `--no-crc` | **否** | 中，vermagic 对就能试 |

拿不到目标设备符号表时，`--no-crc` 是唯一能让模块动起来的路。

### 隐藏性现状（待处理）

模块**不产生任何挂载**，`/proc/mounts` 干净，所以 8 线程/16 线程的自洽问题**不是**首要暴露点。

但当前仍有 `/proc/cpuinfo_redirect` 控制节点——一个自定义 `/proc` 条目比数据不一致显眼得多。
彻底隐藏需要把控制通道换成共享内存或隐藏的设备节点，属于另一项工作。


## 五内核版本产物（实测编译通过）

以下五个 `.ko` 均在设备上从对应 GKI 源码树实际编译产出，非预置文件：

| 内核 | 大小 | vermagic | CRC 校验 |
|---|---|---|---|
| 5.10 | 239,320 | `5.10.245 SMP preempt mod_unload aarch64` | 无 modversions |
| 5.15 | 234,360 | `5.15.194 SMP preempt mod_unload aarch64` | 无 modversions |
| 6.1  | 245,592 | `6.1.155 SMP preempt mod_unload aarch64` | 无 modversions |
| 6.6  | 190,072 | `6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k ... modversions` | **与目标设备完全匹配** |
| 6.12 | 255,368 | `6.12.52 SMP preempt mod_unload aarch64` | 无 modversions |

注意：除 6.6 外，其余四个的 vermagic 是**标准 GKI 内核**的，不带厂商后缀。
要装到真实手机上，vermagic 必须与那台设备的 `uname -r` 一致 —— 用 `build-for-device.sh` + 该设备的 `kx.tgz` 重新编。

## 设备端编译实录：五个必须打的补丁

在 Android/bionic 环境里编 GKI 模块，源码本身要打五处补丁。这些不是可选项，缺一个就编不动：

| # | 位置 | 问题 | 处理 |
|---|---|---|---|
| 1 | `scripts/sorttable.c` | `<elf.h>` 的 `ELF64_ST_TYPE` 与内核宏互递归 | 注入 `#include "elf_compat.h"` 覆盖为 `(x) & 0xf` |
| 2 | `scripts/Makefile` | `asn1_compiler` 被 bionic 的 `bits/wait.h` 拖进内核头污染链 | 从 `hostprogs-always` 摘掉 |
| 3 | `scripts/mod/modpost.h` | 同上 ELF 宏问题，导致 modpost 段错误 | 直接展开 `ELF_ST_BIND/ST_TYPE`（`R_SYM/R_TYPE` 32/64 位宽不同，必须保留原定义） |
| 4 | `scripts/ld-version.sh` | 5.10 用 `#!/usr/bin/awk -f`，Android 无此路径 | 改 `#!/system/bin/awk -f` |
| 5 | `*.mod.c`（生成物） | 无 `Module.symvers` 时 `__version_ext_names = ;` 是语法错误 | awk 修补为 `= "";` 后重试链接 |

另外两处源码级兼容：

- `kvmalloc/kvfree` 需要 `#include <linux/mm.h>`（只 include `slab.h` 在 5.15 上不导出）
- `asm/unaligned.h` 在 6.12 并入 `linux/unaligned.h`，用 `__has_include` 兼容

### 关于 modpost 段错误

最初把 `ELF_R_SYM/ELF_R_TYPE` 也一并改成 32 位语义（`>> 8` / `& 0xff`），导致 modpost 解析重定位表越界崩在 `Segmentation fault`。
正确做法：**只改 `ELF_ST_*`（32/64 语义相同），`ELF_R_*` 保留内核原本的 `ELF32_/ELF64_` 定义**。


## 隐藏能力的边界（实测结论）

模块能做到的隐藏：

| 项目 | 状态 | 手段 |
|---|---|---|
| `lsmod` | 已隐藏 | `list_del_init(&THIS_MODULE->list)` |
| `/proc/modules` | 已隐藏 | 同上（两者遍历同一链表） |
| `/proc` 控制节点 | 已消除 | 载荷编进 .ko，默认不建节点（`CR_CTL_NODE` 未定义） |
| 挂载痕迹 | 无 | 内核模块方案不产生任何 mount |
| `/sys/module/cpuinfo_redirect` | **无法消除** | 见下 |
| dmesg | 有 `pr_info` 一行 | 可改 `pr_debug` 消除 |

### 为什么 `/sys/module` 去不掉

两条路都试过，都堵死：

**`kobject_del()`** — 该符号已导出（`__ksymtab` 有），调用后目录确实消失。
但它是模块收尾流程依赖的对象，**实测删掉后整个模块加载失败**（`kallsyms` 无符号、`dmesg` 无 `cr_init` 日志），伪装完全不生效。

**`kobject_rename()` / `sysfs_rename_dir()`** — 思路是只改名字不删对象，绕开上面的副作用。
但实测这两个符号在该内核**只有 kallsyms 条目、没有 `__ksymtab`**，即未 `EXPORT_SYMBOL`，
模块引用会导致 `insmod` 报 `Unknown symbol` 而失败。

```
kobject_del     __ksymtab 条目 = 1   (已导出)
kobject_rename  __ksymtab 条目 = 0   (未导出)
sysfs_rename_dir __ksymtab 条目 = 0  (未导出)
```

结论：**`/sys/module/cpuinfo_redirect` 这个暴露点在模块层面无解**，只能接受。
实际风险中等——`/sys/module/` 下有 809 个条目，需要检测方主动遍历全表做名字匹配才会命中；
而最常被查的 `lsmod` 和 `/proc/modules` 已经是干净的。

## 与其他模块共存

两个模块加载同名的内核模块时，第二次会静默失败：

```
cpuinfo_redirect: module is already loaded
```

所以把职责拆开：

- `cpuinfo-redirect-driver`（provider）：只放 `.ko` 文件，**不加载**
- `plq110-spoof-suite`：唯一加载方，`cpu.sh` 的 `cpu_pick_ko()` 按 `uname -r` 从两边挑文件

## 完整性清单是硬门禁

改动 `payload/` 下任何被清单覆盖的文件后，**必须同步 `payload.sha256`**，
否则开机 `apply.sh` 会拒绝执行整层：

```
ERROR: payload integrity failed (sha256sum -c)
```

症状是「开关是 on 但完全没生效」——因为加载阶段就被跳过了。

