# 从 gs.awesome.rip 学到的（第二轮）

分类：Cheat / Android Kernel driver development，8 个项目里挖出的可用技术。

---

## 一、最重要的四条

### 1. `__nocfi` 是必须的（修正我之前的判断）

来源：`WeiJiLab/kernel-inline-hook-framework` 的 `linux/sample/hook_vfs_read.c`

```c
__nocfi ssize_t hook_vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    char *origin = RESUME_FUNC_ADDERSS(vfs_read);
    ...
    return ((ssize_t (*)(...))origin)(file, buf, count, pos);
}
```

**我之前说「签名匹配就不用 nocfi」是错的。** 参考实现明确标了 `__nocfi`。

**意义**：任何 hook 函数、任何通过函数指针调用原函数的地方，都必须加这个属性。
我们的 PROC 后端替换 `proc_ops->proc_read`，**必须加**；
如果走 inline hook 路线，**同样必须加**。

### 2. kprobe 有黑名单（6.1+）

来源：`fuqiuluo/android-wuwa` 的功能列表

```
[x] Kprobe Blacklist Disable - Clear kprobe blacklist to allow hooking
    protected kernel functions (kernel 6.1+)
```

**这直接影响我之前提出的 kprobe 方案。** 6.1 以后内核有 kprobe 黑名单，
`show_cpuinfo` 这类函数可能在黑名单里，`register_kprobe` 会直接返回失败。

**wuwa 的做法**：清除黑名单表。但具体怎么清，README 没写，需要看源码。

**对我们的意义**：kprobe 方案在 6.1 上**不一定能直接跑**，得先处理黑名单。

### 3. CFI 可以 patch 掉

来源：同上

```
[x] CFI Bypass - Automatically patch kernel CFI check functions
    to disable Control Flow Integrity protection
```

**它 patch 的是内核里的 CFI 检查函数本身**，不是给每个 hook 加标注。

两条路的区别：

| 做法 | 需要改内核 | 效果 |
|---|---|---|
| `__nocfi` 标注 | 否 | 只对标注过的函数生效 |
| patch CFI 检查函数 | **是（boot.img）** | 全局失效 |

**优先用 `__nocfi`**，改内核风险太高。

### 4. ABI symbols 可以在编译时导出

来源：`systemnb/compile_android_driver`（kade）

```yaml
abi:
  upstream_patch: true
  symbols_file: "${GITHUB_WORKSPACE}/abi.symbols"
```

```
# abi.symbols
register_kprobe
unregister_kprobe
kallsyms_lookup_name
```

**kade 会自动给内核源码打 patch，把我需要的符号加进 `EXPORT_SYMBOL`。**

**这解决了「符号未导出」这个根本问题**，而且**不需要改 boot.img**——
因为改的是**编译时的源码**，产出的内核本来就是你自己的。

**但前提**：你得能编译整个内核（不只是模块）。

---

## 二、inline hook 框架的可用性

### 实测覆盖

`WeiJiLab/kernel-inline-hook-framework` 的 README 写明：

```
Tested can work on 6.12.52-android16-6-maybe-dirty-4k kernel within cutterfish VM,
and AOSP - KonstaKANG (Android 16) 6.12.77-g55b572d3ed0e-v8 kernel on raspberry pi4.
Kernel version 6.18+
```

**正好覆盖 6.12 这条线**，和 p3 的 `6.12.38-android16-5-...-4k` 同代。

### 核心 API

```c
HOOK_FUNC_TEMPLATE(show_cpuinfo);                  /* 生成 trampoline */

__nocfi int hook_show_cpuinfo(struct seq_file *m, void *v)
{
    int (*origin)(struct seq_file *, void *) = RESUME_FUNC_ADDERSS(show_cpuinfo);
    /* 改 m->count 和内容 */
    return origin(m, v);
}

/* 注册 */
fn = find_func("show_cpuinfo");
HIJACK_TARGET_PREP_HOOK(fn, show_cpuinfo);
hijack_target_enable(fn);

/* 卸载：自动恢复原指令 */
hijack_target_disable(fn, true);
```

### 相比自己写 inline hook 的优势

自己写要处理：
- ARM64 远跳（`ldr x16,#8; br x16`，16 字节覆盖）
- 指令边界对齐（4 字节对齐，不能跨函数）
- 代码段写保护（`set_memory_rw` / `text_poke`）
- trampoline 跳回（原指令 + 跳回原函数 + 偏移）
- 并发安全（改代码时其他 CPU 可能正在执行）

**这个框架全都处理了**，而且实测过。

### 但它自己的 vermagic 也要改

README 原文：

```
Sometimes you will find the vermagic of hookFrame.ko and hookFrameTest.ko
different from your target kernel. You can pass the target kernel's vermagic:

$ sudo apt-get install bbe
$ make arm64 KDIR=... vermagic="4.14.98 SMP preempt mod_unload modversions aarch64"
```

**和我做的改串是同一件事**，只是它用的是 `bbe` 工具。

---

## 三、其他项目里的东西

### Diamorphine（hide module 参考）

`m0nad/Diamorphine`，支持 2.6 ~ 6.x / ARM64，**加载后立即不可见**。

手法是经典的：摘 `modules` 链表 + 摘 sysfs kobject。

**我们的现状**：已经用 `list_del_init` 摘了链表，但 `/sys/module/cpuinfo_redirect` 还在。

**之前试过 `kobject_del` 会导致模块加载回滚**（`kallsyms` 无符号、`dmesg` 无日志）。

**Diamorphine 可能用的是不同时机或不同 API**，值得看它的源码。

### ovo 的自定义协议族

`fuqiuluo/ovo` 和 `android-wuwa` 都用 **自定义 socket 协议族**做用户态通信：

> By implementing a custom network protocol family, user space communication can be
> achieved both synchronously and asynchronously **without relying on character
> devices or netlinks**. No filesystem operations required.

**比 `/proc` 节点隐蔽一个量级**——`/proc` 下多一个文件 `ls` 就看得到，
自定义 `AF_XXX` 协议族在用户态完全不可见。

### DDK（容器化构建）

`Ylarod/ddk` 提供 Docker 镜像，预配置内核源码：

```bash
sudo curl -fsSL https://raw.githubusercontent.com/Ylarod/ddk/main/scripts/ddk -o /usr/local/bin/ddk
./scripts/build-ddk.sh build android14-6.1
```

**这解决了「没有 Linux 环境」的问题**——只要有 Docker 就行。

---

## 四、对当前工程的直接影响

| 学到的 | 影响 |
|---|---|
| `__nocfi` 必须加 | **现有源码缺这个，PROC 后端要补** |
| kprobe 黑名单 | kprobe 方案在 6.1 上要多一步 |
| kade 的 ABI patch | 能彻底解决符号未导出，但要能编内核 |
| inline hook 框架实测 6.12 | p3 多一条备选路 |
| DDK | 解决编译环境问题 |

---

## 五、优先级建议

**立刻能做的**：给现有 PROC 后端加 `__nocfi`

```c
__nocfi static ssize_t cr_proc_read(struct file *f, char __user *b, size_t n, loff_t *p)
```

零成本，可能直接解决 6.1 的 CFI 风险。

**其次**：用 DDK 编一次 6.1 的真产物（有 Docker 即可）

**再次**：如果需要彻底通用，评估 inline hook 框架 + `__nocfi`

**最后**：kprobe 方案降级为备选（黑名单是额外障碍）
