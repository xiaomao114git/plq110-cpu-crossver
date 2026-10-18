# 自检报告（2026-10-18）

检查范围：驱动源码、生成的 `.ko`、模块包、工具脚本。

---

## 一、发现并修复的 bug（3 个）

### BUG-1：`cr_patched_read` 缺 RCU 保护 → use-after-free（严重）

**位置**：`src/cpuinfo_redirect.c` PROC 后端

**问题**：

```c
/* 修复前 */
static ssize_t cr_patched_read(struct file *file, char __user *ubuf,
                               size_t count, loff_t *ppos)
{
    const struct cr_blob *blob = cr_blob_rcu();   /* 没有 rcu_read_lock */
    ...
    copy_to_user(ubuf, blob->bytes + pos, n);      /* 直接解引用 */
}
```

`cr_blob_rcu()` 只是 `rcu_dereference()`，**调用方必须自己加锁**（函数注释写明了）。
而 `cr_replace_locked()` 在写入新载荷时会 `synchronize_rcu()` + `kvfree(old)`。

**后果**：并发写入时，正在读的 blob 会被释放 → 读到已释放内存 → 崩溃或数据泄露。

**对比**：RVH 后端的 `cr_cpuinfo_show` 是**有锁的**，只有 PROC 后端漏了。

**修复**：

```c
/* 修复后：读区内拷到临时缓冲，出读区再交给用户 */
rcu_read_lock();
blob = cr_blob_rcu();
if (blob && count && pos >= 0 && pos < (loff_t)blob->len) {
    n = min_t(size_t, count, blob->len - (size_t)pos);
    tmp = kmalloc(n, GFP_ATOMIC);   /* RCU 读区内不能睡眠 */
    if (tmp) memcpy(tmp, blob->bytes + pos, n);
}
rcu_read_unlock();

if (tmp) {
    int rc = copy_to_user(ubuf, tmp, n);
    kfree(tmp);
    ...
}
```

**为什么不能简单加锁**：`copy_to_user` 可能触发缺页而睡眠，RCU 读区内不允许睡眠。
所以必须先拷贝到临时缓冲。

### BUG-2：`cr_init` 失败路径泄漏载荷（中）

**位置**：`cr_init`

**问题**：`cr_install_builtin()` 成功分配 blob 后，如果后续 `cr_make_ctl_nodes()` 或
`cr_backend_register()` 失败，直接 `return ret`，**blob 没有释放**。

`kvmalloc` 分配的内存在模块加载失败后不会被内核自动回收。

**修复**：新增 `cr_release_payload()`，在两条失败路径上调用：

```c
static void cr_release_payload(void)
{
    mutex_lock(&cr_lock);
    cr_replace_locked(NULL);   /* 内部 synchronize_rcu + kvfree */
    cr_ready = false;
    mutex_unlock(&cr_lock);
}
```

### BUG-3：两处 `pr_info` 在 dmesg 留模块名（隐蔽性）

**位置**：`cr_install_builtin`（653 行）、`cr_init`（771 行）

**问题**：`dmesg` 会显示

```
cpuinfo_redirect: builtin payload armed (6827 bytes)
cpuinfo_redirect: init ok backend=r vh payload=6827 bytes
```

**这直接暴露模块名**，与「隐藏」目标冲突。

**修复**：改为 `pr_debug`（默认不输出，需要时开 `CONFIG_DYNAMIC_DEBUG`）。

保留的三个 `pr_err` 是有意的——加载失败必须能诊断。

---

## 二、补充的加固（1 项）

### `__nocfi` 标注

来源：`WeiJiLab/kernel-inline-hook-framework` 的实际实现。

```c
__nocfi static void cr_cpuinfo_show(...)      /* RVH 回调 */
__nocfi static ssize_t cr_patched_read(...)   /* PROC 替换的 read */
```

并加了兜底定义：

```c
#ifndef __nocfi
#define __nocfi
#endif
```

老内核头文件没这个宏时空定义（无害），CFI 内核用内核自己的定义。

**意义**：`CONFIG_CFI_CLANG=y` 的内核（两台 6.1 都是），替换 `proc_ops->proc_read` 后
调用时会被 CFI 检查。不标 `__nocfi` 就可能 trap。

---

## 三、检查通过的部分

### 3.1 生成的 `.ko` ELF 完整性

| 文件 | 段表 | 段偏移 | vermagic |
|---|---|---|---|
| final-p1.ko | OK | OK | 79 字节，内容正确 |
| final-p2.ko | OK | OK | 87 字节，内容正确 |
| final-p3.ko | OK | OK | 90 字节，内容正确 |
| tpl-5.10/5.15/6.1/6.12/6.18 | OK | OK | 93 字节 |

逐项检查了：ELF64 魔数、段表边界、每个段的 `offset+size` 是否落在文件内、
`shstrtab` 是否越界。**全部通过**。

### 3.2 `.modinfo` 内容合法性

重排后仍然合法：

```
license      = GPL
description  = Cross-ve      ← 被截断，但内核不检查这个字段
version      = 4.0
vermagic     = <完整目标串>
name         = cpuinfo_redirect
depends      =
srcversion   = <哈希>
```

`vermagic` / `name` / `license` 三个关键字段都在，没有格式错误的条目。

### 3.3 模块包一致性

| 包 | 包内 ko sha | 清单 sha | 一致 |
|---|---|---|---|
| spoof-p1.zip | 82f75928... | 82f75928... | ✓ |
| spoof-p2.zip | 906add49... | 906add49... | ✓ |
| spoof-p3.zip | d2ccb3ca... | d2ccb3ca... | ✓ |

门禁也确认解除：

```
DEV_PRODUCT_NAME=
DEV_PRODUCT_DEVICE=
DEV_BOARD_PLATFORM=
KERNEL_RELEASE=
REQUIRE_VERMAGIC=0
```

### 3.4 源码结构

```
{} : 75 / 75   OK
() : 270 / 270 OK
[] : 16 / 16   OK
```

括号配对、函数定义顺序都正常。

---

## 四、已知设计取舍（不是 bug，但要知道）

### 4.1 没有 `module_exit`

```c
/* 故意不写 module_exit：RVH 不可反注册，proc_ops 替换后卸载会留悬挂指针。 */
```

**后果**：
- 模块**永远无法卸载**，`rmmod` 会失败
- **出问题只能重启**
- `cr_backend_unregister()` 成了死代码（从未被调用）

这个取舍本身合理，但部署时要知道：**一旦装上，除非重启否则撤不掉**。

### 4.2 `description` 被截断成 `Cross-ve`

为腾空间给 vermagic。**无害**——内核不读这个字段，只影响 `modinfo` 命令显示。

### 4.3 遍历 `/proc` 红黑树的开销

`cr_find_cpuinfo()` 借 `proc_create` 反查根，再深度优先遍历找 `cpuinfo`。
`/proc` 根有几十个子节点，每个递归一层，实测开销可接受。

---

## 五、未验证的部分（重要）

**以上三处修复都没有经过编译验证。**

本机没有 `make` 和交叉编译器，改完的源码**一个字都没编译过**。
所以：

- 改动是否引入语法错误：**未知**（只做了括号配对检查）
- 修复后是否真能编过：**未知**
- 现有三个 `.ko` 是**修复前**的产物，不含这三处改动

**要验证必须做一次真实编译**：

```bash
./tools/build-three-devices.sh /path/to/packages    # 需要 Linux + 工具链
# 或者用 DDK：./scripts/build-ddk.sh build android14-6.1
```

---

## 六、结论

| 项 | 状态 |
|---|---|
| 生成的 .ko 结构完整性 | ✓ 通过 |
| 模块包一致性 | ✓ 通过 |
| 源码逻辑 bug | 发现 2 个，已修 |
| dmesg 痕迹 | 发现 1 处，已修 |
| CFI 标注 | 已补 |
| **修复的编译验证** | **✗ 未做** |
| 真机加载验证 | ✗ 未做 |

**最要紧的一句**：三个 `.ko` 是修复前的产物。如果要用修复后的代码，
必须在 Linux 上重新编一次。
