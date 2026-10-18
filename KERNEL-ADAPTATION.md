
# 内核模块跨版本适配 —— 深度分析

综合 `gs.awesome.rip` 上「Android 内核驱动开发」分类及相关搜索结果，结合本工程的实测数据。

## 一、问题的真实结构：五个维度，不是一个问题

「为什么一份 .ko 装不到所有设备」不是单一问题。内核加载一个模块要连过五关：

| # | 关卡 | 检查者 | 数据来源 | 能否绕过 |
|---|---|---|---|---|
| 1 | **架构/字节序** | `elf_check_arch` | 编译目标 | 不需要（都是 ARM64 LE） |
| 2 | **vermagic** | `check_modinfo` | 编译时的 `LOCALVERSION` | **编译时可对齐** |
| 3 | **符号 CRC** | `check_version` | `Module.symvers` | **`--no-crc` 或 patch** |
| 4 | **签名** | `module_sig_check` | `CONFIG_MODULE_SIG_FORCE` | 多数设备没开 |
| 5 | **结构布局** | 无（运行时才崩） | BTF / 源码 | **必须精确** |

前四关都是「加载时能不能过」，第五关是「过了以后会不会崩」——**第五关才最致命**。

## 二、逐关分析

### 关卡 2：vermagic —— 编译时可完全对齐

vermagic 由三部分组成：

```
vermagic = <KERNEL_VERSION> + <LOCALVERSION 后缀> + " " + <flags>
例:  6.6.89              -android15-8-g7e1f3c083cc6-abogki467167594-4k  SMP preempt mod_unload modversions aarch64
     └─ 源码 Makefile ─┘ └────────── LOCALVERSION ──────────┘  └──── 由 CONFIG 决定 ────┘
```

**关键结论：这三段全都可以在编译时精确指定。**

```bash
make -C $KDIR M=$PWD/src \
    ARCH=arm64 LLVM=1 LLVM_IAS=1 \
    LOCALVERSION="-android15-8-g7e1f3c083cc6-abogki467167594-4k" \
    modules
```

不需要目标设备的源码树。标准 GKI 分支 + 正确的 `LOCALVERSION` 就能产出完全一致的 vermagic 字符串。
`scripts/setlocalversion` 里那个 `g<commit>` 是 git 自动生成的，但 `LOCALVERSION` 会**追加在它后面**——
所以更稳妥的做法是编译时把 git commit 部分也固定掉（用 `scripts/setlocalversion` 的 `--no-local` 或直接设 `LOCALVERSION` 覆盖）。

本工程的 `tools/build-for-device.sh` 已经这么做了：

```bash
KV=${UNAME_R%%-*}          # 6.6.89
LOCALVER=${UNAME_R#"$KV"}  # -android15-8-g7e1f3c083cc6-abogki467167594-4k
MAKEVARS+=("LOCALVERSION=$LOCALVER")
```

### 关卡 3：符号 CRC —— 两种解法

`CONFIG_MODVERSIONS=y` 时，`.ko` 的 `__versions` 段记录每个导入符号的 CRC，加载时逐个比对。

**解法 A：`--no-crc`（本工程采用）**

编模块时把 `Module.symvers` 藏起来 + `KBUILD_MODPOST_WARN=1`，产出**空的 `__versions` 段**。
内核读不到 CRC 时不报错，只警告 `no symbol version for X`，照常加载。

```bash
mv $OUTD/Module.symvers $OUTD/Module.symvers.bak
make ... KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n modules
```

代价：ABI 不匹配不会在加载期暴露，会推迟到运行时崩溃。

**解法 B：patch `check_version`（AndroidDriveSignity 的做法）**

把函数改成 `mov w0,#1; ret`（返回 1 = 校验通过）。需要改 boot.img，风险高。

### 关卡 4：签名 —— 大多数设备没开

`CONFIG_MODULE_SIG_FORCE` 未设时，签名只验证不强制。本机实测：

```
CONFIG_MODULE_SIG_FORCE 未出现  →  不强制
```

所以这一关通常不需要处理。真要处理就 patch `module_sig_check`。

### 关卡 5：结构布局 —— 唯一无法在编译时绕过的一关

这是真正的难点。`PROC` 后端需要知道 `proc_dir_entry` 和 `proc_ops` 的精确字段偏移，
而 `proc_dir_entry` 带 `__randomize_layout`：

```c
struct proc_dir_entry { ... } __randomize_layout;
```

| 情况 | 布局 | 偏移可否从源码推导 |
|---|---|---|
| `CONFIG_RANDSTRUCT=n` | 按声明顺序固定 | 可以 |
| `CONFIG_RANDSTRUCT=y` | 编译时随机打乱 | **不行，只能读 BTF** |

本机实测 `CONFIG_RANDSTRUCT` 未出现（即关闭），所以布局固定。

**但即使布局固定，各版本的字段也可能不同**：

| 字段 | 6.1 / 6.6 | 6.18 |
|---|---|---|
| `proc_iops` | 40 | 40 |
| **`proc_ops`** | **48** | **48** |
| `proc_dops` | **56** | **不存在** |
| `seq_ops` | 64 | 56 |
| `parent` | 120 | 112 |
| `name` | 160 | 152 |

好消息：`CR_PDE_PROCOPS` 和 `CR_POPS_*` 在这些版本上**恰好一致**，
而本工程的代码只用这两组——所以六个版本能共用一份 `cr_offsets.h`。

**但这是巧合，不是保证。** 换内核必须重新生成：

```bash
python3 tools/gen_offsets.py --btf <目标内核的 vmlinux BTF> --out src/cr_offsets.h
```

## 三、RVH 后端 vs PROC 后端

| | RVH | PROC |
|---|---|---|
| 原理 | 注册受限 vendor hook 回调 | 替换 `proc_ops->proc_read` |
| 需要结构偏移 | **不需要** | **需要，且必须精确** |
| 需要目标符号 | `android_rvh_cpuinfo_c_show` | `cpuinfo_proc_ops` 或偏移推导 |
| 崩溃风险 | 低 | **高**（偏移错一位就 oops） |
| 覆盖面 | 仅厂商内核（6.6+ 常见） | 5.6+ 全部 |

**这是核心取舍**：

- **RVH 安全但覆盖窄**——`android12-5.10`/`android13-5.15`/`android14-6.1`/`android17-6.18` 的公开 GKI 树**都没有** `trace/hooks/cpuinfo.h`
- **PROC 覆盖广但危险**——需要逐内核验证偏移

本工程的选择：**有 RVH 就走 RVH，没有就 PROC**，并用 `static_assert` 在编译期卡住影子结构尺寸。

## 四、本工程的六个产物，实际可信度

| 内核 | vermagic 是否匹配真实设备 | CRC | 结构偏移 | 综合可信度 |
|---|---|---|---|---|
| 5.10 | 否（标准 GKI） | 空（`--no-crc`） | 6.6 的，未验证 | **低** |
| 5.15 | 否 | 空 | 同上 | **低** |
| 6.1  | 否 | 空 | 同上 | **低** |
| **6.6** | **是**（设备实测） | 设备自有 | **设备 BTF 推导** | **高** |
| 6.12 | 否 | 空 | 同上 | **低** |
| 6.18 | 否 | 空 | 同上 | **低** |

**除 6.6 外，其余五个都不是「能装」的产物，而是「能编」的证明。**

## 五、真正可用的跨设备流程

综合以上，如果要给**一台新设备**产出可用的 `.ko`，只需要两样东西：

### 必需材料

| 材料 | 用途 | 从哪拿 |
|---|---|---|
| `uname -r` | 构造 `LOCALVERSION` | `adb shell uname -r` |
| `/sys/kernel/btf/vmlinux` | 推导结构偏移 | `adb pull`（需 root） |

**不需要**：完整 OEM 源码树、`Module.symvers`、厂商固件包。

### 流程

```bash
# 1. 采集（设备上）
adb shell su -c 'uname -r'
adb shell su -c 'dd if=/sys/kernel/btf/vmlinux of=/data/local/tmp/vmlinux.btf'
adb pull /data/local/tmp/vmlinux.btf

# 2. 生成偏移表
python3 tools/gen_offsets.py --btf vmlinux.btf --out src/cr_offsets.h

# 3. 编译（本地或 CI，任意 x86_64 Linux）
KVER_FULL="$(cat uname_r.txt)"
KV=${KVER_FULL%%-*}
LOCALVER=${KVER_FULL#"$KV"}
git clone --depth 1 -b android15-6.6 https://android.googlesource.com/kernel/common ktree
make -C ktree O=out ARCH=arm64 LLVM=1 defconfig modules_prepare
mv out/Module.symvers out/Module.symvers.bak        # 空 __versions
make -C out M=$PWD/src ARCH=arm64 LLVM=1 \
     LOCALVERSION="$LOCALVER" \
     KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n modules
```

产物就是那台设备能加载的模块。**全程不需要设备参与编译。**

## 六、搜索中发现的、比本工程更彻底的做法

### 1. 内核直接写属性物理页（`propedit`）

来源：`systemnb/android-kernel-hacking-toolkit`

Android 属性存在共享内存 `/dev/__properties__/`，每个属性是一个 `prop_info`（含 `serial` + `value`）。

```
用户态：扫描 /proc/self/maps 定位属性区 → 暴力搜索目标 key → 拿到 value 的虚拟地址
内核：  页表遍历 VA→PA → ioremap_cache 映射 → 直接写物理页 → unmap
```

**优势**：不经过 syscall，不触发 SELinux 检查，`serial` 字段不动所以属性系统自洽。
**比 `resetprop` 彻底一个量级**——`resetprop` 是用户态工具，native 层或 sysfs 直读可能绕过。

### 2. CFI 绕过（所有现代内核都需要）

```c
__attribute__((no_sanitize("cfi")))
static int my_call_original(...) { return orig_fn(...); }
```

通过函数指针调用原函数时，不加这个属性会触发 CFI panic。本工程的 RVH 回调**不经过函数指针**，所以暂时不需要。

### 3. `kallsyms_lookup_name` 在 ≥5.7 不导出

标准解法是用 **kprobe** 挂上去拿地址：

```c
static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
register_kprobe(&kp);
kallsyms_lookup_name_fn = (void *)kp.addr;
unregister_kprobe(&kp);
```

本工程用了 RVH 的导出符号，没走这条路；如果要 hook `show_cpuinfo` 这类非导出函数就必须用。

### 4. mmuhack —— 关闭内核只读内存保护

`sys_call_table` 等内核只读数据受 PTE 保护。绕过方式：

```
1. 定位 init_mm
2. 遍历页表拿目标地址的 PTE
3. pte_mkwrite() 改成可写
4. 改完 pte_wrprotect() 恢复
```

## 七、结论

**「适配所有内核」的正确表述是：针对每个目标内核，用 15 分钟生成一份产物。**

不是「一份产物到处跑」——最后那句是做不到的，因为结构布局必然随内核演进而变。

**能做到的最好程度**：

1. **vermagic** —— 编译时 100% 对齐（只要知道 `uname -r`）
2. **CRC** —— `--no-crc` 完全绕过
3. **签名** —— 多数设备不需要处理
4. **结构偏移** —— 一次 `gen_offsets.py` + BTF，自动生成

**唯一拿不到的东西**：有 `CONFIG_RANDSTRUCT=y` 且不提供 BTF 的内核。这种情况下无法推导布局，只能放弃 PROC 后端。

**本工程当前状态**：流程完整、工具齐全、六个版本实编通过，但只有 6.6 做过真机验证。
