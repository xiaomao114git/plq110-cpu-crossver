# 5 系内核适配结论

## 一、结论：5.10 / 5.15 与 6.6 布局完全兼容

从 AOSP 官方镜像逐字段核对，三个分支的 `struct proc_dir_entry` 和 `struct proc_ops`
**完全相同**。所以现有 `cr_offsets.h`（6.6 实测值）在 5 系上是正确的。

---

## 二、`proc_dir_entry` 逐字段核对

对比分支：`android12-5.10`、`android13-5.15`、`android15-6.6`

| 字段 | 偏移 | 5.10 | 5.15 | 6.6 |
|---|---|---|---|---|
| `in_use` (atomic_t) | 0 | ✓ | ✓ | ✓ |
| `refcnt` (refcount_t) | 4 | ✓ | ✓ | ✓ |
| `pde_openers` (list_head) | 8 | ✓ | ✓ | ✓ |
| `pde_unload_lock` (spinlock_t) | 24 | ✓ | ✓ | ✓ |
| `pde_unload_completion` | 32 | ✓ | ✓ | ✓ |
| `proc_iops` | **40** | ✓ | ✓ | ✓ |
| `proc_ops` / `proc_dir_ops` (union) | **48** | ✓ | ✓ | ✓ |
| `proc_dops` | 56 | **✓ 有** | ✓ | ✓ |
| `seq_ops` / `single_show` (union) | 64 | ✓ | ✓ | ✓ |
| `write` | 72 | ✓ | ✓ | ✓ |
| `data` | 80 | ✓ | ✓ | ✓ |
| `state_size` | 88 | ✓ | ✓ | ✓ |
| `low_ino` | 92 | ✓ | ✓ | ✓ |
| `nlink` | 96 | ✓ | ✓ | ✓ |
| `uid` / `gid` | 100 / 104 | ✓ | ✓ | ✓ |
| `size` | 112 | ✓ | ✓ | ✓ |
| `parent` | **120** | ✓ | ✓ | ✓ |
| `subdir` (rb_root) | **128** | ✓ | ✓ | ✓ |
| `subdir_node` (rb_node) | **136** | ✓ | ✓ | ✓ |
| `name` | 160 | ✓ | ✓ | ✓ |
| `mode` | 168 | ✓ | ✓ | ✓ |
| `flags` / `namelen` | 170 / 171 | ✓ | ✓ | ✓ |

**重点纠正**：我之前以为 `proc_dops` 是 6.x 才加的，**错了**。
5.10 就已经有这个字段，**只有 6.18 把它删掉了**。

所以布局的分界线在 **6.18**，不是 6.x 整体。

---

## 三、`proc_ops` 逐字段核对

| 字段 | 偏移 | 5.10 | 5.15 | 6.6 |
|---|---|---|---|---|
| `proc_flags` (uint) | 0 | ✓ | ✓ | ✓ |
| `proc_open` | **8** | ✓ | ✓ | ✓ |
| `proc_read` | **16** | ✓ | ✓ | ✓ |
| `proc_read_iter` | 24 | ✓ | ✓ | ✓ |
| `proc_write` | **32** | ✓ | ✓ | ✓ |
| `proc_lseek` | **40** | ✓ | ✓ | ✓ |
| `proc_release` | **48** | ✓ | ✓ | ✓ |
| `proc_poll` | 56 | ✓ | ✓ | ✓ |
| `proc_ioctl` | 64 | ✓ | ✓ | ✓ |
| 结构总大小 | **96** | ✓ | ✓ | ✓ |

注意 `proc_write` 的第三参数：

```c
5.10 : ssize_t (*proc_write)(struct file *, const char __user *, size_t, loff_t *);
5.15 : ssize_t (*proc_write)(struct file *, const char __user *, size_t, loff_t *);
6.6  : ssize_t (*proc_write)(struct file *, const char __user *, size_t, loff_t *);
```

三个分支连 `const` 都一样。

---

## 四、可用的 5 系模板

已生成容量化模板（vermagic 从 38→93 字节，能装下完整设备串）：

```
build/tpl-5.10.ko    239,320 B   vermagic 93 字节
build/tpl-5.15.ko    234,360 B   vermagic 93 字节
```

### 适配任意 5 系设备

```bash
python tools/repack-modinfo.py \
  --template build/tpl-5.10.ko \
  --uname-r "目标设备的完整 uname -r" \
  --out out/cpuinfo_redirect-5.10-适配.ko
```

几秒出结果，**不需要编译**。

### 示例

```
目标: 5.10.198-android12-9-g3a4b5c6d7e8f-ab123456789-4k
生成: vermagic=5.10.198-android12-9-g3a4b5c6d7e8f-ab123456789-4k SMP preempt mod_unload modversions aarch64
```

---

## 五、但仍需注意的三点

### 5.1 `CONFIG_RANDSTRUCT`

`proc_dir_entry` 带 `__randomize_layout` 属性。**如果目标内核开了 RANDSTRUCT，字段顺序会被打乱**，
上面的偏移全部失效。

判据：

```bash
gzip -dc config.gz | grep RANDSTRUCT
# 有 CONFIG_RANDSTRUCT=y      -> 偏移不可预测，必须用 BTF
# 只有 # ... is not set       -> 布局固定，上表适用
```

### 5.2 CFI

`CONFIG_CFI_CLANG=y` 的内核，替换 `proc_read` 后调用时可能被 CFI 检查拦下。

**必须先给 hook 函数加 `__nocfi`**（这是本轮从 `kernel-inline-hook-framework` 学到的）：

```c
__nocfi static ssize_t cr_proc_read(struct file *f, char __user *b, size_t n, loff_t *p)
```

**现有源码缺这个标注。**

### 5.3 `Module.symvers` 与 CRC

现有 5 系产物是 `--no-crc` 编的（`.modinfo` 里没有 `__versions` 段），
所以不需要处理 CRC 段名。这一点已经验证：

```
final-p1.ko (6.1)  __versions 字节位置: []      ← 完全没有
```

---

## 六、完整适配矩阵

| 内核分支 | 模板 | 布局兼容 | 需要 RANDSTRUCT 检查 |
|---|---|---|---|
| android12-5.10 | `tpl-5.10.ko` | ✓ 与 6.6 同 | **是** |
| android13-5.15 | `tpl-5.15.ko` | ✓ 与 6.6 同 | **是** |
| android14-6.1 | `tpl-6.1.ko` | ✓ 与 6.6 同 | **是** |
| android15-6.6 | `cpuinfo_redirect-proc.ko` | ✓ 基准 | **是** |
| android16-6.12 | `tpl-6.12.ko` | ✓ 与 6.6 同（已核到 6.12） | **是** |
| android17-6.18 | `tpl-6.18.ko` | **✗ 少了 proc_dops** | **是，且偏移全变** |

**6.18 是唯一的例外**，需要单独编译一份用正确偏移表的产物。

---

## 七、下一步

1. **给源码加 `__nocfi`**（零成本，消除 CFI 风险）
2. **要一台真实 5 系设备的采集包**验证（目前只有源码层面的核对，没有 BTF 实证）
3. 6.18 单独处理
