// SPDX-License-Identifier: GPL-2.0
/*
 * ki_guard.c - 内核拦截：按 uid 隔离设备标记持久化分区 + 压制厂商安全上报
 *
 * 设计要点
 * --------
 * 1. 只对目标 uid 生效。hook 入口先做 O(n) 的 uid 命中（n <= 32），
 *    未命中立即返回，其余进程零影响。
 *
 * 2. 目录按 (s_dev, i_ino) 缓存比对，O(1)，不在 hook 里遍历路径。
 *    目标目录在模块初始化时用 kern_path() 解析；目录尚未挂载时由
 *    delayed_work 每 5 秒重试，最多 24 次（约 2 分钟）。
 *
 * 3. 只拦目标目录本身的 inode_permission。进入目录必须先拿到 MAY_EXEC，
 *    所以拦这一层就覆盖整个子树，不需要逐级比对后代。
 *    返回 -ENOENT 而非 -EACCES：让上层认为"不存在"，不是"没权限"。
 *
 * 4. 厂商上报点用 pre_handler 返回 1 直接跳过原函数执行，
 *    而不是 kretprobe 改返回值 —— 改返回值挡不住已经发出去的上报。
 *    要 hook 的符号名做成模块参数，设备上确认实际符号后改参数即可，不必重编。
 *
 * 5. 符号不存在时跳过并记日志，不阻塞加载。所以本模块在非 OPPO/一加
 *    设备上只做目录隐藏，仍可正常工作。
 *
 * 6. hide=1 时摘除 module 链表与 sysfs 目录，与 cpuinfo_spoof 同手法。
 *    默认 0（可观测），验证通过后再开。因为本模块没有修改任何外部
 *    函数指针，卸载路径是干净的，不需要像 cpuinfo_spoof 那样"只能重启清除"。
 */

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/list.h>

#define KI_MAX_UIDS   32
#define KI_MAX_DIRS   8
#define KI_MAX_SUPPS  8

/* ---- 模块参数 ---- */

static char *uids = "";
module_param(uids, charp, 0644);
MODULE_PARM_DESC(uids, "逗号分隔的目标 uid 列表，例如 10234,10235");

static char *dirs = "/mnt/vendor/persist/data,/mnt/vendor/nvdata";
module_param(dirs, charp, 0644);
MODULE_PARM_DESC(dirs, "逗号分隔的要隐藏的目录，例如 /mnt/vendor/persist/data");

static char *suppress = "report_security_event,oplus_report_execveat_new";
module_param(suppress, charp, 0644);
MODULE_PARM_DESC(suppress, "逗号分隔的厂商上报函数名，这些函数将被跳过执行");

static unsigned int hide = 0;
module_param(hide, uint, 0644);
MODULE_PARM_DESC(hide, "1=从 /proc/modules 与 /sys/module 摘除自身");

/* 只读状态出口：供 ki.sh 判断是否真的生效，而不是只看 insmod 返回码。
 * hide=1 时 sysfs 目录已被摘除，这两个值不再可读 —— 那时只能靠 dmesg。 */
static unsigned int ndirs_resolved;
module_param(ndirs_resolved, uint, 0444);
MODULE_PARM_DESC(ndirs_resolved, "已成功解析的目标目录数");

static unsigned int nsupp_armed;
module_param(nsupp_armed, uint, 0444);
MODULE_PARM_DESC(nsupp_armed, "已成功挂上的厂商上报压制点数");

/* ---- 目标 uid ---- */

static unsigned int ki_uids[KI_MAX_UIDS];
static int ki_nuids;

static int ki_parse_uids(const char *s)
{
	int n = 0;
	const char *p = s;

	ki_nuids = 0;
	if (!s || !*s)
		return 0;

	while (*p && n < KI_MAX_UIDS) {
		unsigned long v;
		char *end;

		/* 允许逗号和空格分隔 */
		while (*p == ',' || *p == ' ')
			p++;
		if (!*p)
			break;

		v = simple_strtoul(p, &end, 10);
		if (end == p) {
			pr_warn("ki_guard: bad uid token at '%s'\n", p);
			break;
		}
		ki_uids[n++] = (unsigned int)v;
		p = end;
		while (*p == ' ')
			p++;
		if (*p && *p != ',')
			break;
	}
	ki_nuids = n;
	return n;
}

static bool ki_uid_hit(void)
{
	unsigned int uid = (unsigned int)current_fsuid().val;
	int i;

	for (i = 0; i < ki_nuids; i++) {
		if (ki_uids[i] == uid)
			return true;
	}
	return false;
}

/* ---- 目标目录缓存 ---- */

struct ki_dir {
	dev_t dev;
	ino_t ino;
	const char *path;
};

static struct ki_dir ki_dirs[KI_MAX_DIRS];
static int ki_ndirs;
static int ki_want_dirs;
static int ki_resolve_tries;

static char *ki_dirs_copy;   /* 解析用的私有副本，不破坏 module_param 原文 */

static int ki_resolve_dirs(void)
{
	char *work, *tok, *cur;
	int found = 0, i;

	if (!ki_dirs_copy)
		return 0;

	work = kstrdup(ki_dirs_copy, GFP_KERNEL);
	if (!work)
		return 0;

	/* 重试路径会重新解析：先释放上一轮已分配的路径串，否则每次重试都漏一份 */
	for (i = 0; i < ki_ndirs; i++) {
		kfree(ki_dirs[i].path);
		ki_dirs[i].path = NULL;
	}

	ki_ndirs = 0;
	cur = work;
	while (ki_ndirs < KI_MAX_DIRS && (tok = strsep(&cur, ",")) != NULL) {
		struct path path;

		while (*tok == ' ')
			tok++;
		if (!*tok)
			continue;

		if (kern_path(tok, LOOKUP_FOLLOW, &path)) {
			pr_info("ki_guard: dir not mounted yet: %s\n", tok);
			continue;
		}

		if (!d_inode(path.dentry)) {
			path_put(&path);
			continue;
		}

		ki_dirs[ki_ndirs].dev  = path.dentry->d_sb->s_dev;
		ki_dirs[ki_ndirs].ino  = d_inode(path.dentry)->i_ino;
		ki_dirs[ki_ndirs].path = kstrdup(tok, GFP_KERNEL);
		path_put(&path);

		pr_info("ki_guard: watching %s (dev=%u ino=%lu)\n",
			ki_dirs[ki_ndirs].path ? ki_dirs[ki_ndirs].path : tok,
			(unsigned int)ki_dirs[ki_ndirs].dev,
			(unsigned long)ki_dirs[ki_ndirs].ino);
		ki_ndirs++;
		found++;
	}

	kfree(work);
	return found;
}

static struct delayed_work ki_resolve_work;

static void ki_resolve_work_fn(struct work_struct *w)
{
	(void)w;

	if (ki_ndirs >= ki_want_dirs)
		return;

	ki_resolve_dirs();
	ndirs_resolved = ki_ndirs;

	if (ki_ndirs >= ki_want_dirs)
		return;

	if (++ki_resolve_tries > 24) {
		pr_warn("ki_guard: giving up resolving %d/%d dirs\n",
			ki_ndirs, ki_want_dirs);
		return;
	}
	schedule_delayed_work(&ki_resolve_work, 5 * HZ);
}

static bool ki_dir_match(struct inode *ino)
{
	int i;

	if (!ino || !ino->i_sb)
		return false;

	for (i = 0; i < ki_ndirs; i++) {
		if (ki_dirs[i].dev == ino->i_sb->s_dev &&
		    ki_dirs[i].ino == ino->i_ino)
			return true;
	}
	return false;
}

/* ---- hook 1: inode_permission ---- */

struct ki_perm_data {
	struct inode *inode;
};

/*
 * 入口参数位置随内核版本变化：
 *   6.1 / 6.6 : inode_permission(struct mnt_idmap|user_namespace *, struct inode *, int)
 *   5.x       : inode_permission(struct inode *, int)
 * 统一从 x1 取 inode；若目标内核是 2 参数形式，把 ki_inode_arg 调成 0。
 */
static int ki_inode_arg = 1;
module_param(ki_inode_arg, int, 0644);
MODULE_PARM_DESC(ki_inode_arg, "inode 参数所在寄存器序号：6.x 为 1，5.x 为 0");

static __always_inline unsigned long ki_reg(struct pt_regs *regs, int idx)
{
	switch (idx) {
	case 0: return regs->regs[0];
	case 1: return regs->regs[1];
	case 2: return regs->regs[2];
	case 3: return regs->regs[3];
	case 4: return regs->regs[4];
	case 5: return regs->regs[5];
	default: return 0;
	}
}

static int ki_perm_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ki_perm_data *d = (struct ki_perm_data *)ri->data;

	d->inode = (struct inode *)ki_reg(regs, ki_inode_arg);
	return 0;
}

static int ki_perm_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ki_perm_data *d = (struct ki_perm_data *)ri->data;

	/* 原本就失败的不动，只把"成功"改成"不存在" */
	if (regs_return_value(regs) != 0)
		return 0;

	if (!ki_uid_hit())
		return 0;

	if (ki_dir_match(d->inode))
		regs_set_return_value(regs, -ENOENT);

	return 0;
}

static struct kretprobe ki_perm_krp = {
	.kp.symbol_name = "inode_permission",
	.entry_handler  = ki_perm_entry,
	.handler        = ki_perm_ret,
	.data_size      = sizeof(struct ki_perm_data),
	.maxactive      = 64,
};

/* ---- hook 2: 厂商上报点（跳过执行） ---- */

struct ki_supp {
	struct kretprobe krp;
	char *name;
	bool   live;
};

static struct ki_supp ki_supps[KI_MAX_SUPPS];
static int ki_nsupps;

/*
 * 这里刻意用 kretprobe 改返回值，而不是 kprobe pre_handler 返回 1。
 *
 * 原因：pre_handler 返回非 0 在 ARM64 上只跳过"被探测的那一条指令"，不是
 * 跳过整个函数。跳过 prologue 的第一条指令之后，后面的指令照常执行，栈和
 * 寄存器状态就失衡了 —— 结果是直接崩，不是"安静地阻止上报"。
 *
 * 代价要说清楚：改返回值只让调用者认为"上报成功"，上报内容本身在函数里
 * 该发的已经发出去了。要真正阻止执行需要把 regs->pc 改到函数末尾的 ret，
 * 但那在函数已改过栈指针的情况下同样不安全，必须逐函数在真机上验证。
 *
 * 所以本模块的定位是"降低可观测性"，不宣称"完全阻断上报"。
 */
static int ki_supp_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	(void)ri;
	regs_set_return_value(regs, 0);
	return 0;
}

static char *ki_supp_copy;

static void ki_install_supps(void)
{
	char *work, *tok, *cur;

	if (!ki_supp_copy)
		return;

	work = kstrdup(ki_supp_copy, GFP_KERNEL);
	if (!work)
		return;

	cur = work;
	while (ki_nsupps < KI_MAX_SUPPS && (tok = strsep(&cur, ",")) != NULL) {
		struct ki_supp *s = &ki_supps[ki_nsupps];
		int rc;

		while (*tok == ' ')
			tok++;
		if (!*tok)
			continue;

		s->name = kstrdup(tok, GFP_KERNEL);
		if (!s->name) {
			ki_nsupps++;
			continue;
		}
		s->krp.kp.symbol_name = s->name;
		s->krp.handler = ki_supp_ret;
		s->krp.maxactive = 32;

		rc = register_kretprobe(&s->krp);
		if (rc) {
			/* 符号不存在属正常：非 OPPO 设备没有这些厂商函数 */
			pr_info("ki_guard: suppress target '%s' not hookable (%d), skipped\n",
				tok, rc);
			kfree(s->name);
			s->name = NULL;
			ki_nsupps++;
			continue;
		}
		s->live = true;
		nsupp_armed++;
		pr_info("ki_guard: suppress '%s' armed\n", tok);
		ki_nsupps++;
	}

	kfree(work);
}

/* ---- 初始化 / 退出 ---- */

static int __init ki_guard_init(void)
{
	int rc;

	rc = ki_parse_uids(uids);
	if (rc <= 0) {
		pr_warn("ki_guard: no target uid given, module would do nothing\n");
	} else {
		pr_info("ki_guard: %d target uid(s)\n", rc);
	}

	ki_dirs_copy = kstrdup(dirs ? dirs : "", GFP_KERNEL);
	ki_want_dirs = 0;
	if (ki_dirs_copy) {
		const char *p = ki_dirs_copy;

		while (*p) {
			if (*p == ',')
				ki_want_dirs++;
			p++;
		}
		ki_want_dirs++;
	}

	ki_resolve_dirs();
	ndirs_resolved = ki_ndirs;
	if (ki_ndirs < ki_want_dirs)
		schedule_delayed_work(&ki_resolve_work, 5 * HZ);

	rc = register_kretprobe(&ki_perm_krp);
	if (rc) {
		pr_err("ki_guard: register_kretprobe(inode_permission)=%d\n", rc);
		goto err_krp;
	}
	pr_info("ki_guard: inode_permission hooked\n");

	ki_supp_copy = kstrdup(suppress ? suppress : "", GFP_KERNEL);
	ki_install_supps();

	if (hide) {
		list_del(&THIS_MODULE->list);
		kobject_del(&THIS_MODULE->mkobj.kobj);
		pr_info("ki_guard: active (hidden)\n");
	} else {
		pr_info("ki_guard: active (visible)\n");
	}

	return 0;

err_krp:
	cancel_delayed_work_sync(&ki_resolve_work);
	kfree(ki_dirs_copy);
	return rc;
}

static void __exit ki_guard_exit(void)
{
	int i;

	cancel_delayed_work_sync(&ki_resolve_work);

	unregister_kretprobe(&ki_perm_krp);

	for (i = 0; i < ki_nsupps; i++) {
		if (ki_supps[i].live)
			unregister_kretprobe(&ki_supps[i].krp);
		kfree(ki_supps[i].name);
	}

	for (i = 0; i < ki_ndirs; i++)
		kfree(ki_dirs[i].path);

	kfree(ki_dirs_copy);
	kfree(ki_supp_copy);

	pr_info("ki_guard: unloaded\n");
}

module_init(ki_guard_init);
module_exit(ki_guard_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel intercept: isolate device-flag dirs per uid, mute vendor reports");
MODULE_AUTHOR("plq110-spoof-suite");
