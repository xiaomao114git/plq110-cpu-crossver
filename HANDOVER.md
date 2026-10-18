# 交接文档 — CPU 伪装 / 内核模块工程

> 最后更新：2026-10-18
> 目标设备：OnePlus PLQ110 (Ace 6)，SM8750，Android 16，kernel 6.6.89-android15-8

---

## 1. 这个工程是什么

让 `/proc/cpuinfo` 输出伪装成 Kirin 9050 Pro（16 线程 / CPU part 0xd25 / implementer 0x48），
支持 **5.10 / 5.15 / 6.1 / 6.6 / 6.12** 五个内核版本。

核心设计：**伪装载荷编译进 .ko**，加载即生效。运行时不需要 `/proc` 控制节点、不需要 `/dev`、
不写任何文件、不产生任何挂载。

---

## 2. 目录结构

```
plq110-cpu-crossver/
├── QUICKSTART.md               三步上手（先看这个）
├── COVERAGE.md                 六版本覆盖矩阵、为什么不支持 5.5–5.9
├── KERNEL-ADAPTATION.md        五个兼容关卡的深度分析
├── HANDOVER.md                 本文
├── README.md                   技术记录
├── src/
│   ├── cpuinfo_redirect.c      主模块（RVH + proc_ops 双后端）
│   ├── cr_payload.h            伪装载荷，编译期嵌入（16 线程 Kirin）
│   ├── cr_offsets.h            proc_dir_entry 偏移表（6.6 实测值）
│   └── Kbuild
├── tools/
│   ├── grab-device-info.bat    ★Windows 双击采集（推到设备执行 sh 版）
│   ├── grab-device-info.sh     ★设备端采集逻辑（uname-r + BTF + config）
│   ├── adapt-new-device.sh     ★消费采集包，产出单个 .ko（任意 Linux）
│   ├── update-module.sh        ★消费采集包，产出可刷 zip；无参数=全自动
│   ├── adapt-on-device.sh      ★设备端现场编译（Termux）
│   ├── make-target-env.sh      设备侧补全 config/btf 指纹，升级到六因子匹配
│   ├── build-for-device.sh     旧版：消费完整 kx.tgz
│   ├── collect-for-build.sh    旧版：采集完整 kx.tgz
│   ├── gen_offsets.py          从 BTF 或 vmlinux 生成 cr_offsets.h
│   ├── README-adapt.md         adapt-new-device.sh 细节
│   └── README-update-module.md update-module.sh 细节
├── build/                      六个内核版本的 .ko（只有 6.6 上过真机）
└── .github/workflows/
    ├── build-all.yml           五版本矩阵 CI
    └── build.yml               单设备构建
```

带 ★ 的是 2026-10-18 晚新增的自动化工具链，详见第 15 节。

---

## 3. 五个 .ko 的现状

| 内核 | 大小 | vermagic | CRC | 状态 |
|---|---|---|---|---|
| 5.10 | 239,320 | `5.10.245 SMP preempt mod_unload aarch64` | 无 | 编译通过，未上机 |
| 5.15 | 234,360 | `5.15.194 SMP preempt mod_unload aarch64` | 无 | 编译通过，未上机 |
| 6.1 | 245,592 | `6.1.155 SMP preempt mod_unload aarch64` | 无 | 编译通过，未上机 |
| **6.6** | **190,072** | `6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k ... modversions` | 有 | **实机验证通过** |
| 6.12 | 255,368 | `6.12.52 SMP preempt mod_unload aarch64` | 无 | 编译通过，未上机 |

**重要**：除 6.6 外，其余四份是**标准 GKI 内核**产物，vermagic 不带厂商后缀。
装到真实手机必须 vermagic 完全匹配，否则 `insmod` 报 `invalid module format`。
正确做法是用 `tools/build-for-device.sh` + 目标设备的 `kx.tgz` 重新编。

---

## 4. 两个后端

| 后端 | 条件 | 原理 |
|---|---|---|
| **RVH** | 内核有 `__tracepoint_android_rvh_cpuinfo_c_show`（6.6+ 厂商内核） | 注册受限 vendor hook 回调，改 `seq_file` 输出 |
| **proc_ops** | 无 RVH，但 ≥5.6 | 用 `cr_offsets.h` 定位 `cpu_dir_entry`，替换 `proc_ops` |
| **bind** | 模块编不过/装不上 | `cpu-viewctl` bind 挂载，不需内核模块 |
| **degraded** | 上面都失败 | 只保留属性层，`/proc/cpuinfo` 保持原样 |

5.5 及以下不支持（`proc_ops` 是 5.6 引入）。

---

## 5. 设备端编译：六个必须打的补丁

在 Android/bionic 里编 GKI 模块，源码要改六处，缺一个编不动：

| # | 文件 | 问题 | 处理 |
|---|---|---|---|
| 1 | `scripts/sorttable.c` | bionic 的 `ELF64_ST_TYPE` 与内核宏互递归 | 注入 `#include "elf_compat.h"` |
| 2 | `scripts/Makefile` | `asn1_compiler` 被 `bits/wait.h` 拖进内核头污染链 | 从 `hostprogs-always` 摘掉 |
| 3 | `scripts/mod/modpost.h` | 同 ELF 宏问题 → modpost **段错误** | 只展开 `ELF_ST_BIND/ST_TYPE`；**`ELF_R_SYM/R_TYPE` 必须保留原定义**（32/64 位宽不同） |
| 4 | `scripts/ld-version.sh` | 5.10 用 `#!/usr/bin/awk -f`，Android 无此路径 | 改 `#!/system/bin/awk -f` |
| 5 | `*.mod.c`（生成物） | 无 `Module.symvers` 时 `__version_ext_names = ;` 语法错 | awk 补成 `= "";` 后重试链接 |
| 6 | `src/cpuinfo_redirect.c` | `kvmalloc` 缺 `<linux/mm.h>`；6.12 的 `asm/unaligned.h` 并入 `linux/unaligned.h` | 补 include + `__has_include` 兼容 |

**第 3 条踩过大坑**：最初把 `ELF_R_SYM/ELF_R_TYPE` 也按 32 位语义改成 `>>8` / `&0xff`，
modpost 解析重定位表越界直接 `Segmentation fault`，连续崩了三次才定位。

---

## 6. 隐藏能力：能做到什么，做不到什么

| 项目 | 状态 | 手段 |
|---|---|---|
| `lsmod` | **已隐藏** | `list_del_init(&THIS_MODULE->list)` |
| `/proc/modules` | **已隐藏** | 同上（两者遍历同一链表） |
| `/proc` 控制节点 | **已消除** | 载荷编进 .ko，`CR_CTL_NODE` 默认不定义 |
| 挂载痕迹 | **无** | 内核模块方案不产生 mount |
| `dmesg` | 有 1 行 `pr_info` | 改 `pr_debug` 可消除 |
| `/sys/module/cpuinfo_redirect` | **无法消除** | 见下 |

### 为什么 `/sys/module` 去不掉

两条路都堵死，实测数据：

```
kobject_del       __ksymtab = 1（已导出）→ 调用后目录消失，但模块加载整体失败
kobject_rename    __ksymtab = 0（未导出）→ 模块引用会 Unknown symbol
sysfs_rename_dir  __ksymtab = 0（未导出）→ 同上
```

`kobject_del` 被模块收尾流程依赖，在 `cr_init` 里删掉它的 `kobj` 会让内核后续收尾失败，
整个模块被回滚（表现：`kallsyms` 无符号、`dmesg` 无 `cr_init` 日志、伪装完全不生效）。

**结论：这个暴露点无解，只能接受。** 实际风险中等——`/sys/module/` 下有 809 个条目，
需要检测方主动遍历全表做名字匹配才会命中；而最常被查的 `lsmod` 和 `/proc/modules` 已经干净。

---

## 7. 与 plq110-spoof-suite 的共存

两个模块加载同名的内核模块时，第二次会**静默失败**：

```
cpuinfo_redirect: module is already loaded
```

所以职责拆分：

- `cpuinfo-redirect-driver`（provider 模块）：**只放 .ko 文件，不加载**
- `plq110-spoof-suite`：**唯一加载方**，`cpu.sh` 的 `cpu_pick_ko()` 按 `uname -r` 从两边挑文件

查找顺序：

```
$MODDIR/payload/cpuinfo_redirect-<MAJ.MIN>.ko
/data/adb/modules/cpuinfo-redirect-driver/payload/cpuinfo_redirect-<MAJ.MIN>.ko
$MODDIR/payload/cpuinfo_redirect.ko          # 兜底
```

---

## 8. 完整性清单是硬门禁（重要）

改动 `payload/` 下任何被 `payload.sha256` 覆盖的文件后，**必须同步更新清单**，
否则开机的 `apply.sh` 会在加载阶段整层跳过：

```
ERROR: payload integrity failed (sha256sum -c)
```

**症状是「开关是 on 但完全没生效」**，极难从表象判断。这个坑踩过一次。

更新方法（只改对应行，不要重算全表——原清单刻意排除了 `system.prop` 等构建期产物）：

```sh
nh=$(sha256sum payload/cpu.sh | cut -d' ' -f1)
sed -i "s|^[0-9a-f]*  payload/cpu.sh$|$nh  payload/cpu.sh|" payload.sha256
```

---

## 9. 哪些属性可以伪装，哪些绝对不行

实测结论（`grep -rl <prop> /vendor/etc/init /system/etc/init /odm/etc/init`）：

| 属性 | init 引用数 | 能否伪装 |
|---|---|---|
| `ro.product.model` / `brand` / `manufacturer` / `marketname` | 0 | ✅ 安全，纯显示 |
| `ro.product.{product,odm,vendor,system_ext,system}.{model,brand,manufacturer}` | 0 | ✅ 安全 |
| `ro.build.device_family` / `host` / `description` | 0 | ✅ 安全 |
| **`ro.product.device`** | 0（但被运行时服务查） | ❌ **绝对不能改** |
| **`ro.product.name`** | 0（同上） | ❌ **不能改** |
| **`ro.soc.model`** | **3**（qcv / cryptoeng / gameopt） | ❌ 不能改 |
| **`ro.hardware`** | 1（init.rc）+ HAL 定位 | ❌ 绝对不能改 |
| **`ro.board.platform`** | 1（cryptoeng） | ❌ 不能改 |
| **`ro.build.fingerprint`** | — | ❌ 会导致高通音频 HAL abort |

### 三个真实事故

**`ro.product.device` = CMM-AL10** 导致两个子系统同时崩：

```
SurfaceFlinger  → DisplayColorSetting: Unknown 303  → 全局屏幕偏暗
MediaProvider   → 数据库与文件系统不同步          → 文件管理器看不到文件
```

修复：`resetprop ro.product.device OP6113L1` + 重启 SurfaceFlinger，两项立即恢复。
该属性已从 `ident_keys()` 永久移除。

**`ro.soc.model` = Kirin9050Pro** 被 3 个 init 脚本引用（相机 QCV、加密引擎 FDE、游戏优化），
按型号找不到配置。已恢复 `SM8750` 并永久移除。

**`ro.build.fingerprint` = HUAWEI/CMM-AL10/...** 导致高通 Codec2 音频组件 `SIGTRAP` 自杀：

```
#00 libqc2audio_core.so
#01 ComponentStore::tryCreateMultiAccessUnitInterface
signal 5 (SIGTRAP), code 1 (TRAP_BRKPT)
```

累积产生 32 个 tombstone。`StackManager` 按 fingerprint 找厂商配置，读到 Huawei 就 `abort()`。
FP 层现已关闭。这是**根本性冲突**，想保住指纹伪装就得接受音频 HAL 崩溃。

---

## 10. 设备遗留问题（与伪装无关）

这些在任何属性伪装生效**之前**就已存在，改伪装不会影响它们，也不会被伪装修好：

```
OPLUS_CHG ... rc=-524            开机 4.8 秒即出现，充电芯片通信失败
cpudmof/0-7 wchan=qcom_dmof_kthread_fn
   内核栈 qcom_dmof_kthread_fn+0x150/0x2dc [qti_dmof_scmi]
   8 个 CPU 调频线程卡在高通 SCMI 框架里等固件响应，持续 D 状态
usbtemp_kthread / osml_monitor   同样 D 状态
```

**后果**：`load average` 长期 12–25（8 核机器），系统响应迟钝，冷启动慢。
开机瞬时峰值可达 **348**，随后十几分钟内逐步回落。

**这不是软件能修的**，指向内核/固件侧的通信异常。排查方向：是否刷过非官方内核、或等厂商固件更新。

---

## 11. 出问题怎么救

### 卡开机 / bootloop

```sh
# 1. 先看能不能连上
adb devices
adb shell getprop sys.boot_completed   # 1 = 系统已起，只是 UI 没出来
adb shell getprop init.svc.bootanim    # running = 卡在开机动画

# 2. 拉显示栈
adb shell su -c 'setprop ctl.restart surfaceflinger'
adb shell su -c 'setprop service.bootanim.exit 1'
```

### 彻底禁用模块

```sh
# CPU 驱动 provider（不加载任何东西，留着无害）
touch /data/adb/modules/cpuinfo-redirect-driver/disable

# spoof-suite CPU 层
touch /data/adb/modules/plq110-spoof-suite/state/off
```

### 属性被改坏（屏幕暗 / 存储空）

```sh
su -c 'resetprop ro.product.device OP6113L1'
su -c 'resetprop ro.product.name PLQ110'
su -c 'resetprop ro.soc.model SM8750'
su -c 'setprop ctl.restart surfaceflinger'
su -c 'content call --uri content://media/ --method scan_volume --arg external_primary'
```

### 回滚 spoof-suite 补丁

补丁安装器会在 `payload/` 下留 `.bak.<时间戳>` 备份，直接覆盖回去即可。

---

## 12. 项目当前状态（2026-10-18 晚更新）

**已完成**：

- **6 个内核版本**的 `.ko` 全部编译通过并验证 vermagic（5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18）
- 6.6 版本在真机验证：16 线程 / `CPU part 0xd25` / `implementer 0x48`
- `lsmod` / `/proc/modules` / `/proc` 控制节点 / 挂载痕迹 全部消除
- provider 模块与 spoof-suite 共存机制验证通过（六因子匹配 + 前缀回退）
- 补丁安装器实测通过（备份→覆盖→只更新清单对应行→校验 0 失败）
- 已定位并永久移除三个高危伪装属性（`ro.product.device` / `ro.product.name` / `ro.soc.model`）
- **采集脚本实测通过**：`grab-device-info.bat` 双击 → 2 MB 包，含 uname-r + 6 MB BTF + config
- **全自动编译脚本实测通过**：`update-module.sh` 无参数自动 adb 采集（BTF 6,099,729 B 拉到）
- **设备端自动编译脚本语法验证通过**（六脚本全部过 mksh `sh -n`）

**未完成 / 已知限制**：

1. `/sys/module/cpuinfo_redirect` 无法消除（技术死路，见第 6 节）
2. **5.10 / 5.15 / 6.1 / 6.12 / 6.18 五份 `.ko` 未在任何真机上验证过加载**，只有编译验证
3. `dmesg` 仍有 1 行 `pr_info`，可改 `pr_debug` 消除（零风险，未做）
4. 设备本身的 DMOF / 充电芯片通信异常未解决（非本工程范畴，见第 10 节）
5. 指纹伪装（FP 层）与高通音频 HAL 存在根本冲突，当前选择保音频、关 FP
6. **Windows 原生环境无法编译**（缺 make / 交叉工具链），只能采集；编译需 Linux / WSL2 / CI / 设备端
7. **设备端自动编译未在真机上完整跑过一遍**（脚本语法通过，但 `adapt-on-device.sh` 的编译段没实测）

---

## 13. 如果继续做，建议顺序

1. **实测设备端自动编译** —— 在设备上跑一次 `tools/adapt-on-device.sh`，确认 2–3 分钟内能出产物。
   这是整条链路唯一没实测的环节（脚本语法已过，但编译段没跑过）。
2. **验证其余五份 .ko** —— 找对应内核的真机实测 `insmod`，或至少在同型号不同固件版本上试。
3. **`pr_info` → `pr_debug`** —— 消除最后一处 dmesg 痕迹，零风险。
4. **修设备** —— 弄清 `qti_dmof_scmi` 为什么卡在 SCMI 通信，这是当前一切卡顿的根源（与伪装无关）。
4. **重启后回归验证**：确认 `ro.product.device` 保持 `OP6113L1`、显示与存储正常

---

## 14. 源码获取与复现编译

宿主无 Linux 环境（Windows 10 家庭版、非管理员、无 WSL），编译在**设备上**完成：

```
设备侧：Termux clang 21 + make + ld.lld
源码：github.com/aosp-mirror/kernel_common（android.googlesource.com 在该网络不可达）
分支：android12-5.10 / android13-5.15 / android14-6.1 / android15-6.6 / android16-6.12
```

编译流程见第 5 节的六个补丁，每版本约 60–90 秒（modules_prepare 45s + 模块 10s + 解压 8s）。

跨设备构建用 `tools/build-for-device.sh`：

```sh
./tools/build-for-device.sh kx.tgz              # 标准 CRC 构建
./tools/build-for-device.sh kx.tgz --no-crc     # 空 __versions，绕开 Module.symvers
./tools/build-for-device.sh kx.tgz --legacy     # 强制 make 而非 bazel
```

## 6.18 适配记录（2026-10-18 追加）

第六个内核版本 `android17-6.18`（6.18.0-rc6）已编译通过。

### 新增的三个坑

**坑 1：`proc_dir_entry` 少了 `proc_dops` 字段**

```c
// 6.1 / 6.6
const struct inode_operations *proc_iops;
union { proc_ops / proc_dir_ops };
const struct dentry_operations *proc_dops;   // ← 6.18 没有这个
union { seq_ops / single_show };

// 6.18
const struct inode_operations *proc_iops;
union { proc_ops / proc_dir_ops };
union { seq_ops / single_show };             // ← 前移 8 字节
```

意味着 `parent` / `subdir` / `name` 等字段偏移在 6.18 上全部前移 8。
**但 `CR_PDE_PROCOPS`（48）和 `CR_POPS_*` 不变**，而代码运行时只用这两个，
所以现有 `cr_offsets.h` 在 6.18 上仍能编译通过、`static_assert` 也自洽。

**风险**：如果用到了 `parent`/`name` 等字段就会读错位置。当前实现没有用，但换功能前必须重新生成偏移表。

**坑 2：6.18 的 host 工具带 `-Werror`**

bionic 的 `bits/wait.h` 引起的 `-Wincompatible-pointer-types`、`-Wmacro-redefined` 直接变 error。
解法是在 `HOSTCFLAGS` 里追加：

```sh
-Wno-error -Wno-incompatible-pointer-types -Wno-macro-redefined -Wno-int-conversion
```

**坑 3：ARM 重定位常量缺失**（与 6.12 相同）

`modpost.c` 引用 `R_ARM_MOVW_ABS_NC` 等 41 个常量，bionic 的 `elf.h` 里没有。
必须用 `#undef` + `#define` 无条件写入 `modpost.h`——**不能包在 `#ifndef` 里**，
因为系统头已经定义了 `R_ARM_NONE`，整块会被跳过。

### 6.18 的 vermagic 现状

```
vermagic=6.18.0-rc6 SMP preempt mod_unload aarch64
```

`android17-6.18` 分支目前仍是 RC 阶段。产物 **253,304 B**，
走 `proc_ops` 后端（该分支没有 `trace/hooks/cpuinfo.h`，无 RVH）。

### 编译耗时（设备端，Termux clang 21.1.8）

| 阶段 | 耗时 |
|---|---|
| 下载 6.18 源码 tar.gz（257 MB） | 78 s |
| 解压（1.6 GB） | 13 s |
| defconfig | 13 s |
| modules_prepare | 12 s |
| prepare | 5 s |
| 编译模块 | 5 s |

### 环境踩坑

`/data/local/tmp/pbuild` 由 root 创建后权限是 `drwxr-xr-x root root`，
`adb push` 以 shell 用户运行会被拒。**必须先 `chmod -R 0777`**。
这个坑很难看出——push 只报 `No such file or directory`，看起来像目录不存在。

---

## 六个版本汇总（更新）

| 内核 | 大小 | vermagic | 后端 | 真机验证 |
|---|---|---|---|---|
| 5.10 | 239,320 | 5.10.245 | proc_ops | ✗ |
| 5.15 | 234,360 | 5.15.194 | proc_ops | ✗ |
| 6.1  | 245,592 | 6.1.155 | proc_ops | ✗ |
| 6.6  | 190,072 | 6.6.89-android15-8-...-4k | **RVH** | **✓** |
| 6.12 | 255,368 | 6.12.52 | proc_ops | ✗ |
| **6.18** | **253,304** | **6.18.0-rc6** | proc_ops | ✗ |

只有 6.6 走 RVH 且经真机验证。其余五个都是 proc_ops 后端，
**用的是同一份 `cr_offsets.h`（6.6 的偏移）**，`CR_PDE_PROCOPS` 与 `CR_POPS_*` 在各版本恰好一致，
但 `proc_dir_entry` 的其他字段偏移不同——**不要在这些版本上扩展使用其它字段**。


## AndroidDriveSignity 分析（2026-10-18）

来源：`github.com/gmh5225/androiddrivesignity`（gs.awesome.rip「Android 内核驱动开发」分类第 3 项）

### 脚本做了什么

85 行 Python，把内核镜像里三个函数的入口改掉。三条 patch 的字节解码如下（ARM64 小端）：

| 目标 | 字节 | 反汇编 | 语义 |
|---|---|---|---|
| `check_modinfo` | `00 00 80 52` / `C0 03 5F D6` | `mov w0,#0` / `ret` | 直接返回 0（成功） |
| `check_version` | `20 00 80 52` / `C0 03 5F D6` | `mov w0,#1` / `ret` | 直接返回 1（通过） |
| `module_sig_check` | `00 00 80 52` / `C0 03 5F D6` | `mov w0,#0` / `ret` | 直接返回 0（成功） |

`check_version` 返回 **1** 而非 0，是因为内核里它的语义是「1 = 校验通过」。

### PACIASP 处理（这部分的实现是对的）

```python
if kernel_data[off:off+4] == bytes.fromhex('3F 23 03 D5'):   # PACIASP
    patch = PACIASP + mov + AUTIASP + ret
```

- `3F 23 03 D5` = `PACIASP`（用 APIA key 签 LR）
- `BF 23 03 D5` = `AUTIASP`（返回前验签）

函数开头如果签了 LR，patch 后**必须补一条 AUTIASP**，否则 `ret` 时 PAC 校验失败直接崩。
本机 `CONFIG_ARM64_PTR_AUTH_KERNEL=y`、CPU 有 `paca`，所以这段逻辑是**必需**的。

### 三个目标的实际可用性（本机实测）

```
check_modinfo      不存在 ✗     ← 被内联
check_version      存在 (T) ✓
module_sig_check   存在 (T) ✓
```

查 `android15-6.6` 的 `kernel/module/main.c` 确认原因：

```c
2122| static int check_modinfo(struct module *mod, struct load_info *info, int flags)
2866|     err = check_modinfo(info->mod, info, flags);      // 全文件仅此一处调用
```

`static` + 单一调用点 → 编译器内联 → **符号表里没有，patch 不到**。

### 这带来的硬限制

| 校验 | 函数 | 能否 patch |
|---|---|---|
| **vermagic**（`invalid module format`） | `check_modinfo` | **不能**（内联） |
| 符号 CRC（`disagrees about version`） | `check_version` | 能 |
| 模块签名 | `module_sig_check` | 能 |

**所以 `AndroidDriveSignity` 绕不过 vermagic。** 它解决的是「CRC 和签名」，
而真正拦住跨版本 `.ko` 的是 vemagic——那是被内联的 `check_modinfo` 在做的事。

### 对本工程的实际意义

**6.6 那份 `.ko` 不需要 patch**：vermagic 原生匹配，CRC 由设备自己的源码树产出，
`CONFIG_MODULE_SIG_FORCE` 未开。它是干净的。

**另外五个版本 patch 了也装不上**：vermagic 仍然不匹配，`check_modinfo` 照样拒绝。

### 如果真要绕 vermagic，还需要什么

需要额外 patch `load_module()` 里那条 `bl check_modinfo` 调用点：

```
1. 反汇编 load_module，找 bl <check_modinfo> 指令
2. 把它改成 mov w0, #0（或跳过后面的错误检查）
3. 或者把紧随其后的 'if (err) goto free_mod' 分支改成无条件跳过
```

原脚本**没有做这件事**。技术上可行，但要逐内核反汇编定位，且回滚成本高（改 boot.img）。

### 另外注意：本机有厂商私有校验

```
grep -ciE 'security_check|oplus.*module' /proc/kallsyms  →  9496 个符号
```

OPPO 有自己的 `KERNEL_SECURITY_CHECK`（日志里报过 `hash not found, maybe unknown ko`）。
但实测它**只警告、不阻止加载**，标准三函数才是真正的拦截点。

### 结论

这个思路对小范围场景有用（CRC/签名不匹配时有救），但**解决不了跨内核版本的核心障碍**。
本工程维持现状：**vermagic 必须匹配，跨设备一律用 `build-for-device.sh` + `kx.tgz` 重编**。


---

## 15. 工具链全貌（2026-10-18 晚补充）

### 15.1 三条使用路径

按「你手上有什么」选：

| 场景 | 用什么 | 产出 |
|---|---|---|
| 有 Windows + 设备 | `tools/grab-device-info.bat` 双击 → 拿包去 Linux 编 | `cr-device-info.tgz`（2 MB）→ `.ko` / `zip` |
| 有 Linux + 设备 | `tools/update-module.sh`（无参数，全自动） | 直接出可刷 zip |
| 只有设备 | 刷入模块，它自己编（需 Termux） | 重启后自动生效 |

### 15.2 采集脚本：为什么用 bat 而不是 PowerShell

PowerShell 版写过三版全失败，坑在这里：

| 问题 | 症状 | 根因 |
|---|---|---|
| **无 BOM 的 UTF-8 按 ANSI 读** | 中文全乱，报 `意外的标记"璁惧:"` | PS 5.x 默认按系统代码页解析 `.ps1` |
| **`adb push \| Out-Null` 死锁** | 卡在 push 不动 | PowerShell 的 `&` 调用 + 管道捕获与 adb 的 stdout 缓冲冲突；同样命令用 Python `subprocess.run` 秒过 |
| **嵌套引号被重解析** | `dd` 那步静默失败 | `adb shell "su -c '...'"` 三层引号在 PS 里被吃掉一层 |

bat 三个问题都没有，且双击可跑。**`.ps1` 已删除，不要复活它。**

### 15.3 全自动模式（`update-module.sh`）

不传参数 = 自动：

```bash
./tools/update-module.sh            # 自己 adb 读 uname-r、自己 dd BTF、编译、打包
./tools/update-module.sh --dry-run  # 只采集不编译，验证连通
./tools/update-module.sh <uname-r> <btf>   # 手动给参数
./tools/update-module.sh --manual ...      # 强制手动，缺参数报错
```

### 15.4 设备端现场编译（`adapt-on-device.sh`）

刷入模块后，如果 `payload/` 里没有本机内核的 `.ko`：

```
post-fs-data.sh   写 state/need-adapt
service.sh        开机 5 秒后 setsid 拉起 adapt-on-device.sh（不阻塞开机）
adapt-on-device.sh
  uname -r → 选 GKI 分支 → 查 Termux → 查 3GB 空间 →
  下载 250MB → 解压 1.6GB → 打六补丁 → dump BTF →
  defconfig/prepare/编译 → 校验 vermagic → 写入 payload/ + targets/
```

耗时约 2–3 分钟（8 核 + 好网络）。进度看 `state/adapt.status` 与 `state/adapt.log`。

**前提**：Termux 已装（`/data/data/com.termux/files/usr/bin/clang`）。没有则失败并写明原因。

### 15.5 这一轮修掉的 bug（都是真问题）

| # | 位置 | 症状 | 根因 | 修法 |
|---|---|---|---|---|
| 1 | `adapt-new-device.sh` / `update-module.sh` | 零输出静默退出 | `${#POS[@]}` 在 `set -u` 下对空数组报 unbound | 改用计数变量 `POSN` |
| 2 | 同上（我的错误修复） | `bad substitution` | `${#POS[@]:-0}` 是非法 bash 语法 | 同 #1 |
| 3 | `update-module.sh` | `info: command not found` | 函数定义落在使用点之后 | 定义前置 |
| 4 | `update-module.sh` | `adb pull` 找 `C:/Program Files/Git/data/local/tmp/...` | MSYS 把设备端路径自动转成 Windows 路径 | `export MSYS2_ARG_CONV_EXCL="*"` |
| 5 | `update-module.sh` | 主版本算成 `6.6.6` | `${KV%.*}` + `cut -f2` 重复拼接 | `cut -d. -f1,2` |
| 6 | 两个脚本 | python 调用全失败 | `python3` 是应用商店 stub，`--version` 无输出却返回 0 | 真执行一次 `-c` 才认 |
| 7 | `adapt-on-device.sh` | `no closing quote` | `log "=== adapt start ===` 结尾引号被写文件时吞掉 | 补引号 |
| 8 | `adapt-on-device.sh` | `unexpected '('` | `log "extracting ($(...))"` 裸括号在 mksh 下解析失败 | 先取值再打印 |
| 9 | `adapt-on-device.sh` | `sh -n` 报错（实际能跑） | `.mod.c` 修补用的 awk 正则转义 | 改 `grep -n` + `head`/`tail` 拼接 |

**#1 和 #2 值得单独记住**：第一个是真 bug，第二个是「用一个新错误掩盖旧错误」的反面教材。最终解法是计数变量，这是 POSIX shell 里唯一稳妥的写法。

### 15.6 脚本语法必须在设备上验，不能用 Git Bash

这些是 Android `sh`（mksh）脚本，Git Bash 的 bash 语法更宽松，会放过 mksh 不认的写法。

正确做法：

```bash
adb push script.sh /data/local/tmp/_c.sh
adb shell "su -c 'sh -n /data/local/tmp/_c.sh'"   # 无输出 = 语法通过
```

本工程六个脚本的最终状态：**全部通过 mksh `sh -n`**。

### 15.7 Windows 上编不了内核模块（硬限制）

实测依赖：

```
git          OK
python       OK (3.14.7)
tar/sha256sum OK
---
make         MISSING
gcc/clang    MISSING
bc/flex/bison MISSING
aarch64-linux-gnu-gcc MISSING
```

`make defconfig` 第一步就会失败。Windows 原生环境做不到，必须 Linux / WSL2 / CI / 设备端。

采集环节不受影响——`adb` 可用就够了。


---

## 16. 快速排错对照表

| 现象 | 最可能的原因 | 先看哪里 |
|---|---|---|
| 刷入后 `/proc/cpuinfo` 没变 | 模块从不自己加载，需要 loader | `cat state/status` 看是否 `exact-match` |
| `state=unsupported` | `payload/` 里没有本机内核的 `.ko` | 看 `state/need-adapt`，然后手工跑 `adapt-on-device.sh` |
| `state=prefix-fallback` | 缺精确元数据，退到主版本匹配 | 跑一次 `tools/make-target-env.sh` 补指纹 |
| `insmod` 报 `invalid module format` | vermagic 不匹配 | `strings xxx.ko \| grep vermagic` 对比 `uname -r` |
| `insmod` 报 `disagrees about version` | 符号 CRC 不匹配 | 用 `--no-crc` 重编（默认已开） |
| `insmod` 报 `Unknown symbol` | 目标内核没有该导出符号 | `grep <符号> /proc/kallsyms` 确认 |
| 加载成功但立刻 oops | `cr_offsets.h` 偏移错误 | 用目标设备的 BTF 重新 `gen_offsets.py` |
| `apply.sh` 报 `payload integrity failed` | 改了 `payload/` 没更新清单 | 更新 `payload.sha256` 对应行（见第 8 节） |
| 屏幕全局偏暗 / 文件管理器空 | `ro.product.device` 被伪装 | 见第 9 节，`resetprop` 恢复后重启 SurfaceFlinger |
| 音频 HAL 反复崩 | `ro.build.fingerprint` 是厂商外的值 | 关 FP 层 |
| `adb pull` 报找不到 `C:/Program Files/Git/...` | MSYS 路径转换 | `export MSYS2_ARG_CONV_EXCL="*"`（见 15.5 #4） |
| 脚本零输出静默退出 | 空数组 + `set -u` | 见 15.5 #1 |
| `sh -n` 报 `unexpected '('` 或 `no closing quote` | mksh 比 bash 严格 | 用设备上的 `sh -n` 验，别用 Git Bash |

---

## 17. 一句话总结

**这份工程解决的是「让 /proc/cpuinfo 在一台特定 Android 设备上伪装成 Kirin」**，
不解决「一份产物通吃所有内核」——后者做不到，因为 `proc_dir_entry` 布局必然随内核演进变化。

能做到的是：**拿到 `uname -r` 和 BTF，15 分钟生成一份可加载的产物**。
窗口期内的三次自动化（采集 / 编译 / 设备端现场编译）就是为了把这个过程压到最低成本。

