# 如何让一份驱动跑多台设备

## 一、先说结论

**现在的架构（RVH / PROC）做不到通用，但换 kprobe 可以接近做到。**

| 方案 | 需要厂商 hook | 需要 proc_dir_entry | 需要改代码段 | CFI 影响 | 跨版本能力 |
|---|---|---|---|---|---|
| RVH | **是** | 否 | 否 | 无 | 只 6.6 / 6.12 |
| PROC（现用） | 否 | **是** | 否 | **可能 trap** | 同 GKI 分支 |
| **kprobe** | **否** | **否** | **否** | **无** | **最好** |
| inline hook | 否 | 否 | **是** | 无 | 好，但改代码段风险高 |

---

## 二、为什么现有两条路都不通用

### RVH 的死穴：依赖厂商

`android_rvh_cpuinfo_c_show` 这个 tracepoint **由厂商的 vendor 模块注册**。

```
你的 OPPO 6.6   → 注册了     → RVH 可用
小米 6.6        → 大概率没有  → RVH 版加载成功但回调永不触发
```

这跟内核版本无关，纯粹是厂商选择。**同一版本不同厂商结果可能完全相反。**

### PROC 的死穴：三个依赖

1. **`proc_dir_entry` 布局**——6.18 就少了 `proc_dops` 字段，后面全前移 8 字节
2. **`proc_ops` 函数指针替换**——`CONFIG_CFI_CLANG=y` 的内核会检查间接调用目标
3. **遍历 `/proc` 红黑树**——需要 `subdir` / `subdir_node` 两个偏移

三个依赖任何一个不满足就崩。

---

## 三、kprobe 方案为什么最通用

### 原理

```
show_cpuinfo(struct seq_file *m, void *v)
        ↑
   kprobe 挂在函数入口
        ↓
pre_handler 拿到 m 指针
        ↓
  m->count = 0;
  seq_write(m, 伪装的文本, 长度);
        ↓
原来的 show_cpuinfo 继续执行，但 buffer 已被我们改成占位
```

### 只依赖一个偏移

**`struct seq_file` 的 `count` 字段**。

实测两台 6.1 设备的 BTF：

```
struct seq_file  size=136  members=12
  +0    buf      char *
  +8    size     size_t
  +16   from     size_t
  +24   count    size_t     ← 只需要这一个
  +32   pad_until
  ...
```

这个布局在 **5.10 ~ 6.12 长期稳定**（`seq_file` 是核心结构，多年没动过字段顺序）。

### 为什么不受 CFI 影响

CFI 检查的是**间接调用的目标是否类型匹配**。kprobe 不改任何函数指针：

- `cpuinfo_op.show` 仍然指向原来的 `show_cpuinfo`
- kprobe 只是在函数入口插了断点指令（由内核自己管理）
- **CFI 看到的目标没变**

### 为什么不需要厂商 hook

`show_cpuinfo` 是内核自己的函数，**永远存在**（`arch/arm64/kernel/cpuinfo.c`）。

实测两台 6.1 的 BTF 里 `show_cpuinfo` 都是 4 处（定义 + 引用）。

---

## 四、kprobe 方案的技术细节

### 4.1 挂载

`show_cpuinfo` **未导出**（不在 `__ksymtab`），但 kprobe 能按符号名挂：

```c
static struct kprobe kp = {
    .symbol_name = 'show_cpuinfo',
    .pre_handler = cr_pre_handler,
};
register_kprobe(&kp);
```

kprobe 自己解析符号名，不需要 `kallsyms_lookup_name`（那玩意 5.7+ 也不导出了）。

### 4.2 pre_handler 里改输出

```c
static int cr_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    struct seq_file *m = (struct seq_file *)regs->regs[0];   /* ARM64：第一个参数在 regs[0] */
    const struct cr_blob *blob;

    if (!m) return 0;
    rcu_read_lock();
    blob = cr_blob_rcu();
    if (blob) {
        m->count = 0;
        seq_write(m, blob->bytes, blob->len);
        m->count = blob->len;
    }
    rcu_read_unlock();
    return 0;
}
```

注意 ARM64 的参数传递：`regs->regs[0]` 是第一个参数（`m`），不是 x86 的 `rdi`。

### 4.3 卸载

```c
unregister_kprobe(&kp);
```

比 PROC 的「恢复函数指针」干净——不需要保存原始 `proc_ops`。

---

## 五、跨内核版本怎么做

### 5.1 偏移

`seq_file.count = 24` 在 5.10 ~ 6.12 都一样，可以直接写死：

```c
#ifndef CR_SEQ_COUNT
#define CR_SEQ_COUNT 24
#endif
```

要保险可以从 BTF 生成：

```bash
python3 tools/gen_offsets.py --btf <目标BTF> --out src/cr_offsets.h
```

### 5.2 vermagic 和 CRC

用已经验证的改串方案：

```bash
python3 tools/make-ko-for-device.py \
  --template build/cpuinfo_redirect-kprobe.ko \
  --uname-r '目标设备 uname -r' \
  --config  目标 config.gz
```

---

## 六、和其他方案的实际对比

| | RVH | PROC | kprobe |
|---|---|---|---|
| 部署到新设备要编译吗 | 不用（改串） | 不用（改串） | **不用（改串）** |
| 依赖厂商 | **是** | 否 | **否** |
| 依赖结构布局 | 否 | **是（3 处）** | 否（1 处且稳定） |
| CFI 风险 | 无 | **有** | **无** |
| 崩溃风险 | 低 | **高** | 中（kprobe 失败返回错误，不会崩） |
| 被检测风险 | 低 | 中 | **中高**（kprobe 表可查） |
| 内核版本范围 | 只有带 hook 的 | 5.6+ | **5.10 ~ 6.12 全部** |

---

## 七、被检测的风险要说清

kprobe 有两个暴露面：

**① `/sys/kernel/debug/kprobes/list`**

需要挂 debugfs。多数量产设备默认不挂，但 root 后可以挂上来看。

**② `kprobe` 结构在内存里**

反作弊可以扫内核堆找 `kprobe` 结构。属于定制检测，通用工具不做。

**对比**：PROC 改的是 `proc_ops` 指针，遍历 PDE 或扫 kallsyms 也能发现；
RVH 最干净但覆盖窄。

---

## 八、建议的推进顺序

1. **先试现有 PROC/RVH 版在三台的实测**——如果 6.1 的 PROC 因 CFI 挂掉，就说明必须换路
2. **再实现 kprobe 版**——代码约 150 行，比 PROC 简洁（不用遍历红黑树）
3. **kprobe 版做成模板**，之后所有设备都用 `make-ko-for-device.py` 改串

**最终形态**：

```
一份 kprobe 版 .ko（编一次，任意机器）
    ↓
make-ko-for-device.py --uname-r <任意 5.10~6.12 设备>
    ↓
改 vermagic + 改 CRC 段名（几秒）
    ↓
直接 insmod，不需要重新编译
```

---

## 九、一句话

**RVH 靠运气（厂商有没有注册），PROC 靠布局（版本对不对得上），kprobe 靠内核自己（`show_cpuinfo` 一直在）。**

要通用，就得从「改数据结构」退到「在函数入口插一脚」——依赖的东西越少，能跑的设备越多。
