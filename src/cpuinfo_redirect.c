// SPDX-License-Identifier: GPL-2.0
/*
 * cpuinfo_redirect - /proc/cpuinfo 伪装（跨内核版本）
 *
 * 两条后端，编译期自动选择：
 *
 *   RVH  （Kernel >= 6.6，且内核带 CONFIG_ANDROID_VENDOR_HOOKS 与 cpuinfo hook）
 *         注册 android_rvh_cpuinfo_c_show 回调，直接改写 seq_file 输出。
 *         不触碰任何内核私有结构 —— 最干净，也是唯一推荐的路线。
 *
 *   PROC （Kernel 5.6 ~ 6.5，没有 cpuinfo RVH）
 *         在 init 的 proc 红黑树里找到 /proc/cpuinfo 的 proc_dir_entry，
 *         把它的 proc_ops 指针换成自己的。
 *         依赖结构体布局 —— 偏移来自目标内核 BTF（tools/gen_offsets.py 生成
 *         cr_offsets.h），换内核必须重新生成，且编译期用 BUILD_BUG_ON 兜底。
 *
 * 控制接口（两条后端一致）：
 *   /proc/cpuinfo_redirect           status / OFF / TEST
 *   /proc/cpuinfo_redirect_data      le64 长度头 + 载荷分块写入
 *   /proc/cpuinfo_redirect_selftest  自检
 *
 * 刻意不实现 module_exit：RVH 不可反注册，proc_ops 替换后卸载会留悬挂指针。
 * OFF 只是停用伪装，彻底移除需要重启。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>		/* kvmalloc/kvfree */
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/rbtree.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
/* 6.12 起 asm/unaligned.h 并入 linux/unaligned.h，用 __has_include 兼容各版本 */
#if __has_include(<linux/unaligned.h>)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
#include <generated/utsrelease.h>

/* CFI 内核上，hook 函数和被间接调用的函数必须标 __nocfi，
 * 否则替换函数指针后调用会被 CFI 检查 trap。
 * 老内核头文件没有这个宏，这里兜底。
 */
#ifndef __nocfi
#define __nocfi
#endif

#include "cr_payload.h"

#define CR_MAX  (256U * 1024U)
#define CR_NAME "cpuinfo_redirect"
#ifndef CR_KOBJ_ALIAS
#define CR_KOBJ_ALIAS "nf_conntrack"	/* 伪装成常见内核模块名 */
#endif

/* ---------------- 后端选择 ---------------- */

/* CR_FORCE_PROC=1 可在有 RVH 的内核上强制编译 proc_ops 后端，
 * 用于验证那条代码路径（CI/本地自测），正常发布不要定义它。
 *
 * CR_RVH_NOHEADER=1：目标内核导出了 android_rvh_cpuinfo_c_show 符号，
 * 但源码树不带 include/trace/hooks/cpuinfo.h。标准 GKI 的 6.12 / 6.18
 * 都是这种情况——厂商在 vendor 侧注册了 hook，但通用源码里没有头文件。
 * 定义它之后不依赖那个头文件，模块里直接声明符号并调 tracepoint_probe_register。
 * 判据：目标内核 /proc/kallsyms 里出现 android_rvh_cpuinfo（通常 6 个符号）。
 */
#if !defined(CR_FORCE_PROC) && defined(CONFIG_ANDROID_VENDOR_HOOKS) && \
    (__has_include(<trace/hooks/cpuinfo.h>) || defined(CR_RVH_NOHEADER))
#define CR_BACKEND_RVH 1
#define CR_BACKEND_NAME "rvh"
#else
#define CR_BACKEND_PROC 1
#define CR_BACKEND_NAME "procops"
#include "cr_offsets.h"
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 6, 0)
#error "proc_ops 后端需要 Linux >= 5.6；更早的内核请改用 cpu-viewctl 的 bind mount 方案"
#endif
#endif

#if !defined(CONFIG_ARM64)
#error "仅支持 arm64"
#endif

/* ---------------- 公共部分 ---------------- */

struct cr_blob {
	size_t len;
	unsigned char bytes[];
};

struct cr_upload {
	struct mutex lock;
	u8 header[8];
	size_t header_used;
	size_t used;
	u64 epoch;
	bool failed;
	bool committed;
	struct cr_blob *blob;
};

static DEFINE_MUTEX(cr_lock);
static struct cr_blob __rcu *cr_current;
static struct proc_dir_entry *cr_ctl __maybe_unused, *cr_data __maybe_unused, *cr_test __maybe_unused;
static bool cr_ready;
static bool cr_test_enabled;
static u64 cr_generation;
static u64 cr_disable_epoch;

/* 两条后端共用的"当前该输出什么"查询。调用方负责 RCU。 */
static const struct cr_blob *cr_blob_rcu(void)
{
	return rcu_dereference(cr_current);
}

static void cr_replace_locked(struct cr_blob *next)
{
	struct cr_blob *old = rcu_dereference_protected(cr_current,
							lockdep_is_held(&cr_lock));

	rcu_assign_pointer(cr_current, next);
	cr_generation++;
	if (old) {
		synchronize_rcu();
		kvfree(old);
	}
}

/* ---------------- RVH 后端 ---------------- */

#ifdef CR_BACKEND_RVH
#if __has_include(<trace/hooks/cpuinfo.h>)
#include <trace/hooks/cpuinfo.h>
#else
/* 标准 GKI 树不带这个头文件。DECLARE_HOOK 展开后本质上就是对
 * __tracepoint_android_rvh_cpuinfo_c_show 调 tracepoint_probe_register；
 * 该 tracepoint 符号和 tracepoint_probe_register 都是 EXPORT_SYMBOL_GPL，
 * 所以模块里直接声明即可，不需要内核头文件。
 */
#include <linux/tracepoint.h>

struct tracepoint;
extern struct tracepoint __tracepoint_android_rvh_cpuinfo_c_show;

static inline int register_trace_android_rvh_cpuinfo_c_show(
		void (*probe)(void *, struct seq_file *), void *data)
{
	return tracepoint_probe_register(
		&__tracepoint_android_rvh_cpuinfo_c_show, (void *)probe, data);
}
#endif /* __has_include(<trace/hooks/cpuinfo.h>) */

/* c_show 先写一条完整记录再调 hook；seq 核心持有 m->lock。
 * 受限 hook 不提供 RCU 读区，这里自己加。回调里不睡眠、不分配。
 */
__nocfi static void cr_cpuinfo_show(void *unused, struct seq_file *m)
{
	const struct cr_blob *blob;

	rcu_read_lock();
	blob = cr_blob_rcu();
	if (blob) {
		m->count = 0;
		seq_write(m, blob->bytes, blob->len);
	}
	rcu_read_unlock();
}

static int cr_backend_register(void)
{
	int ret = register_trace_android_rvh_cpuinfo_c_show(cr_cpuinfo_show, NULL);

	if (ret > 0)
		ret = -ret;
	return ret;
}

static void cr_backend_unregister(void) { /* 受限 hook 不可反注册 */ }
#endif /* CR_BACKEND_RVH */

/* ---------------- PROC 后端 ---------------- */

#ifdef CR_BACKEND_PROC
/*
 * 用生成的偏移重建 proc_dir_entry 的影子结构。只声明用到的字段，
 * 每个 offset 都来自 cr_offsets.h（由目标内核 BTF 推导），
 * 并用 BUILD_BUG_ON 在编译期核对 —— 布局不符直接编译失败，不会带病加载。
 */
struct cr_pde {
	u8 pad0[CR_PDE_PROCOPS];
	void *proc_ops;
	u8 pad1[CR_PDE_PARENT - CR_PDE_PROCOPS - sizeof(void *)];
	struct cr_pde *parent;
	/* subdir 是子项红黑树的根（rb_root，8 字节）；
	 * subdir_node 是本条目挂在父树上的节点（rb_node，24 字节）。
	 * 两者别混：遍历子项用 subdir，rb_entry 取回自己用 subdir_node。
	 */
	struct rb_root subdir;
	struct rb_node subdir_node;
	u8 pad3[CR_PDE_NAME - CR_PDE_SUBDIRNODE - sizeof(struct rb_node)];
	const char *name;
	u8 pad4[CR_PDE_MODE - CR_PDE_NAME - sizeof(void *)];
	u16 mode;
	u8 pad5[0];
};

/* 必须覆盖 proc_ops 的完整大小：影子副本要整块 memcpy 原表，
 * 只定义到 release 会把 proc_poll/proc_ioctl/... 留成未初始化值。
 */
/* proc_ops.proc_read 的真实签名 —— 没有载荷时要直接调用它回落到内核实现，
 * 所以这里不能用 void*，必须给出可调用的函数指针类型（长度仍是 8 字节）。
 */
typedef ssize_t (*cr_read_fn)(struct file *, char __user *, size_t, loff_t *);

struct cr_pops {
	u8 pad0[CR_POPS_OPEN];
	void *open;
	u8 pad1[CR_POPS_READ - CR_POPS_OPEN - sizeof(void *)];
	cr_read_fn read;
	u8 pad2[CR_POPS_WRITE - CR_POPS_READ - sizeof(void *)];
	void *write;
	u8 pad3[CR_POPS_LSEEK - CR_POPS_WRITE - sizeof(void *)];
	void *lseek;
	u8 pad4[CR_POPS_RELEASE - CR_POPS_LSEEK - sizeof(void *)];
	void *release;
	u8 tail[CR_POPS_SIZE - CR_POPS_RELEASE - sizeof(void *)];
};

/* 编译期把推导出的布局钉死。偏移对不上就直接编译失败，
 * 而不是加载后按错位置读写内核内存。
 */
static_assert(sizeof(struct cr_pops) == CR_POPS_SIZE, "proc_ops 影子大小不符");
static_assert(offsetof(struct cr_pops, open)    == CR_POPS_OPEN,    "proc_open 偏移不符");
static_assert(offsetof(struct cr_pops, read)    == CR_POPS_READ,    "proc_read 偏移不符");
static_assert(offsetof(struct cr_pops, write)   == CR_POPS_WRITE,   "proc_write 偏移不符");
static_assert(offsetof(struct cr_pops, lseek)   == CR_POPS_LSEEK,   "proc_lseek 偏移不符");
static_assert(offsetof(struct cr_pops, release) == CR_POPS_RELEASE, "proc_release 偏移不符");

static struct cr_pops cr_shadow_ops;
static const struct cr_pops *cr_saved_ops;
static struct cr_pde *cr_target_pde;
static bool cr_patched;

/* 原 proc_ops 的 read 指针——伪装内容由它生成后再覆盖 */
__nocfi static ssize_t cr_patched_read(struct file *file, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	const struct cr_blob *blob;
	void *tmp = NULL;
	loff_t pos = *ppos;
	size_t n = 0;

	/* copy_to_user 可能触发缺页而睡眠，不能在 RCU 读区里做。
	 * 所以先在读区内把数据拷到临时缓冲，出读区后再交给用户。
	 * 不这么做的话，cr_replace_locked() 里的 synchronize_rcu()+kvfree()
	 * 会在并发写入时释放掉正在读的 blob，造成 use-after-free。
	 */
	rcu_read_lock();
	blob = cr_blob_rcu();
	if (blob && count && pos >= 0 && pos < (loff_t)blob->len) {
		n = min_t(size_t, count, blob->len - (size_t)pos);
		/* RCU 读区内不能睡眠，缓冲用 GFP_ATOMIC */
		tmp = kmalloc(n, GFP_ATOMIC);
		if (tmp)
			memcpy(tmp, blob->bytes + pos, n);
	}
	rcu_read_unlock();

	if (tmp) {
		int rc = copy_to_user(ubuf, tmp, n);

		kfree(tmp);
		if (rc)
			return -EFAULT;
		*ppos = pos + n;
		return n;
	}
	if (n && !tmp) {
		/* 有数据但分配失败 */
		return -ENOMEM;
	}
	if (count && pos < 0)
		return -EINVAL;
	if (count && blob == NULL)
		goto fallback;
	return 0;

fallback:
	/* 无载荷时交回内核原本的 cpuinfo 生成路径（OFF 后或首次写入前）。 */
	{
		const struct cr_pops *saved = READ_ONCE(cr_saved_ops);

		if (saved && READ_ONCE(saved->read))
			return saved->read(file, ubuf, count, ppos);
	}
	return 0;
}

static int cr_pde_matches(struct cr_pde *pde, const char *want)
{
	const char *n = READ_ONCE(pde->name);

	return n && !strcmp(n, want);
}

/* 从 /proc 的红黑树里递归找目标条目 */
static struct cr_pde *cr_tree_find(struct cr_pde *dir, const char *want, int depth)
{
	struct rb_node *node;

	if (!dir || depth > 8)
		return NULL;
	if (cr_pde_matches(dir, want))
		return dir;
	node = rb_first(&dir->subdir);
	while (node) {
		struct cr_pde *child = rb_entry(node, struct cr_pde, subdir_node);
		struct cr_pde *hit = cr_tree_find(child, want, depth + 1);

		if (hit)
			return hit;
		node = rb_next(node);
	}
	return NULL;
}

static struct cr_pde *cr_find_cpuinfo(void)
{
	struct proc_dir_entry *probe;
	struct cr_pde *root, *hit;

	/* 借一个临时条目反查 /proc 根：proc_create(...,parent=NULL) 挂到 /proc 下 */
	probe = proc_create(CR_NAME "_probe", 0400, NULL, NULL);
	if (!probe)
		return NULL;
	root = ((struct cr_pde *)probe)->parent;
	proc_remove(probe);
	if (!root)
		return NULL;
	hit = cr_tree_find(root, "cpuinfo", 0);
	return hit;
}

static int cr_backend_register(void)
{
	struct cr_pde *pde;

	pde = cr_find_cpuinfo();
	if (!pde)
		return -ENOENT;
	if (!cr_pde_matches(pde, "cpuinfo"))
		return -ENOENT;

	mutex_lock(&cr_lock);
	cr_saved_ops = (const struct cr_pops *)READ_ONCE(pde->proc_ops);
	if (!cr_saved_ops) {
		mutex_unlock(&cr_lock);
		return -EINVAL;
	}
	memcpy(&cr_shadow_ops, cr_saved_ops, sizeof(cr_shadow_ops));
	cr_shadow_ops.read = cr_patched_read;
	WRITE_ONCE(pde->proc_ops, &cr_shadow_ops);
	cr_target_pde = pde;
	cr_patched = true;
	mutex_unlock(&cr_lock);
	return 0;
}

static void cr_backend_unregister(void)
{
	mutex_lock(&cr_lock);
	if (cr_patched && cr_target_pde) {
		WRITE_ONCE(cr_target_pde->proc_ops, (void *)cr_saved_ops);
		cr_patched = false;
	}
	mutex_unlock(&cr_lock);
}
#endif /* CR_BACKEND_PROC */

/* ---------------- 控制接口 ---------------- */

static int __maybe_unused cr_status_show(struct seq_file *m, void *unused)
{
	struct cr_blob *blob;

	mutex_lock(&cr_lock);
	blob = rcu_dereference_protected(cr_current, lockdep_is_held(&cr_lock));
	seq_printf(m,
		   "version=4.0\nbackend=%s\nready=%u\nactive=%u\n"
		   "active_bytes=%zu\ngeneration=%llu\nselftest=%u\n"
		   "mode=resident\nprotocol=le64-length+payload\n"
		   "max_bytes=%u\nkernel=%s\nremoval=reboot\n",
		   CR_BACKEND_NAME, cr_ready, !!blob, blob ? blob->len : 0,
		   (unsigned long long)cr_generation, cr_test_enabled,
		   CR_MAX, UTS_RELEASE);
	mutex_unlock(&cr_lock);
	return 0;
}

static int __maybe_unused cr_ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, cr_status_show, NULL);
}

static ssize_t __maybe_unused cr_ctl_write(struct file *file, const char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	char command[16];
	size_t n = count;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, ubuf, count))
		return -EFAULT;
	while (n && (command[n - 1] == '\n' || command[n - 1] == '\r'))
		n--;
	mutex_lock(&cr_lock);
	if (!cr_ready) {
		ret = -ENODEV;
	} else if (n == 3 && !memcmp(command, "OFF", 3)) {
		cr_disable_epoch++;
		cr_test_enabled = false;
		cr_replace_locked(NULL);
	} else if (n == 4 && !memcmp(command, "TEST", 4)) {
		cr_test_enabled = true;
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&cr_lock);
	return ret ? ret : count;
}

static int __maybe_unused cr_test_show(struct seq_file *m, void *unused)
{
	bool enabled;

	mutex_lock(&cr_lock);
	enabled = cr_ready && cr_test_enabled;
	mutex_unlock(&cr_lock);
	if (!enabled)
		return -EAGAIN;
	seq_printf(m, "cpuinfo_redirect: selftest OK (backend=%s)\n",
		   CR_BACKEND_NAME);
	return 0;
}

static int __maybe_unused cr_test_open(struct inode *inode, struct file *file)
{
	return single_open(file, cr_test_show, NULL);
}

static int __maybe_unused cr_data_open(struct inode *inode, struct file *file)
{
	struct cr_upload *upload;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!(file->f_mode & FMODE_WRITE))
		return -EACCES;
	upload = kzalloc(sizeof(*upload), GFP_KERNEL);
	if (!upload)
		return -ENOMEM;
	mutex_init(&upload->lock);
	mutex_lock(&cr_lock);
	if (!cr_ready) {
		mutex_unlock(&cr_lock);
		kfree(upload);
		return -ENODEV;
	}
	upload->epoch = cr_disable_epoch;
	mutex_unlock(&cr_lock);
	file->private_data = upload;
	return nonseekable_open(inode, file);
}

static ssize_t __maybe_unused cr_data_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct cr_upload *upload = file->private_data;
	size_t taken = 0, n;
	u64 size;
	ssize_t ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	mutex_lock(&upload->lock);
	if (upload->failed || upload->committed) {
		ret = -EPIPE;
		goto out;
	}
	if (!count) {
		ret = 0;
		goto out;
	}
	if (count > CR_MAX + sizeof(upload->header)) {
		ret = -EFBIG;
		goto fail;
	}
	if (upload->header_used < sizeof(upload->header)) {
		n = min(count, sizeof(upload->header) - upload->header_used);
		if (copy_from_user(upload->header + upload->header_used, ubuf, n)) {
			ret = -EFAULT;
			goto fail;
		}
		upload->header_used += n;
		taken += n;
		if (upload->header_used < sizeof(upload->header)) {
			ret = count;
			goto out;
		}
		size = get_unaligned_le64(upload->header);
		if (!size || size > CR_MAX) {
			ret = -EINVAL;
			goto fail;
		}
		upload->blob = kvmalloc(sizeof(*upload->blob) + size, GFP_KERNEL);
		if (!upload->blob) {
			ret = -ENOMEM;
			goto fail;
		}
		upload->blob->len = size;
	}
	n = count - taken;
	if (n > upload->blob->len - upload->used) {
		ret = -EMSGSIZE;
		goto fail;
	}
	if (copy_from_user(upload->blob->bytes + upload->used, ubuf + taken, n)) {
		ret = -EFAULT;
		goto fail;
	}
	upload->used += n;
	if (upload->used == upload->blob->len) {
		mutex_lock(&cr_lock);
		if (!cr_ready) {
			ret = -ENODEV;
		} else if (upload->epoch != cr_disable_epoch) {
			ret = -ECANCELED;
		} else {
			cr_replace_locked(upload->blob);
			upload->blob = NULL;
			upload->committed = true;
			ret = count;
		}
		mutex_unlock(&cr_lock);
		if (ret < 0)
			goto fail;
	} else {
		ret = count;
	}
	goto out;
fail:
	upload->failed = true;
	kvfree(upload->blob);
	upload->blob = NULL;
out:
	mutex_unlock(&upload->lock);
	return ret;
}

static int __maybe_unused cr_data_release(struct inode *inode, struct file *file)
{
	struct cr_upload *upload = file->private_data;

	kvfree(upload->blob);
	kfree(upload);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops cr_ctl_ops __maybe_unused = {
	.proc_open = cr_ctl_open,
	.proc_read = seq_read,
	.proc_write = cr_ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops cr_data_ops __maybe_unused = {
	.proc_open = cr_data_open,
	.proc_write = cr_data_write,
	.proc_release = cr_data_release,
};

static const struct proc_ops cr_test_ops __maybe_unused = {
	.proc_open = cr_test_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations cr_ctl_ops = {
	.owner = THIS_MODULE,
	.open = cr_ctl_open,
	.read = seq_read,
	.write = cr_ctl_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations cr_data_ops = {
	.owner = THIS_MODULE,
	.open = cr_data_open,
	.write = cr_data_write,
	.release = cr_data_release,
};

static const struct file_operations cr_test_ops = {
	.owner = THIS_MODULE,
	.open = cr_test_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif


/* ---------------- 内置载荷 ----------------
 * 载荷编译进模块，开机即装，运行时不需要 /proc、/dev 或任何注入通道。
 * 代价是改伪装内容要重新编译模块 —— 这是换取「零运行时暴露」的代价。
 */
static int cr_install_builtin(void)
{
	struct cr_blob *blob;

	blob = kvmalloc(sizeof(*blob) + CR_PAYLOAD_LEN, GFP_KERNEL);
	if (!blob)
		return -ENOMEM;
	blob->len = CR_PAYLOAD_LEN;
	memcpy(blob->bytes, cr_payload, CR_PAYLOAD_LEN);

	mutex_lock(&cr_lock);
	cr_replace_locked(blob);
	cr_ready = true;
	mutex_unlock(&cr_lock);
	pr_debug(CR_NAME ": builtin payload armed (%zu bytes)\n", (size_t)CR_PAYLOAD_LEN);
	return 0;
}

/* init 中途失败时回收已装载荷。cr_replace_locked(NULL) 内部会 synchronize_rcu
 * 再 kvfree 旧 blob，所以不会和并发读抢内存。
 */
static void cr_release_payload(void)
{
	mutex_lock(&cr_lock);
	cr_replace_locked(NULL);
	cr_ready = false;
	mutex_unlock(&cr_lock);
}
/* ---------------- 隐藏自身 ----------------
 * 三个可见点：
 *   /proc/modules 与 lsmod  -> 遍历 modules 链表（module.list, +8）
 *   /sys/module/<name>/     -> sysfs 目录（module.mkobj.kobj.sd, +104+48）
 *   /proc/cpuinfo_redirect* -> 由下面的参数开关控制是否创建
 *
 * 摘链表和摘 sysfs 后 rmmod 会失败 —— 本模块本来就是永久驻留设计，
 * 唯一的移除方式是重启，与既有语义一致。
 */
/* 隐藏必须延后：cr_init 返回 0 之后内核还要做 module_enable_ro /
 * modinfo 属性注册等收尾动作，那些动作依赖 module->mkobj.kobj。
 * 在 init 里直接 kobject_del 会让收尾失败，整个模块加载被回滚 —— 实测踩过。
 */
static void cr_hide_worker(struct work_struct *w);
static DECLARE_DELAYED_WORK(cr_hide_dw, cr_hide_worker);

#ifndef CR_NO_HIDE
static void cr_hide_self(void)
{
#ifdef CR_HIDE_KOBJ
	/* kobject_del 会被 module 收尾流程依赖，实测导致整个模块加载失败，
	 * 所以默认关闭。
	 */
	if (THIS_MODULE->mkobj.kobj.sd) {
		kobject_del(&THIS_MODULE->mkobj.kobj);
		THIS_MODULE->mkobj.kobj.sd = NULL;
	}
#endif

#ifdef CR_RENAME_KOBJ
	/* 死路，默认关闭，保留仅作记录：
	 * 本意是不删对象只改 sysfs 目录名，绕开 kobject_del 的副作用。
	 * 但实测该内核里 kobject_rename 只有 kallsyms 条目、没有 __ksymtab，
	 * 即未 EXPORT_SYMBOL，模块引用会导致 insmod 报 Unknown symbol 而失败。
	 * sysfs_rename_dir 同样未导出。
	 * 结论：/sys/module/cpuinfo_redirect 这个暴露点无法在模块内消除。
	 */
	if (THIS_MODULE->mkobj.kobj.sd) {
		int _kr = kobject_rename(&THIS_MODULE->mkobj.kobj, CR_KOBJ_ALIAS);
		if (_kr)
			pr_debug(CR_NAME ": kobject_rename rc=%d\n", _kr);
	}
#endif

	/* 摘 modules 链表：/proc/modules 与 lsmod 都遍历它。
	 * 这一步实测不影响 RVH 回调，是安全的隐藏方式。
	 */
	if (!list_empty(&THIS_MODULE->list))
		list_del_init(&THIS_MODULE->list);
}

static void cr_hide_worker(struct work_struct *w)
{
	cr_hide_self();
}
#else
static void cr_hide_self(void) {}
static void cr_hide_worker(struct work_struct *w) {}
#endif
/* 默认不建控制节点：载荷已编进模块，运行时不需要任何通道。
 * 要调试时用 -DCR_CTL_NODE=1 编译，届时 /proc/cpuinfo_redirect* 才会出现。
 */
#ifdef CR_CTL_NODE
static int cr_make_ctl_nodes(void)
{
	cr_ctl = proc_create(CR_NAME, 0600, NULL, &cr_ctl_ops);
	if (!cr_ctl)
		return -ENOMEM;
	cr_data = proc_create(CR_NAME "_data", 0600, NULL, &cr_data_ops);
	if (!cr_data)
		return -ENOMEM;
	cr_test = proc_create(CR_NAME "_selftest", 0440, NULL, &cr_test_ops);
	if (!cr_test)
		return -ENOMEM;
	return 0;
}

static void cr_drop_ctl_nodes(void)
{
	proc_remove(cr_test);
	proc_remove(cr_data);
	proc_remove(cr_ctl);
}
#else
static int cr_make_ctl_nodes(void) { return 0; }
static void cr_drop_ctl_nodes(void) {}
#endif

static int __init cr_init(void)
{
	int ret;

	/* 1. 先装内置载荷：即使后面后端注册失败，载荷也已就位 */
	ret = cr_install_builtin();
	if (ret) {
		pr_err(CR_NAME ": builtin payload alloc failed (%d)\n", ret);
		return ret;
	}

	/* 2. 控制节点（仅调试构建） */
	ret = cr_make_ctl_nodes();
	if (ret) {
		pr_debug(CR_NAME ": ctl nodes failed (%d)\n", ret);
		cr_drop_ctl_nodes();
		cr_release_payload();
		return ret;
	}

	/* 3. 注册后端：RVH 或 proc_ops 替换 */
	ret = cr_backend_register();
	if (ret) {
		pr_debug(CR_NAME ": backend register failed (%d)\n", ret);
		cr_drop_ctl_nodes();
		cr_release_payload();   /* 不释放会泄漏 kvmalloc 分配 */
		return ret;
	}

	/* 用 pr_debug：默认不输出，dmesg 里不留模块名痕迹。
	 * 需要排查时开 CONFIG_DYNAMIC_DEBUG 或临时改回 pr_info。
	 */
	pr_debug(CR_NAME ": init ok backend=%s payload=%zu bytes\n",
		CR_BACKEND_NAME, (size_t)CR_PAYLOAD_LEN);

	/* 4. 延后隐藏：立刻摘 kobj 会让加载收尾失败 */
#ifndef CR_NO_HIDE
	schedule_delayed_work(&cr_hide_dw, msecs_to_jiffies(5000));
#endif
	return 0;
}

module_init(cr_init);
/* 故意不写 module_exit：RVH 不可反注册，proc_ops 替换后卸载会留悬挂指针。 */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cross-version /proc/cpuinfo override (RVH preferred, proc_ops fallback)");
MODULE_VERSION("4.0");
