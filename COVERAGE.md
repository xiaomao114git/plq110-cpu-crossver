# 内核覆盖矩阵

生成时间：2026-10-18

## 支持范围

| 内核 | GKI 分支 | 后端 | 产物 | vermagic | 真机验证 |
|---|---|---|---|---|---|
| 5.10 | android12-5.10 | proc_ops | 239,320 B | 5.10.245 SMP preempt mod_unload aarch64 | ✗ |
| 5.15 | android13-5.15 | proc_ops | 234,360 B | 5.15.194 SMP preempt mod_unload aarch64 | ✗ |
| 6.1  | android14-6.1  | proc_ops | 245,592 B | 6.1.155 SMP preempt mod_unload aarch64 | ✗ |
| 6.6  | android15-6.6  | **RVH** | **190,072 B** | 6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k SMP preempt mod_unload modversions aarch64 | **✓** |
| 6.12 | android16-6.12 | proc_ops | 255,368 B | 6.12.52 SMP preempt mod_unload aarch64 | ✗ |
| 6.18 | android17-6.18 | proc_ops | 253,304 B | 6.18.0-rc6 SMP preempt mod_unload aarch64 | ✗ |

六个版本全部在设备端（Termux clang 21.1.8）实际编译产出，非预置。

## 为什么不覆盖 5.5–5.9

两个硬约束，都不是实现问题：

**① `proc_ops` 是 5.6 才引入的**

```c
// include/linux/proc_fs.h
struct proc_ops { ... };   // Linux 5.6+
// 5.5 及以下用 struct file_operations + proc_dir_entry->proc_fops
```

本工程的 proc_ops 后端在 5.5 上无法编译，因为结构体根本不存在。

**② Android GKI 分支从 5.10 开始**

```
android12-5.10    ← 最早的 GKI
android13-5.15
android14-6.1
android15-6.6
android16-6.12
android17-6.18
```

5.5–5.9 没有对应的 Android 通用内核分支，各厂商（OPPO/小米/华为）都用自己的树，
接口和结构布局互不相同。没有源码就拿不到 BTF、拿不到 `proc_dir_entry` 布局。

**结论**：5.5–5.9 要支持，只能针对具体机型 + 具体厂商源码单独适配，不属于「通用覆盖」范畴。

## 为什么不覆盖 5.4

`android11-5.4` 分支确实存在，但：

- 5.4 < 5.6，没有 `proc_ops`
- 该分支没有 `trace/hooks/cpuinfo.h`，无法走 RVH
- 要走的话得基于 `file_operations` 另写一条后端，且同样依赖逐机型 BTF

## 五个 proc_ops 版本的共同风险

**它们共用同一份 `cr_offsets.h`（6.6 实测值）。**

现在能编过、`static_assert` 也自洽，是因为代码运行时**只用两个偏移**：

```
CR_PDE_PROCOPS = 48     定位 proc_ops 指针
CR_POPS_*       = 8/16/32/40/48   替换函数指针
```

这两个值在 5.10 / 5.15 / 6.1 / 6.12 / 6.18 上恰好一致（已逐一核对结构定义）。

**但 `proc_dir_entry` 的其它字段偏移并不相同：**

| 字段 | 6.1 / 6.6 | 6.18 |
|---|---|---|
| `proc_dops` | **存在（56）** | **不存在** |
| `seq_ops` | 64 | 56 |
| `parent` | 120 | 112 |
| `name` | 160 | 152 |

**所以：不要在这些版本上扩展使用 `parent` / `name` / `mode` 等字段。**
一旦要用，必须用 `tools/gen_offsets.py --btf <目标内核>` 重新生成偏移表。

## 真机验证状态

只有 **6.6.89** 做过完整验证：

```
dmesg: cpuinfo_redirect: init ok backend=rvh payload=6827 bytes
/proc/cpuinfo: 16 线程 / CPU part 0xd25 / implementer 0x48
lsmod: 不可见    /proc/modules: 不可见    /proc 控制节点: 无
```

其余五个版本的产物**只有编译验证**，没有在任何真机上 `insmod` 过。
装上之前必须用 `tools/build-for-device.sh` 配目标设备的 `kx.tgz` 重新编，
否则 vermagic 不匹配，`insmod` 会直接拒绝。
