/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ki_guard_kpm.c - 显示事件通知层（KernelPatch Module）
 *
 * 作用：按目标 uid 隔离设备标记持久化分区（/mnt/vendor/persist/data、
 *       /mnt/vendor/nvdata 等），让指定应用读不到也写不进抗清除的设备标记。
 *
 * 为什么用 KPM 而不是 LKM
 * ----------------------
 * .ko 要过三道加载关卡：struct module 尺寸（6.1=1088/6.6=1536/6.12=1600）、
 * 符号 CRC、vermagic。前两项逼着每个 ABI 组编一个二进制。
 * KPM 由 KernelPatch 运行时加载，不走 kernel/module/main.c，这三道全不适用，
 * 所以一个 .kpm 覆盖所有装了 KernelPatch 的 aarch64 设备。
 *
 * 另外 KPM 的 hook_fargs 有 skip_origin 字段，可以真正不进入原函数体；
 * 而 .ko 的 kprobe pre_handler 返回非 0 只跳过一条指令，跳 prologue 会栈失衡。
 *
 * 不依赖编译期布局
 * ----------------
 * struct inode 的 i_sb / i_ino 偏移不在本文件写死，全部通过 kp_btf_member_offset()
 * 在运行时从目标内核自己的 BTF 取。换了内核不用重编，只要 BTF 可用。
 *
 * 参数（由 kpatch kpm load 传入 args）
 * -----------------------------------
 *   uids=10234,10235            目标应用 uid，必填
 *   dirs=/mnt/vendor/persist/data,/mnt/vendor/nvdata   要隐藏的目录，逗号分隔
 *   argno=3                     inode_permission 的参数个数：6.x 用 3，5.x 用 2
 *   hide=1                      是否尝试额外隐藏（保留）
 */

#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <kallsyms.h>
#include <kbtf.h>
#include <hook.h>
#include <ktypes.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <asm/current.h>

KPM_NAME("msm_drm_notify");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("display");
KPM_DESCRIPTION("Display panel event notification helper");

/*
 * 输出控制
 * --------
 * 默认完全静默。这一层每一条 pr_* 都会落进 dmesg，而厂商的安全检测会采集
 * 内核日志 —— cpuinfo_spoof 当初就是被记下 "[cpuinfo-spoof] active (hidden)"
 * 那一行。排查问题时在加载参数里加 verbose=1。
 */
static int ki_verbose = 0;

#define KI_LOG(...) 	do { if (ki_verbose) pr_info(__VA_ARGS__); } while (0)
#define KI_ERR(...) 	do { if (ki_verbose) pr_err(__VA_ARGS__); } while (0)

#define KI_MAX_UIDS 32
#define KI_MAX_DIRS 8
#define KI_MAX_SUPP 8

/* ---------------- 配置 ---------------- */

static uint32_t ki_uids[KI_MAX_UIDS];
static int ki_nuids;

static char ki_dir_specs[KI_MAX_DIRS][160];
static int ki_ndirs_want;

/* 解析后的目标目录：(i_sb 指针, i_ino) —— 只认这两个，不比较路径字符串 */
struct ki_target {
	uint64_t sb;
	uint64_t ino;
};
static struct ki_target ki_targets[KI_MAX_DIRS];
static int ki_ntargets;

static int ki_argno = 3;      /* 6.x: inode_permission(idmap, inode, mask) */
static int ki_inode_arg = 1;  /* inode 在 arg1 */

/* 厂商上报压制：目标符号名，逗号分隔 */
static char ki_supp_specs[KI_MAX_SUPP][96];
static int ki_nsupp_want;
static int ki_nsupp_hooked;

/* 累计拦截次数（统计用，单写多读，不需要强一致性） */
static uint64_t ki_intercepts;

/* 被替换掉的函数，卸载时要逐个 unhook */
static uint64_t ki_supp_funcs[KI_MAX_SUPP];

/* ---- 按需锁定 ---- */
static int ki_enabled = 1;          /* ctl1 切；相当于原版的 lock/unlock */

/* ---- 目录条目过滤 ---- */
/*
 * 原版靠替换 inode_operations 让条目从 readdir 结果里消失。这里不改那个
 * 只读结构，改成在 iterate_dir 里换掉 ctx->actor：dir_context 是调用方栈上
 * 的结构，可写，风险低得多。
 *
 * 6.1+ 的 actor 返回 bool（true = 继续遍历），KernelPatch 的 dir_context 已经
 * 按这个签名声明。旧的 int 版本语义相反，需要 dir_context_int。
 */
typedef bool (*ki_filldir_t)(struct dir_context *, const char *, int, loff_t,
			     u64, unsigned int);

/*
 * 槽数必须留够余量：槽满时 ki_iterate_before 会放弃替换 actor，
 * 那一次 readdir 就不隐藏 —— 目标应用只要同时打开 N 个目录 fd 并发读，
 * 就能把隐藏绕过去。64 个槽对正常应用足够，也远高于任何"顺手探测"的并发。
 *
 * 另外每个槽记下占用它的 task：ctx 是调用方栈上的结构，栈帧复用会让同一
 * 地址在不同线程间重复出现，只按 ctx 匹配可能把 A 线程的槽误当成 B 线程的。
 */
#define KI_MAX_ITER 64
struct ki_iter_slot {
	uint64_t ctx;
	uint64_t task;
	uint64_t saved;
	int used;
};
static struct ki_iter_slot ki_iter_slots[KI_MAX_ITER];
static uint64_t ki_iter_overflow;

static uint64_t ki_cur_task(void)
{
	return (uint64_t)current;
}

static uint64_t ki_parent_dentry[KI_MAX_DIRS];
static char ki_hide_name[KI_MAX_DIRS][64];
static int ki_nhide;

static long off_file_path = -1;

/* ---------------- BTF 偏移 ---------------- */

static long off_inode_sb = -1;
static long off_inode_ino = -1;
static long off_path_dentry = -1;
static long off_dentry_inode = -1;

static int ki_btf_offsets_ok(void)
{
	return off_inode_sb >= 0 && off_inode_ino >= 0;
}

static inline uint64_t ki_rd64(uint64_t addr)
{
	return *(volatile uint64_t *)addr;
}

/* ---------------- 参数解析 ---------------- */

static int ki_eq(const char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static int ki_parse_u32_list(const char *s, const char *end, uint32_t *out, int max)
{
	int n = 0;
	uint32_t v = 0;
	int got = 0;

	while (s < end && n < max) {
		char c = *s;

		if (c >= '0' && c <= '9') {
			v = v * 10 + (uint32_t)(c - '0');
			got = 1;
			s++;
			continue;
		}
		if (c == ',' || c == ' ' || c == '\t') {
			if (got) {
				out[n++] = v;
				v = 0;
				got = 0;
			}
			s++;
			continue;
		}
		break;   /* 遇到不认识的字符就停，不硬吃 */
	}
	if (got && n < max)
		out[n++] = v;
	return n;
}

/* 把 args 里 key= 之后、下一个空格之前的值取出来 */
static int ki_take_value(const char *args, const char *key, const char **val, int *len)
{
	int klen = 0;
	const char *p = args;

	while (key[klen])
		klen++;

	while (*p) {
		if (ki_eq(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1;
			int n = 0;
			while (v[n] && v[n] != ' ' && v[n] != '\t')
				n++;
			*val = v;
			*len = n;
			return 1;
		}
		while (*p && *p != ' ' && *p != '\t')
			p++;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	return 0;
}

static void ki_parse_dirs(const char *val, int len)
{
	int i = 0, n = 0;

	ki_ndirs_want = 0;
	while (i < len && n < KI_MAX_DIRS) {
		int j = i;
		int k = 0;

		while (j < len && val[j] != ',')
			j++;
		if (j > i) {
			while (i < j && k < (int)sizeof(ki_dir_specs[0]) - 1)
				ki_dir_specs[n][k++] = val[i++];
			ki_dir_specs[n][k] = '\0';
			n++;
		} else {
			i++;
		}
		if (i < len && val[i] == ',')
			i++;
	}
	ki_ndirs_want = n;
}

static void ki_parse_supps(const char *val, int len)
{
	int i = 0, n = 0;

	ki_nsupp_want = 0;
	while (i < len && n < KI_MAX_SUPP) {
		int j = i;
		int k = 0;

		while (j < len && val[j] != ',')
			j++;
		if (j > i) {
			while (i < j && k < (int)sizeof(ki_supp_specs[0]) - 1)
				ki_supp_specs[n][k++] = val[i++];
			ki_supp_specs[n][k] = '\0';
			n++;
		} else {
			i++;
		}
		if (i < len && val[i] == ',')
			i++;
	}
	ki_nsupp_want = n;
}

/* ---------------- 目录解析 ---------------- */

/*
 * kern_path 没有 kfunc_def 声明，用 kallsyms 取地址后按原型调用。
 * struct path 是 { struct vfsmount *mnt; struct dentry *dentry; }，
 * 但具体成员偏移不写死，用 BTF 取 dentry 的位置。
 */
typedef int (*ki_kern_path_fn)(const char *name, unsigned int flags, void *path);

static ki_kern_path_fn ki_kern_path;

#define KI_LOOKUP_FOLLOW 1

static int ki_resolve_dirs(void)
{
	int i, ok = 0;

	ki_ntargets = 0;
	if (!ki_kern_path || off_path_dentry < 0 || off_dentry_inode < 0 ||
	    !ki_btf_offsets_ok())
		return 0;

	for (i = 0; i < ki_ndirs_want; i++) {
		uint64_t pathbuf[3];   /* 够放下 { mnt, dentry } */
		uint64_t dentry, inode, sb, ino;
		int rc;

		pathbuf[0] = 0;
		pathbuf[1] = 0;
		pathbuf[2] = 0;

		rc = ki_kern_path(ki_dir_specs[i], KI_LOOKUP_FOLLOW, pathbuf);
		if (rc != 0) {
			KI_LOG("drmn: [%s] kern_path rc=%d (尚未挂载?)\n",
				ki_dir_specs[i], rc);
			continue;
		}

		dentry = *(uint64_t *)((char *)pathbuf + off_path_dentry);
		if (!dentry) {
			KI_LOG("drmn: [%s] dentry null\n", ki_dir_specs[i]);
			continue;
		}

		inode = ki_rd64(dentry + off_dentry_inode);
		if (!inode) {
			KI_LOG("drmn: [%s] inode null\n", ki_dir_specs[i]);
			continue;
		}

		sb = ki_rd64(inode + off_inode_sb);
		ino = ki_rd64(inode + off_inode_ino);

		ki_targets[ki_ntargets].sb = sb;
		ki_targets[ki_ntargets].ino = ino;
		ki_ntargets++;
		ok++;

		KI_LOG("drmn: watching %s (sb=%llx ino=%llu)\n",
			ki_dir_specs[i], (unsigned long long)sb,
			(unsigned long long)ino);
	}

	return ok;
}

/* ---------------- hook ---------------- */

static int ki_uid_hit(void)
{
	uint32_t uid = (uint32_t)current_uid();
	int i;

	for (i = 0; i < ki_nuids; i++) {
		if (ki_uids[i] == uid)
			return 1;
	}
	return 0;
}

static int ki_inode_is_target(uint64_t inode)
{
	uint64_t sb, ino;
	int i;

	if (!inode)
		return 0;

	sb = ki_rd64(inode + off_inode_sb);
	ino = ki_rd64(inode + off_inode_ino);

	for (i = 0; i < ki_ntargets; i++) {
		if (ki_targets[i].sb == sb && ki_targets[i].ino == ino)
			return 1;
	}
	return 0;
}

/*
 * 只拦目标目录本身的权限检查。进入目录必须先拿到 MAY_EXEC，所以拦这一层
 * 就覆盖整个子树：游戏的 ls / stat / open 全部失败，其他进程不受影响。
 * 返回 -ENOENT 而不是 -EACCES —— "没权限"本身就是可疑信号。
 */
static void ki_perm_before(hook_fargs3_t *fargs, void *udata)
{
	uint64_t inode;

	(void)udata;

	if (ki_ntargets <= 0)
		return;
	if (!ki_uid_hit())
		return;

	inode = fargs->args[ki_inode_arg];
	if (!ki_inode_is_target(inode))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_intercepts++;
}

static int ki_hooked;
static int ki_iter_hooked;

/* ---------------- 厂商上报压制 ---------------- */

/*
 * 用 hook() 把目标函数整个替换成一个返回 0 的桩，而不是 skip_origin 或改返回值。
 *
 * 区别在哪：
 *   - kretprobe 改返回值（.ko 版的做法）：函数照跑，上报内容已经发出去了
 *   - skip_origin：不进入函数体，但调用点仍然存在
 *   - hook() 替换：调用点直接跳到桩，原函数完全不执行
 *
 * 桩的签名按"无参数、返回 long"写：aarch64 上多余参数留在寄存器里不影响，
 * 而这些上报函数普遍是 void 或 int，调用方只看返回值。
 */
static long ki_suppress_stub(void)
{
	return 0;
}

static void ki_install_supps(void)
{
	int i;

	ki_nsupp_hooked = 0;
	for (i = 0; i < ki_nsupp_want; i++) {
		uint64_t addr = kallsyms_lookup_name(ki_supp_specs[i]);
		hook_err_t herr;
		void *backup = 0;

		if (!addr) {
			/* 非 OPPO/一加 设备没有这些厂商符号，属正常 */
			KI_LOG("drmn: suppress '%s' 不存在，跳过\n", ki_supp_specs[i]);
			continue;
		}

		herr = hook((void *)addr, (void *)ki_suppress_stub, &backup);
		if (herr != HOOK_NO_ERR) {
			KI_LOG("drmn: suppress '%s' hook 失败 rc=%d\n",
				ki_supp_specs[i], (int)herr);
			continue;
		}

		ki_supp_funcs[ki_nsupp_hooked] = addr;
		ki_nsupp_hooked++;
		KI_LOG("drmn: suppress '%s' armed (addr=%llx)\n",
			ki_supp_specs[i], (unsigned long long)addr);
	}
}

/* ---------------- 目录条目隐藏 ---------------- */

static int ki_split_path(const char *full, char *parent, int plen,
			 char *leaf, int llen)
{
	int n = 0, cut = -1, i, k;

	while (full[n])
		n++;
	for (i = n - 1; i >= 0; i--) {
		if (full[i] == '/') {
			cut = i;
			break;
		}
	}
	if (cut <= 0 || cut >= plen)
		return -1;

	for (i = 0; i < cut; i++)
		parent[i] = full[i];
	parent[cut] = '\0';

	k = n - cut - 1;
	if (k <= 0 || k >= llen)
		return -1;
	for (i = 0; i < k; i++)
		leaf[i] = full[cut + 1 + i];
	leaf[k] = '\0';
	return 0;
}

/*
 * 预解析每个目标目录的父目录 dentry，并记下要过滤掉的项名。
 * 比较 dentry 指针而不是每次拼路径，hook 里的开销才是常数级。
 */
static void ki_prepare_hide(void)
{
	int i, ok = 0;

	ki_nhide = 0;
	if (off_path_dentry < 0 || !ki_kern_path)
		return;

	for (i = 0; i < ki_ndirs_want && ok < KI_MAX_DIRS; i++) {
		char parent[160];
		char leaf[64];
		uint64_t pathbuf[3];
		uint64_t dentry;
		int j = 0;

		if (ki_split_path(ki_dir_specs[i], parent, (int)sizeof(parent),
				  leaf, (int)sizeof(leaf)))
			continue;

		pathbuf[0] = 0;
		pathbuf[1] = 0;
		pathbuf[2] = 0;
		if (ki_kern_path(parent, KI_LOOKUP_FOLLOW, pathbuf) != 0)
			continue;

		dentry = *(uint64_t *)((char *)pathbuf + off_path_dentry);
		if (!dentry)
			continue;

		ki_parent_dentry[ok] = dentry;
		while (leaf[j] && j < (int)sizeof(ki_hide_name[0]) - 1) {
			ki_hide_name[ok][j] = leaf[j];
			j++;
		}
		ki_hide_name[ok][j] = '\0';

		KI_LOG("drmn: entry '%s' parent=%llx\n",
			ki_hide_name[ok], (unsigned long long)dentry);
		ok++;
	}
	ki_nhide = ok;
}

static int ki_name_is_hidden(const char *name, int namelen)
{
	int i, j;

	for (i = 0; i < ki_nhide; i++) {
		if (!ki_hide_name[i][0])
			continue;
		for (j = 0; j < namelen; j++) {
			if (ki_hide_name[i][j] != name[j])
				break;
		}
		if (j == namelen && ki_hide_name[i][j] == '\0')
			return 1;
	}
	return 0;
}

static int ki_iter_slot_index(uint64_t ctx, uint64_t task)
{
	int i;

	for (i = 0; i < KI_MAX_ITER; i++) {
		if (ki_iter_slots[i].used && ki_iter_slots[i].ctx == ctx &&
		    ki_iter_slots[i].task == task)
			return i;
	}
	return -1;
}

static bool ki_actor(struct dir_context *ctx, const char *name, int namelen,
		     loff_t offset, u64 ino, unsigned int d_type)
{
	ki_filldir_t saved = (ki_filldir_t)0;
	int slot;

	slot = ki_iter_slot_index((u64)ctx, ki_cur_task());
	if (slot < 0) {
		/*
		 * 槽丢了（不该发生）。此时 ctx->actor 仍指向我们，没法转发给
		 * 原 actor —— 返回 true 让遍历继续、该条目丢失；返回 false 会
		 * 直接中止遍历。前者影响面更小，所以取 true，同时计数备查。
		 */
		ki_iter_overflow++;
		return true;
	}
	saved = (ki_filldir_t)ki_iter_slots[slot].saved;
	if (!saved) {
		ki_iter_overflow++;
		return true;
	}

	/* 命中要隐藏的项：不转发给原 actor，条目就不会出现在结果里。
	 * 返回 true 是"继续遍历"，不是"失败"——6.1+ 的 actor 语义。 */
	if (name && namelen > 0 && ki_name_is_hidden(name, namelen))
		return true;

	return saved(ctx, name, namelen, offset, ino, d_type);
}

static void ki_iterate_before(hook_fargs2_t *fargs, void *udata)
{
	struct file *file = (struct file *)fargs->arg0;
	struct dir_context *ctx = (struct dir_context *)fargs->arg1;
	uint64_t dentry, ctxkey, task;
	int i, slot = -1;

	(void)udata;

	if (!ki_enabled || ki_nhide <= 0 || off_file_path < 0)
		return;
	if (!ctx || !ctx->actor)
		return;
	if (!ki_uid_hit())
		return;

	dentry = *(uint64_t *)((char *)file + off_file_path + off_path_dentry);
	if (!dentry)
		return;

	for (i = 0; i < ki_nhide; i++) {
		if (ki_parent_dentry[i] && ki_parent_dentry[i] == dentry)
			break;
	}
	if (i >= ki_nhide)
		return;   /* 不是在遍历被隐藏目录的父目录 */

	ctxkey = (uint64_t)ctx;
	task = ki_cur_task();
	if (ki_iter_slot_index(ctxkey, task) >= 0)
		return;   /* 已经换过，别重复包装 */

	for (i = 0; i < KI_MAX_ITER; i++) {
		if (!ki_iter_slots[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		/*
		 * 槽满：这一次不替换 actor，readdir 结果里会出现本该隐藏的条目。
		 * 计数下来，ctl0 能查到 —— 这是隐藏性的真实短板，不该静默。
		 */
		ki_iter_overflow++;
		return;
	}

	ki_iter_slots[slot].ctx = ctxkey;
	ki_iter_slots[slot].task = task;
	ki_iter_slots[slot].saved = (uint64_t)ctx->actor;
	ki_iter_slots[slot].used = 1;
	ctx->actor = ki_actor;
}

static void ki_iterate_after(hook_fargs2_t *fargs, void *udata)
{
	struct dir_context *ctx = (struct dir_context *)fargs->arg1;
	int slot;

	(void)udata;

	if (!ctx)
		return;

	slot = ki_iter_slot_index((uint64_t)ctx, ki_cur_task());
	if (slot < 0)
		return;

	ctx->actor = (ki_filldir_t)ki_iter_slots[slot].saved;
	ki_iter_slots[slot].used = 0;
	ki_iter_slots[slot].ctx = 0;
	ki_iter_slots[slot].task = 0;
	ki_iter_slots[slot].saved = 0;
}

/* ---------------- 状态 ---------------- */

static void ki_fill_status(char *out, int outlen)
{
	int n = 0, i;

#define KI_PUTS(s)                                       \
	do {                                             \
		const char *_s = (s);                    \
		while (*_s && n < outlen - 1)            \
			out[n++] = *_s++;                \
	} while (0)

#define KI_PUTU(v)                                             \
	do {                                                   \
		uint64_t _v = (uint64_t)(v);                   \
		char _t[24];                                   \
		int _c = 0, _j;                                \
		if (_v == 0) {                                 \
			_t[_c++] = '0';                        \
		} else {                                       \
			while (_v) {                           \
				_t[_c++] = (char)('0' + _v % 10); \
				_v /= 10;                      \
			}                                      \
		}                                              \
		for (_j = _c - 1; _j >= 0 && n < outlen - 1; _j--) \
			out[n++] = _t[_j];                     \
	} while (0)

	KI_PUTS("hooked=");
	KI_PUTU(ki_hooked);
	KI_PUTS(" locked=");
	KI_PUTU(ki_enabled);
	KI_PUTS(" uids=");
	for (i = 0; i < ki_nuids; i++) {
		if (i)
			KI_PUTS(",");
		KI_PUTU(ki_uids[i]);
	}
	KI_PUTS(" dirs=");
	KI_PUTU(ki_ntargets);
	KI_PUTS("/");
	KI_PUTU(ki_ndirs_want);
	KI_PUTS(" ent=");
	KI_PUTU(ki_nhide);
	KI_PUTS(" iter=");
	KI_PUTU(ki_iter_hooked);
	KI_PUTS(" itovf=");
	KI_PUTU(ki_iter_overflow);
	KI_PUTS(" supp=");
	KI_PUTU(ki_nsupp_hooked);
	KI_PUTS("/");
	KI_PUTU(ki_nsupp_want);
	KI_PUTS(" hits=");
	KI_PUTU(ki_intercepts);
	KI_PUTS(" argno=");
	KI_PUTU(ki_argno);
	out[n] = '\0';

#undef KI_PUTU
#undef KI_PUTS
}

/* ---------------- 入口 ---------------- */

static long ki_init(const char *args, const char *event, void *__user reserved)
{
	const char *val;
	int len;
	uint64_t addr;

	(void)reserved;

	/*
	 * verbose 必须在第一条日志之前解析，否则调试开关自己也被静默掉。
	 * 默认 0：加载、挂 hook、卸载全程 dmesg 零输出。
	 */
	ki_verbose = 0;
	if (args && ki_take_value(args, "verbose", &val, &len)) {
		uint32_t tmpv[1] = { 0 };

		if (ki_parse_u32_list(val, val + len, tmpv, 1) == 1)
			ki_verbose = (int)tmpv[0];
	}

	KI_LOG("drmn: init event=%s args=%s\n", event ? event : "(null)",
		args ? args : "(null)");
	KI_LOG("drmn: kpver=%x\n", kpver);

	ki_nuids = 0;
	ki_ndirs_want = 0;
	ki_ntargets = 0;
	ki_nsupp_want = 0;
	ki_nsupp_hooked = 0;
	ki_nhide = 0;
	ki_intercepts = 0;
	ki_iter_overflow = 0;
	ki_hooked = 0;
	ki_iter_hooked = 0;
	ki_enabled = 1;

	if (args && ki_take_value(args, "uids", &val, &len))
		ki_nuids = ki_parse_u32_list(val, val + len, ki_uids, KI_MAX_UIDS);

	if (args && ki_take_value(args, "dirs", &val, &len))
		ki_parse_dirs(val, len);

	if (args && ki_take_value(args, "suppress", &val, &len))
		ki_parse_supps(val, len);

	if (args && ki_take_value(args, "argno", &val, &len)) {
		uint32_t tmp[1] = { 0 };
		if (ki_parse_u32_list(val, val + len, tmp, 1) == 1 && tmp[0] >= 1 && tmp[0] <= 5)
			ki_argno = (int)tmp[0];
	}

	KI_LOG("drmn: uids=%d dirs_want=%d supp_want=%d argno=%d\n",
		ki_nuids, ki_ndirs_want, ki_nsupp_want, ki_argno);

	if (ki_nuids <= 0)
		KI_LOG("drmn: 没有目标 uid，模块不会做任何事\n");

	/* BTF 偏移 */
	KI_LOG("drmn: btf available=%d\n", kp_btf_available());
	off_inode_sb = kp_btf_member_offset("inode", "i_sb");
	off_inode_ino = kp_btf_member_offset("inode", "i_ino");
	off_path_dentry = kp_btf_member_offset("path", "dentry");
	off_dentry_inode = kp_btf_member_offset("dentry", "d_inode");
	off_file_path = kp_btf_member_offset("file", "f_path");

	KI_LOG("drmn: off inode.i_sb=%ld inode.i_ino=%ld path.dentry=%ld dentry.d_inode=%ld file.f_path=%ld\n",
		off_inode_sb, off_inode_ino, off_path_dentry, off_dentry_inode,
		off_file_path);

	if (!ki_btf_offsets_ok()) {
		KI_ERR("drmn: BTF 取不到 inode 偏移，无法工作（内核缺 BTF?）\n");
		return -EINVAL;
	}

	/* 目录解析 */
	if (ki_ndirs_want > 0) {
		addr = kallsyms_lookup_name("kern_path");
		KI_LOG("drmn: kern_path=%llx\n", (unsigned long long)addr);
		if (addr)
			ki_kern_path = (ki_kern_path_fn)addr;

		ki_resolve_dirs();
		KI_LOG("drmn: resolved %d/%d dirs\n", ki_ntargets, ki_ndirs_want);

		/* 目录条目隐藏需要父目录 dentry；与上面共用同一次解析结果 */
		ki_prepare_hide();
		KI_LOG("drmn: entries prepared %d\n", ki_nhide);
	}

	/* 挂 inode_permission */
	addr = kallsyms_lookup_name("inode_permission");
	KI_LOG("drmn: inode_permission=%llx\n", (unsigned long long)addr);
	if (!addr) {
		KI_ERR("drmn: 找不到 inode_permission\n");
		return -ENOENT;
	}

	{
		hook_err_t herr = hook_wrap((void *)addr, ki_argno,
					    (void *)ki_perm_before, (void *)0, (void *)0);
		KI_LOG("drmn: hook_wrap rc=%d\n", (int)herr);
		if (herr != HOOK_NO_ERR) {
			KI_ERR("drmn: hook 安装失败 rc=%d\n", (int)herr);
			return -EIO;
		}
	}

	ki_hooked = 1;

	/* 目录条目隐藏：hook iterate_dir，换 ctx->actor 过滤掉目标项 */
	if (ki_nhide > 0) {
		addr = kallsyms_lookup_name("iterate_dir");
		KI_LOG("drmn: iterate_dir=%llx\n", (unsigned long long)addr);
		if (addr) {
			hook_err_t herr = hook_wrap((void *)addr, 2,
						    (void *)ki_iterate_before,
						    (void *)ki_iterate_after,
						    (void *)0);
			KI_LOG("drmn: iterate_dir hook rc=%d\n", (int)herr);
			if (herr == HOOK_NO_ERR)
				ki_iter_hooked = 1;
		} else {
			KI_LOG("drmn: 找不到 iterate_dir，条目仍会出现在 readdir 里\n");
		}
	}

	/* 厂商上报压制：符号不存在属正常，逐个记录不阻塞 */
	if (ki_nsupp_want > 0)
		ki_install_supps();

	KI_LOG("drmn: active (uids=%d dirs=%d/%d ent=%d supp=%d/%d)\n",
		ki_nuids, ki_ntargets, ki_ndirs_want, ki_nhide,
		ki_nsupp_hooked, ki_nsupp_want);
	return 0;
}

/* kpatch kpm ctl0 <name> <args> —— 状态查询 */
static long ki_control0(const char *args, char *__user out_msg, int outlen)
{
	char status[256];

	(void)args;

	ki_fill_status(status, (int)sizeof(status));
	KI_LOG("drmn: ctl0 %s\n", status);

	if (out_msg && outlen > 0)
		compat_copy_to_user(out_msg, status, (int)sizeof(status));

	return 0;
}

/* kpatch kpm ctl1 <name> —— lock/unlock 切换
 *
 * 对应原版的按需锁定：目标应用运行时 lock，退出后 unlock。
 * 关掉时 hook 仍在，但 ki_enabled=0 让两个回调立即放行，
 * 效果等价于撤销，且不需要反复重挂（重挂有窗口，还有失败风险）。
 */
static long ki_control1(void *a1, void *a2, void *a3)
{
	(void)a1;
	(void)a2;
	(void)a3;

	ki_enabled = !ki_enabled;
	KI_LOG("drmn: ctl1 locked=%d\n", ki_enabled);
	return (long)ki_enabled;
}

static long ki_exit(void *__user reserved)
{
	uint64_t addr;
	int i;

	(void)reserved;

	/* 撤的顺序：条目过滤 -> 主 hook -> 抑制。
	 * 先撤过滤是因为它依赖 iterate_dir 的包装状态，晚了会留下悬空 actor。 */
	if (ki_iter_hooked) {
		addr = kallsyms_lookup_name("iterate_dir");
		if (addr)
			unhook((void *)addr);
		ki_iter_hooked = 0;
	}
	for (i = 0; i < KI_MAX_ITER; i++) {
		ki_iter_slots[i].used = 0;
		ki_iter_slots[i].ctx = 0;
		ki_iter_slots[i].task = 0;
		ki_iter_slots[i].saved = 0;
	}

	if (ki_hooked) {
		addr = kallsyms_lookup_name("inode_permission");
		if (addr)
			unhook((void *)addr);
		ki_hooked = 0;
	}

	/* 最后撤抑制：让上报通道恢复得比拦截晚一点，避免中间窗口漏事件 */
	for (i = 0; i < ki_nsupp_hooked; i++) {
		if (ki_supp_funcs[i])
			unhook((void *)ki_supp_funcs[i]);
		ki_supp_funcs[i] = 0;
	}
	ki_nsupp_hooked = 0;

	KI_LOG("drmn: unloaded (n=%llu)\n",
		(unsigned long long)ki_intercepts);
	return 0;
}

KPM_INIT(ki_init);
KPM_CTL0(ki_control0);
KPM_CTL1(ki_control1);
KPM_EXIT(ki_exit);
