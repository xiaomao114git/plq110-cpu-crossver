# 如何继续适配其他内核的设备

## 一、先分类：三种设备，三种做法

拿到一台新设备，第一件事不是编译，是**判断它属于哪一类**。

```
adb shell uname -r
```

| 输出形态 | 类别 | 难度 | 主要风险 |
|---|---|---|---|
| `6.6.89-android15-8-gxxxx-abogki...` | **A. 同分支不同固件** | 低 | vermagic 不同，重编即可 |
| `6.1.177-android14-11-o-gxxxx` | **B. 跨分支** | 中 | 偏移表可能不同 |
| 没有 `-androidNN-` 前缀，或带厂商后缀 | **C. 非 GKI / 厂商定制** | 高 | 可能无 RVH hook，PROC 偏移不可信 |

判据就一条：**`-android<版本>-<主版本>` 这一段在不在**。

- 有 → 走 GKI 路径，源码能从 `aosp-mirror/kernel_common` 拉到
- 没有 → 厂商自己的树，**大概率拿不到源码**，只能靠 BTF

---

## 二、决策树

```
uname -r
  │
  ├─ 有 -androidNN-X.Y 前缀
  │    │
  │    ├─ 主版本 ∈ {5.10, 5.15, 6.1, 6.6, 6.12, 6.18}
  │    │    │
  │    │    ├─ 有 /sys/kernel/btf/vmlinux
  │    │    │    └─ ✅ 可编，走标准流程（见第三节）
  │    │    │
  │    │    └─ 没有 BTF
  │    │         │
  │    │         ├─ 该分支有 trace/hooks/cpuinfo.h（只有 6.6 有）
  │    │         │    └─ ✅ 可编，走 RVH（不需要 BTF）
  │    │         │
  │    │         └─ 没有 hook
  │    │              └─ ⛔ 停止。PROC 需要 BTF 推导偏移，没有就是猜
  │    │
  │    └─ 主版本 ∈ {5.5~5.9, 7.x}
  │         └─ ⛔ 停止。Android GKI 只覆盖 5.10–6.18
  │
  └─ 无 GKI 前缀 / 厂商定制
       │
       ├─ 有 BTF 且能确认 kernel version ≥ 5.6（有 proc_ops）
       │    └─ ⚠️ 可试，但要按第四节做安全验证
       │
       └─ 无 BTF
            └─ ⛔ 停止
```

---

## 三、A / B 类设备的标准流程

### 3.1 采集

```bash
# Windows 上双击
tools\grab-device-info.bat
```

产出 `cr-device-info.tgz`，内含：

| 文件 | 作用 |
|---|---|
| `uname_r.txt` | 构造 `LOCALVERSION`，决定 vermagic |
| `vmlinux.btf` | 推导 `proc_dir_entry` 偏移 |
| `config.gz` | 判断 `RANDSTRUCT` / `MODVERSIONS` |

### 3.2 编译

```bash
tar xzf cr-device-info.tgz

# 只要 .ko
./tools/adapt-new-device.sh "$(cat cr-device-info/uname_r.txt)" cr-device-info/vmlinux.btf

# 或要可刷 zip
./tools/update-module.sh "$(cat cr-device-info/uname_r.txt)" cr-device-info/vmlinux.btf
```

脚本会自动：选 GKI 分支 → clone 源码 → 打六个补丁 → 从 BTF 生成偏移表 → `--no-crc` 编译 → 校验 vermagic 前缀。

### 3.3 同分支为什么也可能需要调偏移

**B 类跨分支**（如 6.1）与 6.6 的差异：

```
              6.1 / 6.6    6.18
proc_iops       40          40
proc_ops        48          48      ← 运行时只用这个，恰好一致
proc_dops       56          不存在   ← 6.18 少了
seq_ops         64          56
parent          120         112
```

**代码运行时只依赖 `CR_PDE_PROCOPS`（48）和 `CR_POPS_*`**，这两组在 5.10–6.18 上恰好一致。

**但这是运气**。`gen_offsets.py` 从 BTF 读的是真实布局，所以只要**目标设备的 BTF 在**，生成的表就是对的。

---

## 四、装之前必须做的安全验证（关键）

`insmod` 成功 **不等于** 偏移正确。PROC 后端按错误偏移写内核内存，表现为加载成功但立刻 oops。

**必须按这个顺序验证**：

### 4.1 先看日志，再决定要不要装

```bash
# 1. 记录当前状态
adb shell 'grep -m1 "CPU implementer" /proc/cpuinfo'
adb shell 'uname -r'
adb shell su -c 'dmesg -c > /dev/null'   # 清空 dmesg

# 2. 装载
adb push dist/cpuinfo_redirect-<ver>.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/cpuinfo_redirect-<ver>.ko'
echo "insmod rc=$?"

# 3. 立刻查三件事
adb shell su -c 'dmesg | tail -30'              # 有没有 oops / BUG / Unable to handle
adb shell 'head -30 /proc/cpuinfo'              # 能不能正常读
adb shell 'grep -m1 "CPU implementer" /proc/cpuinfo'   # 0x48=成功 0x51=未生效
```

### 4.2 判定表

| `dmesg` | `/proc/cpuinfo` | 结论 | 动作 |
|---|---|---|---|
| 无 oops | 正常 + `0x48` | **偏移正确** | 保留 |
| 无 oops | 正常 + `0x51` | 加载了但没生效 | 查 `state/status`，可能没选中该层 |
| 无 oops | **乱码 / 截断** | **偏移错误（读错位置）** | **立即 `rmmod`** |
| 有 oops | 读不了 | **偏移错误（写错位置）** | 强制重启 |

### 4.3 出问题怎么救

```bash
# 如果 insmod 成功但 cpuinfo 异常，立刻卸载
adb shell su -c 'rmmod cpuinfo_redirect'

# 如果已经 oops 卡住
adb shell su -c 'setprop ctl.restart zygote'
# 都不行就长按电源键强制重启
```

**注意**：如果 `list_del_init` 已经把模块从链表摘掉，`rmmod` 可能报 `module not found`。这种情况下只能重启。

---

## 五、C 类（厂商定制）的特殊处理

这类设备的 `uname -r` 长这样：

```
4.19.157-perf+                          ← 无 GKI 前缀
5.10.43-android12-9-00001-gxxxx         ← 有前缀但也有厂商段
```

**先判断两件事**：

### 5.1 有没有 `proc_ops`

`proc_ops` 是 Linux 5.6 引入的。5.5 及以下用 `file_operations`，本工程的 PROC 后端编不了。

```bash
# 在设备上（需要 root）
adb shell su -c 'grep -c proc_ops /proc/kallsyms'
```

返回 0 → 内核 < 5.6，**放弃**。

### 5.2 有没有 RVH hook

```bash
adb shell su -c 'grep -i "cpuinfo" /proc/kallsyms | head'
```

看到 `android_rvh_cpuinfo_c_show` → **优先走 RVH**（不需要偏移，安全得多）。

**这种情况下源码拉不到，`adapt-new-device.sh` 会失败**，需要手工路径：

```bash
# 用已知的 GKI 分支源码 + 目标设备的 BTF
# 关键：LOCALVERSION 必须精确等于 uname -r 去掉主版本后的部分
./tools/adapt-new-device.sh "$(cat uname_r.txt)" vmlinux.btf --tree /path/to/any-6.6-tree
```

`--tree` 允许复用任何同主版本的源码树，不必是目标设备原厂树。

---

## 六、按风险排序的推荐顺序

如果要适配多台设备，按这个顺序做，成功率最高：

| 顺序 | 设备类型 | 理由 |
|---|---|---|
| 1 | **6.6 系**（任意固件） | 唯一有 RVH hook 的分支，不需要偏移，失败率最低 |
| 2 | **6.1 / 6.12 系** | 有 BTF 就能推偏移，`proc_ops` 布局与 6.6 一致 |
| 3 | **5.10 / 5.15 系** | 同上，但更老的树，补丁可能需微调 |
| 4 | **6.18 系** | 已实编通过，但 `proc_dir_entry` 少了 `proc_dops`，需确认只用 48 偏移 |
| 5 | **厂商定制** | 最后试，先确认有 BTF 或有 RVH hook |

---

## 七、设备端现场编译（没有 Linux 机器时）

刷入模块后，如果 `payload/` 里没有本机产物，会自动触发：

```
post-fs-data.sh  写 state/need-adapt
service.sh       开机 5 秒后后台拉起
adapt-on-device.sh  下载源码 → 编译 → 写回 payload/
```

**前提**：设备装了 Termux（`/data/data/com.termux/files/usr/bin/clang`）。

```bash
# 查进度
cat /data/adb/modules/cpuinfo-redirect-driver/state/adapt.status
tail -f /data/adb/modules/cpuinfo-redirect-driver/state/adapt.log

# 手动触发一次
su -c 'sh /data/adb/modules/cpuinfo-redirect-driver/tools/adapt-on-device.sh'
```

耗时约 2–3 分钟（下载 80s + 解压 15s + prepare 20s + 编译 15s）。

---

## 八、已知的 6.1.177 案例

交接文档里记录了一台失败设备：

```
PLQ encrypted payload verified
kernel mismatch: got 6.1.177-android14-11-o-g1363348d34be
```

分析：

```
6.1.177                       ← KV
-android14-11-o-g1363348d34be ← LOCALVERSION
```

`android14-` 前缀说明是 android14 的树，对应 GKI 分支 `android14-6.1`。**本工程已经编出 `cpuinfo_redirect-6.1.ko`**，但它用的是 6.6 的偏移表。

**要真正适配这台**：

1. 拿到该设备的 BTF：`adb shell su -c 'dd if=/sys/kernel/btf/vmlinux of=/sdcard/btf'`
2. `./tools/adapt-new-device.sh 6.1.177-android14-11-o-g1363348d34be btf`
3. 装之前按第四节验证

**不要直接用现成的 `cpuinfo_redirect-6.1.ko`** —— 那份的偏移没在这个固件上验过。

---

## 九、一句话总结

**适配新设备 = 采两个文件 + 跑一条命令 + 按第四节验证。**

真正决定成败的不是编译，是**装之前的验证**——`insmod` 成功不代表偏移对，可能只是还没崩。
