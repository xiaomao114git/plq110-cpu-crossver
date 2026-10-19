/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ki_guard.kpm - 内核拦截层（KernelPatch Module）
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

KPM_NAME("ki-guard");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("plq110-spoof-suite");
KPM_DESCRIPTION("Per-uid isolation of device-flag persist dirs (BTF-driven, no fixed offsets)");

#define KI_MAX_UIDS 32
#define KI_MAX_DIRS 8

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
	int klen = 0, i;
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
			pr_info("ki_guard: [%s] kern_path rc=%d (尚未挂载?)\n",
				ki_dir_specs[i], rc);
			continue;
		}

		dentry = *(uint64_t *)((char *)pathbuf + off_path_dentry);
		if (!dentry) {
			pr_info("ki_guard: [%s] dentry null\n", ki_dir_specs[i]);
			continue;
		}

		inode = ki_rd64(dentry + off_dentry_inode);
		if (!inode) {
			pr_info("ki_guard: [%s] inode null\n", ki_dir_specs[i]);
			continue;
		}

		sb = ki_rd64(inode + off_inode_sb);
		ino = ki_rd64(inode + off_inode_ino);

		ki_targets[ki_ntargets].sb = sb;
		ki_targets[ki_ntargets].ino = ino;
		ki_ntargets++;
		ok++;

		pr_info("ki_guard: watching %s (sb=%llx ino=%llu)\n",
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
}

static int ki_hooked;

/* ---------------- 状态 ---------------- */

static void ki_fill_status(char *out, int outlen)
{
	int n = 0, i, j;

#define KI_PUT(s)                                        \
	do {                                             \
		const char *_s = (s);                    \
		while (*_s && n < outlen - 1)            \
			out[n++] = *_s++;                \
	} while (0)

	KI_PUT("hooked=");
	KI_PUT(ki_hooked ? "1" : "0");
	KI_PUT(" uids=");
	for (i = 0; i < ki_nuids; i++) {
		uint32_t v = ki_uids[i];
		char tmp[12];
		int t = 0;
		if (v == 0) {
			tmp[t++] = '0';
		} else {
			while (v) {
				tmp[t++] = (char)('0' + v % 10);
				v /= 10;
			}
		}
		if (i)
			KI_PUT(",");
		for (j = t - 1; j >= 0; j--)
			out[n++] = tmp[j];
	}
	KI_PUT(" dirs=");
	{
		char tmp[12];
		uint32_t v = (uint32_t)ki_ntargets;
		int t = 0;
		if (v == 0) {
			tmp[t++] = '0';
		} else {
			while (v) {
				tmp[t++] = (char)('0' + v % 10);
				v /= 10;
			}
		}
		for (j = t - 1; j >= 0; j--)
			out[n++] = tmp[j];
	}
	KI_PUT("/");
	{
		char tmp[12];
		uint32_t v = (uint32_t)ki_ndirs_want;
		int t = 0;
		if (v == 0) {
			tmp[t++] = '0';
		} else {
			while (v) {
				tmp[t++] = (char)('0' + v % 10);
				v /= 10;
			}
		}
		for (j = t - 1; j >= 0; j--)
			out[n++] = tmp[j];
	}
	KI_PUT(" argno=");
	{
		char tmp[12];
		uint32_t v = (uint32_t)ki_argno;
		int t = 0;
		if (v == 0) {
			tmp[t++] = '0';
		} else {
			while (v) {
				tmp[t++] = (char)('0' + v % 10);
				v /= 10;
			}
		}
		for (j = t - 1; j >= 0; j--)
			out[n++] = tmp[j];
	}
	out[n] = '\0';
#undef KI_PUT
}

/* ---------------- 入口 ---------------- */

static long ki_init(const char *args, const char *event, void *__user reserved)
{
	const char *val;
	int len;
	uint64_t addr;

	(void)reserved;

	pr_info("ki_guard: init event=%s args=%s\n", event ? event : "(null)",
		args ? args : "(null)");
	pr_info("ki_guard: kpver=%x\n", kpver);

	ki_nuids = 0;
	ki_ndirs_want = 0;
	ki_ntargets = 0;
	ki_hooked = 0;

	if (args && ki_take_value(args, "uids", &val, &len))
		ki_nuids = ki_parse_u32_list(val, val + len, ki_uids, KI_MAX_UIDS);

	if (args && ki_take_value(args, "dirs", &val, &len))
		ki_parse_dirs(val, len);

	if (args && ki_take_value(args, "argno", &val, &len)) {
		uint32_t tmp[1] = { 0 };
		if (ki_parse_u32_list(val, val + len, tmp, 1) == 1 && tmp[0] >= 1 && tmp[0] <= 5)
			ki_argno = (int)tmp[0];
	}

	pr_info("ki_guard: uids=%d dirs_want=%d argno=%d\n",
		ki_nuids, ki_ndirs_want, ki_argno);

	if (ki_nuids <= 0)
		pr_warn("ki_guard: 没有目标 uid，模块不会做任何事\n");

	/* BTF 偏移 */
	pr_info("ki_guard: btf available=%d\n", kp_btf_available());
	off_inode_sb = kp_btf_member_offset("inode", "i_sb");
	off_inode_ino = kp_btf_member_offset("inode", "i_ino");
	off_path_dentry = kp_btf_member_offset("path", "dentry");
	off_dentry_inode = kp_btf_member_offset("dentry", "d_inode");

	pr_info("ki_guard: off inode.i_sb=%ld inode.i_ino=%ld path.dentry=%ld dentry.d_inode=%ld\n",
		off_inode_sb, off_inode_ino, off_path_dentry, off_dentry_inode);

	if (!ki_btf_offsets_ok()) {
		pr_err("ki_guard: BTF 取不到 inode 偏移，无法工作（内核缺 BTF?）\n");
		return -EINVAL;
	}

	/* 目录解析 */
	if (ki_ndirs_want > 0) {
		addr = kallsyms_lookup_name("kern_path");
		pr_info("ki_guard: kern_path=%llx\n", (unsigned long long)addr);
		if (addr)
			ki_kern_path = (ki_kern_path_fn)addr;

		ki_resolve_dirs();
		pr_info("ki_guard: resolved %d/%d dirs\n", ki_ntargets, ki_ndirs_want);
	}

	/* 挂 inode_permission */
	addr = kallsyms_lookup_name("inode_permission");
	pr_info("ki_guard: inode_permission=%llx\n", (unsigned long long)addr);
	if (!addr) {
		pr_err("ki_guard: 找不到 inode_permission\n");
		return -ENOENT;
	}

	{
		hook_err_t herr = hook_wrap((void *)addr, ki_argno,
					    (void *)ki_perm_before, (void *)0, (void *)0);
		pr_info("ki_guard: hook_wrap rc=%d\n", (int)herr);
		if (herr != HOOK_NO_ERR) {
			pr_err("ki_guard: hook 安装失败 rc=%d\n", (int)herr);
			return -EIO;
		}
	}

	ki_hooked = 1;
	pr_info("ki_guard: active (uids=%d dirs=%d/%d)\n",
		ki_nuids, ki_ntargets, ki_ndirs_want);
	return 0;
}

/* kpatch kpm ctl0 <name> <args> —— 状态查询 */
static long ki_control0(const char *args, char *__user out_msg, int outlen)
{
	char status[256];

	(void)args;

	ki_fill_status(status, (int)sizeof(status));
	pr_info("ki_guard: ctl0 %s\n", status);

	if (out_msg && outlen > 0)
		compat_copy_to_user(out_msg, status, (int)sizeof(status));

	return 0;
}

/* kpatch kpm ctl1 —— 重新解析目录（分区后挂载时用） */
static long ki_control1(void *a1, void *a2, void *a3)
{
	int n;

	(void)a1;
	(void)a2;
	(void)a3;

	n = ki_resolve_dirs();
	pr_info("ki_guard: ctl1 re-resolve -> %d/%d\n", n, ki_ndirs_want);
	return n;
}

static long ki_exit(void *__user reserved)
{
	uint64_t addr;

	(void)reserved;

	if (ki_hooked) {
		addr = kallsyms_lookup_name("inode_permission");
		if (addr)
			unhook((void *)addr);
		ki_hooked = 0;
	}

	pr_info("ki_guard: unloaded\n");
	return 0;
}

KPM_INIT(ki_init);
KPM_CTL0(ki_control0);
KPM_CTL1(ki_control1);
KPM_EXIT(ki_exit);
