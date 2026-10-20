/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 显示事件通知层（KernelPatch Module）—— 零 BTF 依赖版
 *
 * 为什么不用 BTF
 * --------------
 * 上一版用 kp_btf_member_offset() 取 struct inode / path / file 的运行时偏移，
 * 好处是换内核不用重编。但实测目标环境的 KernelPatch（KPatch-Next d05 与
 * 0.13.9）都没有 kp_btf_* 符号 —— KPM 的未定义符号在 relo 阶段解析，
 * 缺一个就整包加载失败。所以改成完全不碰结构偏移的方案。
 *
 * 怎么在不知道任何偏移的情况下判定目标路径
 * ----------------------------------------
 * KernelPatch 的 fs.h 给了 struct filename 的完整定义：
 *     struct filename { const char *name; ... };
 * hook do_filp_open(dfd, struct filename *pathname, op) 和
 * filename_lookup(dfd, struct filename *name, ...) 时，
 * arg1 就是 struct filename *，它的 ->name 直接是路径字符串。
 * 于是只做字符串前缀比对 —— 不需要 BTF、不需要 kern_path 预解析、
 * 不需要知道 struct inode / path / dentry / file 的任何字段位置。
 *
 * 覆盖面
 * ------
 *   do_filp_open   -> open / openat / creat
 *   filename_lookup -> stat / lstat / access / mkdir / unlink 等
 * 两个都 hook 才能拦住"打开"和"探测存在性"两类访问。
 *
 * 放弃了什么
 * ----------
 * 上一版还能让目录条目从 readdir 结果里消失（hook iterate_dir 换 ctx->actor）。
 * 那需要 struct file 的 f_path 偏移，没有 BTF 就只能硬编码，风险太高，因此去掉。
 * 本版只保证：目标 uid 打不开、也 stat 不到目标目录。
 *
 * 参数（kpatch kpm load 传入 args）
 * ---------------------------------
 *   uids=10234,10235            目标 uid，必填
 *   dirs=/path1,/path2          要拦的目录，前缀匹配
 *   suppress=sym1,sym2          整函数替换成返回 0 的桩
 *   argno=3                     do_filp_open 的参数个数（filename_lookup 用 5）
 *   verbose=1                   输出调试日志；默认 0 保持 dmesg 干净
 */

#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <kallsyms.h>
#include <hook.h>
#include <ktypes.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <asm/current.h>

KPM_NAME("msm_drm_notify");
KPM_VERSION("2.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("display");
KPM_DESCRIPTION("Display panel event notification helper");

/*
 * 输出开关：默认 0 —— 加载、挂 hook、卸载全程不写 dmesg。
 * insmod 都已经会留 taint，别再额外留文字痕迹。
 */
static int ki_verbose = 0;

#define KI_LOG(...) \
	do { if (ki_verbose) pr_info(__VA_ARGS__); } while (0)
#define KI_ERR(...) \
	do { if (ki_verbose) pr_err(__VA_ARGS__); } while (0)

#define KI_MAX_UIDS 32
#define KI_MAX_DIRS 8
#define KI_MAX_SUPP 8

/* ---------------- 配置 ---------------- */

static uint32_t ki_uids[KI_MAX_UIDS];
static int ki_nuids;

static char ki_dir_specs[KI_MAX_DIRS][160];
static int ki_ndirs_want;

static char ki_supp_specs[KI_MAX_SUPP][96];
static int ki_nsupp_want;
static int ki_nsupp_hooked;
static uint64_t ki_supp_funcs[KI_MAX_SUPP];

static uint64_t ki_hits;      /* open 侧命中数 */
static uint64_t ki_lookups;   /* lookup 侧命中数 */
static uint64_t ki_mutates;   /* mkdir/unlink/rename 等写操作命中数 */
static uint64_t ki_readlinks; /* readlink 命中数 */

static int ki_enabled = 1;
/*
 * hook 覆盖模式。
 *   0 = core（默认）：只挂 do_filp_open + filename_lookup，实测 load/unload 稳定
 *   1 = full：额外挂 mkdir/unlink/rmdir/rename/symlink/readlink 六个入口，
 *             写操作覆盖更全，但实测 unload 时会触发内核重启
 * 由加载参数 hooks=full 打开。默认 core 是因为稳定性优先——用户点开关关闭
 * 拦截层时不该崩设备。
 */
static int ki_hook_full = 0;
/*
 * 逐个 hook 开关，默认都开。CFI + BTI 内核上某些 hook 点可能在特定调用路径
 * 里触发校验失败，需要能单独关掉做二分定位。
 */
static int ki_want_open = 1;
static int ki_want_lookup = 1;

/* ---------------- 参数解析 ---------------- */

/*
 * 不能用 strlen：KernelPatch 的 linux/string.h 把它映射到 kf_strlen，
 * 而 kpimg 里没有该符号，会在 relo 阶段解析失败导致整包加载不上。
 * 同理 ki_fill_status 里也是自己数字节。
 */
static int ki_strlen(const char *s)
{
	int n = 0;

	if (!s)
		return 0;
	while (s[n])
		n++;
	return n;
}

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
		break;
	}
	if (got && n < max)
		out[n++] = v;
	return n;
}

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

static void ki_parse_csv(const char *val, int len, char dst[][160], int max, int *count)
{
	int i = 0, n = 0;

	*count = 0;
	while (i < len && n < max) {
		int j = i;
		int k = 0;

		while (j < len && val[j] != ',')
			j++;
		if (j > i) {
			while (i < j && k < 159)
				dst[n][k++] = val[i++];
			dst[n][k] = '\0';
			n++;
		} else {
			i++;
		}
		if (i < len && val[i] == ',')
			i++;
	}
	*count = n;
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
			while (i < j && k < 95)
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

/* ---------------- 判定 ---------------- */

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

/*
 * 前缀匹配目标目录。命中后必须紧跟 '/' 或字符串结束，
 * 否则 /mnt/vendor/persist/data 会把 /mnt/vendor/persist/database 也拦掉。
 */
static int ki_path_is_target(const char *path)
{
	int i, j;

	if (!path || path[0] != '/')
		return 0;   /* 只处理绝对路径；相对路径交给 dfd，无法可靠判定 */

	for (i = 0; i < ki_ndirs_want; i++) {
		const char *pat = ki_dir_specs[i];

		if (!pat[0])
			continue;
		for (j = 0; pat[j]; j++) {
			if (path[j] != pat[j])
				break;
		}
		if (pat[j] != '\0')
			continue;
		if (path[j] == '\0' || path[j] == '/')
			return 1;
	}
	return 0;
}

static int ki_should_block(struct filename *fn)
{
	if (!ki_enabled)
		return 0;
	if (ki_nuids <= 0 || ki_ndirs_want <= 0)
		return 0;
	/* filename_lookup accepts ERR_PTR from failed getname operations. */
	if (IS_ERR_OR_NULL(fn))
		return 0;
	if (!ki_uid_hit())
		return 0;
	if (IS_ERR_OR_NULL(fn->name))
		return 0;
	return ki_path_is_target(fn->name);
}

/* hook 1: do_filp_open(int dfd, struct filename *pathname, const struct open_flags *op) */
static void ki_open_before(hook_fargs3_t *fargs, void *udata)
{
	(void)udata;

	if (!ki_should_block((struct filename *)fargs->arg1))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_hits++;
}

/* hook 2: filename_lookup(int dfd, struct filename *name, unsigned flags,
 *                         struct path *path, struct path *root)          */
static void ki_lookup_before(hook_fargs5_t *fargs, void *udata)
{
	(void)udata;

	if (!ki_should_block((struct filename *)fargs->arg1))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_lookups++;
}

/*
 * 写操作：mkdir / unlink / rmdir / symlink 这类只带一个 struct filename* 的，
 * 参数位置在 arg1（do_mkdirat/do_unlinkat/do_rmdir）。
 * 实测发现这些原来完全没拦——touch 之所以被拦是因为它内部先做 lookup，
 * 而 mkdir/unlink 走的是独立的 do_* 入口，不经过 do_filp_open。
 */
static void ki_mutate_before(hook_fargs3_t *fargs, void *udata)
{
	(void)udata;

	if (!ki_should_block((struct filename *)fargs->arg1))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_mutates++;
}

/*
 * 双路径写操作：do_renameat2(olddfd, from, newdfd, to, flags) 和
 * do_linkat(olddfd, old, newdfd, new, flags) 都是 5 参数，
 * 两个 filename 分别在 arg1 和 arg3。任一命中目标就拦。
 */
static void ki_rename_before(hook_fargs5_t *fargs, void *udata)
{
	(void)udata;

	if (!ki_should_block((struct filename *)fargs->arg1) &&
	    !ki_should_block((struct filename *)fargs->arg3))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_mutates++;
}

/* do_symlinkat(struct filename *from, int newdfd, struct filename *to) */
static void ki_symlink_before(hook_fargs3_t *fargs, void *udata)
{
	(void)udata;

	if (!ki_should_block((struct filename *)fargs->arg0) &&
	    !ki_should_block((struct filename *)fargs->arg2))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_mutates++;
}

/*
 * vfs_readlink(struct dentry *dentry, char *buffer, int buflen)
 * 这里没有路径字符串，只有 dentry。用 dentry_path_raw 反查——
 * 它只需要 dentry，不需要构造 struct path，所以零 BTF 方案也能用。
 * 注意 dentry_path_raw 是内核符号而非 KernelPatch API，必须 kallsyms 取地址。
 */
static char *(*ki_dentry_path_raw)(void *dentry, char *buf, int buflen);

static void ki_readlink_before(hook_fargs3_t *fargs, void *udata)
{
	char buf[256];
	char *p;

	(void)udata;

	if (!ki_enabled || ki_nuids <= 0 || ki_ndirs_want <= 0)
		return;
	if (!fargs->arg0 || !ki_dentry_path_raw)
		return;
	if (!ki_uid_hit())
		return;

	p = ki_dentry_path_raw((void *)fargs->arg0, buf, (int)sizeof(buf));
	/*
	 * dentry_path_raw 返回的是 buf 内部的指针，不是 buf 本身 ——
	 * __dentry_path 从 buf 末尾向前写，返回值指向路径起始处。
	 * 所以不能判断 p != buf，只能判断它落在 [buf, buf+size) 范围内。
	 */
	if (!p || p < buf || p >= buf + sizeof(buf))
		return;   /* ERR_PTR 或路径超出缓冲 */
	if (!ki_path_is_target(p))
		return;

	fargs->skip_origin = 1;
	fargs->ret = (uint64_t)(long)(-ENOENT);
	ki_readlinks++;
}

/* ---------------- /proc/cpuinfo 伪装 ---------------- */

/* cpuinfo 文本编译进 .rodata（命令行传不下 3~7 KB）。自动生成。 */
#include "ki_cpuinfo_gen.h"

/*
 * 从 cpuinfo_spoof.ko 搬过来。原模块用 kprobe 在 seq_open 前置 hook，
 * 把第二个参数（op 表）换成自己的 seq_operations。这里用 KPM 做同样的事，
 * 好处是不再需要 .ko 的 vermagic / struct module 尺寸 / 符号 CRC 三道关卡，
 * 一个 KPM 覆盖所有内核版本。
 *
 * seq_open(struct file *file, const struct seq_operations *op)
 *   arg0 = file，arg1 = op —— 改 arg1 即可，不需要碰文件内容。
 *
 * 唯一的偏移依赖在 is_cpuinfo()：要读 file->f_path.dentry->d_name 判断文件名。
 * 这三个结构体布局从 4.x 到 6.12 都没变过，直接用常量；读取前做边界检查，
 * 偏移若不符也只会静默跳过，不会崩。
 */
struct ki_seqops {
	void *(*start)(void *m, void *pos);
	void (*stop)(void *m, void *v);
	void *(*next)(void *m, void *v, void *pos);
	int (*show)(void *m, void *v);
};

/* 本文件不再读取任何内核结构体成员，全部靠 kallsyms 拿符号地址，
 * 所以不需要 file/path/dentry/qstr 的偏移常量。 */

static void *(*ki_seq_puts)(void *m, const char *s);
static struct ki_seqops ki_cpuinfo_ops;
static const char *ki_cpuinfo_text;
static int ki_cpu_enabled;
static int ki_hooked_cpu;
static uint64_t ki_cpu_hits;
static uint64_t ki_cpu_scanned;

static void *ki_sp_start(void *m, void *pos)
{
	(void)m;
	return *(loff_t *)pos == 0 ? (void *)1 : 0;
}

static void ki_sp_stop(void *m, void *v)
{
	(void)m;
	(void)v;
}

static void *ki_sp_next(void *m, void *v, void *pos)
{
	(void)m;
	(void)v;
	++*(loff_t *)pos;
	return 0;
}

static int ki_sp_show(void *m, void *v)
{
	(void)v;
	if (ki_seq_puts && ki_cpuinfo_text)
		ki_seq_puts(m, ki_cpuinfo_text);
	return 0;
}

/*
 * 识别 /proc/cpuinfo 不靠 file 结构体偏移，而是比 seq_open 的第二个参数。
 *
 * ARM64 是这样注册的：
 *     static const struct seq_operations cpuinfo_op = {...};
 *     proc_create_seq("cpuinfo", 0, NULL, &cpuinfo_op);
 * 于是 seq_open(file, op) 收到的 op 恒等于 &cpuinfo_op 这个唯一地址。
 * 直接比指针即可，完全不依赖 struct file/path/dentry/qstr 的任何偏移——
 * 这才是真正的跨内核。
 *
 * 早先按 file->f_path.dentry->d_name 判断的版本实测失败：
 * cpu_scan 涨到 250 多次而 cpu_hit 恒为 0，说明 f_path 偏移猜错了。
 * 用符号地址则没有这个风险。
 */
static uint64_t ki_cpuinfo_op_addr;
static uint64_t *ki_cpuinfo_op_p;

static void ki_seqopen_before(hook_fargs3_t *fargs, void *udata)
{
	(void)udata;

	/*
	 * 这里只看 ki_cpu_enabled，不看 ki_enabled。
	 *
	 * ki_enabled 是「目录拦截」的运行时开关，由 ctl0 toggle 翻转。CPU 伪装
	 * 和目录拦截共用同一个 KPM 实例，但它们是两个独立的开关：用户关掉 KI
	 * 时 KPM 会保留下来继续承担 CPU 伪装（ki_off 的 kpm-cpu 分支）。
	 * 早先这里也检查 ki_enabled，结果关 KI 会把 CPU 伪装一起停掉——
	 * 实测表现为「关 KI 后 /proc/cpuinfo 立刻变回真机 8 核」。
	 */
	if (!ki_cpu_enabled)
		return;
	if (!ki_cpuinfo_text)
		return;

	ki_cpu_scanned++;
	if (!ki_cpuinfo_op_addr || fargs->arg1 != ki_cpuinfo_op_addr)
		return;

	fargs->arg1 = (uint64_t)&ki_cpuinfo_ops;
	ki_cpu_hits++;
}

static int ki_hooked_open;
static int ki_hooked_lookup;
static int ki_hooked_mutate;
static int ki_hooked_rename;
static int ki_hooked_symlink;
static int ki_hooked_readlink;

/* Track only callbacks successfully registered by this module. */
struct ki_wrap_registration {
	void *address;
	void *before;
	void *after;
};
static struct ki_wrap_registration ki_wraps[8];
static int ki_nwraps;

static hook_err_t ki_register_wrap(void *address, int32_t argno,
				   void *before, void *after, void *udata)
{
	hook_err_t rc;
	if (ki_nwraps >= 8)
		return -HOOK_CHAIN_FULL;
	rc = hook_wrap(address, argno, before, after, udata);
	if (rc == HOOK_NO_ERR) {
		ki_wraps[ki_nwraps].address = address;
		ki_wraps[ki_nwraps].before = before;
		ki_wraps[ki_nwraps].after = after;
		ki_nwraps++;
	}
	return rc;
}

static void ki_unregister_wraps(void)
{
	while (ki_nwraps > 0) {
		struct ki_wrap_registration *entry = &ki_wraps[--ki_nwraps];
		hook_unwrap(entry->address, entry->before, entry->after);
		entry->address = 0;
		entry->before = 0;
		entry->after = 0;
	}
}

/* ---------------- 厂商上报压制 ---------------- */

static long ki_suppress_stub(void)
{
	return 0;
}

static void ki_install_supps(void)
{
	int i;

	ki_nsupp_hooked = 0;
	for (i = 0; i < ki_nsupp_want && i < KI_MAX_SUPP; i++) {
		uint64_t addr;
		hook_err_t herr;
		void *backup = 0;

		if (!ki_supp_specs[i][0])
			continue;
		addr = kallsyms_lookup_name(ki_supp_specs[i]);
		if (!addr) {
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
		KI_LOG("drmn: suppress '%s' armed\n", ki_supp_specs[i]);
	}
}

/* ---------------- 状态 ---------------- */

/*
 * 返回实际写入的字符数（不含结尾 NUL）。
 *
 * 注意不要在这里用 strlen——KernelPatch 的 linux/string.h 把 strlen 映射成
 * kf_strlen，而 kpimg 里没有这个符号，符号解析阶段就会失败，整个 KPM 加载不上
 * （实测 load rc=255，dmesg 里连 init 的日志都没有）。自己数字节最稳。
 */
static int ki_fill_status(char *out, int outlen)
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
	KI_PUTU(ki_hooked_open);
	KI_PUTS(" lookup=");
	KI_PUTU(ki_hooked_lookup);
	KI_PUTS(" locked=");
	KI_PUTU(ki_enabled);
	KI_PUTS(" uids=");
	for (i = 0; i < ki_nuids; i++) {
		if (i)
			KI_PUTS(",");
		KI_PUTU(ki_uids[i]);
	}
	KI_PUTS(" dirs=");
	KI_PUTU(ki_ndirs_want);
	KI_PUTS(" supp=");
	KI_PUTU(ki_nsupp_hooked);
	KI_PUTS("/");
	KI_PUTU(ki_nsupp_want);
	KI_PUTS(" open_hit=");
	KI_PUTU(ki_hits);
	KI_PUTS(" look_hit=");
	KI_PUTU(ki_lookups);
	KI_PUTS(" mut_hit=");
	KI_PUTU(ki_mutates);
	KI_PUTS(" rl_hit=");
	KI_PUTU(ki_readlinks);
	KI_PUTS(" cpu=");
	KI_PUTU(ki_hooked_cpu);
	KI_PUTS(" cpu_hit=");
	KI_PUTU(ki_cpu_hits);
	KI_PUTS(" cpu_scan=");
	KI_PUTU(ki_cpu_scanned);
	KI_PUTS(" full=");
	KI_PUTU(ki_hook_full);
	out[n] = '\0';

#undef KI_PUTU
#undef KI_PUTS

	return n;
}

/* ---------------- 入口 ---------------- */

/* ---------------- 参数签名 ---------------- */

/*
 * 为什么要签名：
 *   uids / dirs 由用户态脚本解密后传给内核。脚本可以解包，改了再传就是
 *   另一个目标应用；只在脚本层做文章永远拦不住这一步。把校验搬进内核后，
 *   改了参数但算不出正确签名，模块直接不挂 hook，拦截不会生效。
 *
 * 为什么用 SHA256 而不是简单校验和：
 *   shell 侧只有 sha256sum 可用（无 openssl，toybox awk 不支持自定义函数），
 *   内核侧必须能算出同样的值，所以两边都用标准 SHA256。
 *
 * salt 从参数传入（来自加密的 config.sec），不是硬编码：这样泄露一个
 * salt 不会波及所有设备，而且换 salt 不需要重新编译内核模块。
 */

struct ki_sha256_ctx {
	uint32_t h[8];
	uint64_t total;
	uint32_t buflen;
	uint8_t buf[64];
};

static const uint32_t ki_sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define KI_ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void ki_sha256_block(struct ki_sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, d, e, f, g, hh, cc;
	uint32_t s0, s1, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		s0 = KI_ROR32(w[i - 15], 7) ^ KI_ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		s1 = KI_ROR32(w[i - 2], 17) ^ KI_ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
	e = c->h[4]; f = c->h[5]; g = c->h[6]; hh = c->h[7];

	for (i = 0; i < 64; i++) {
		s1 = KI_ROR32(e, 6) ^ KI_ROR32(e, 11) ^ KI_ROR32(e, 25);
		t1 = hh + s1 + ((e & f) ^ (~e & g)) + ki_sha256_k[i] + w[i];
		s0 = KI_ROR32(a, 2) ^ KI_ROR32(a, 13) ^ KI_ROR32(a, 22);
		t2 = s0 + ((a & b) ^ (a & cc) ^ (b & cc));
		hh = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}

	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

static void ki_sha256_init(struct ki_sha256_ctx *c)
{
	c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
	c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
	c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
	c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
	c->total = 0;
	c->buflen = 0;
}

static void ki_sha256_update(struct ki_sha256_ctx *c, const void *data, uint32_t n)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t i;

	c->total += n;
	for (i = 0; i < n; i++) {
		c->buf[c->buflen++] = p[i];
		if (c->buflen == 64) {
			ki_sha256_block(c, c->buf);
			c->buflen = 0;
		}
	}
}

static void ki_sha256_final(struct ki_sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->total * 8;
	uint8_t pad = 0x80;
	uint8_t zero = 0;
	uint8_t lenb[8];
	int i;

	ki_sha256_update(c, &pad, 1);
	while (c->buflen != 56)
		ki_sha256_update(c, &zero, 1);
	for (i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - i * 8));
	ki_sha256_update(c, lenb, 8);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(c->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)c->h[i];
	}
}

/*
 * 签名输入：salt | uids 原文 | dirs 原文 | argno
 * 刻意用分隔符 '|'，避免 uid 串和目录串拼接后产生歧义。
 */
/* ---------------- 许可证验证（Ed25519 非对称） ---------------- */

/*
 * 为什么换成非对称签名：
 *   上一版用 HMAC，key 硬编码在 KPM 里。会反汇编的人能把 key 挖出来，
 *   然后给自己的设备发卡——「搬走即用」堵死了，但「逆向即用」没堵住。
 *   现在私钥只在发卡端（Python 脚本），KPM 里只有公钥：逆向 KPM 只能拿到
 *   公钥，签发不出新卡。
 *
 * PLQ3 格式（99 字符）：
 *   PLQ3-<base64url(payload[6] | sig[64])>
 *   payload = issued_days:u16be | expires_days:u16be | serial:u16be
 *   msg     = "PLQ3\0" | machine[16] | "\0" | payload[6]
 *   sig     = Ed25519_sign(私钥, msg)
 *
 * machine 是签名输入而非输出，KPM 用脚本传来的 mid 重算才能验过——
 * 换设备的 mid 签名就对不上，拷贝许可证到别的机器无效。
 *
 * Ed25519 实现来自 orlp/ed25519（无 libc、无 SIMD），已用 RFC 8032
 * 官方向量在 x86 与 aarch64 双架构验证通过。
 */

#include "ed25519/ed25519.h"

/* 发卡端公钥（0cb425e5... 的 32 字节），私钥不在设备上 */
static const uint8_t ki_lic_pubkey[32] = {
	0x0c, 0xb4, 0x25, 0xe5, 0xa1, 0xe8, 0xaf, 0x28,
	0x5f, 0x27, 0xd1, 0x87, 0xe0, 0x30, 0xac, 0xfa,
	0x3f, 0xda, 0xfe, 0xdd, 0xd7, 0x22, 0x92, 0x46,
	0xa1, 0x45, 0x05, 0xf6, 0x1b, 0x7e, 0xbd, 0xeb
};

static int ki_b64u_val(char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A';
	if (ch >= 'a' && ch <= 'z')
		return ch - 'a' + 26;
	if (ch >= '0' && ch <= '9')
		return ch - '0' + 52;
	if (ch == '-')
		return 62;
	if (ch == '_')
		return 63;
	return -1;
}

/* base64url 无填充解码；返回写出字节数，-1 表示非法字符 */
static int ki_b64u_decode(const char *in, int inlen, uint8_t *out, int outmax)
{
	int i, o = 0;
	uint32_t acc = 0;
	int bits = 0;

	for (i = 0; i < inlen; i++) {
		int v = ki_b64u_val(in[i]);

		if (v < 0)
			return -1;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= outmax)
				return -1;
			out[o++] = (uint8_t)((acc >> bits) & 0xff);
		}
	}
	return o;
}

/*
 * 验证 PLQ3 许可证。
 *   lic / liclen : "PLQ3-<94 字符>"，总长 99
 *   mid          : 本机机器码，恰好 16 字符
 * 返回 1 通过，0 拒绝。
 */
static int ki_verify_license(const char *lic, int liclen, const char *mid)
{
	/* msg 长度必须是 4+1+16+1+6 = 28 字节。
	 * 早先写成 25 是算错的——payload 写到 msg[22..27] 就越界了，
	 * sizeof(msg) 又传 25，签名永远对不上，表现为「正确卡密也被拒绝」。 */
	uint8_t raw[70], msg[28];
	uint32_t issued_days, expires_days;
	int i, n;
	int midlen;

	if (!lic || !mid) {
		KI_ERR("drmn: [lic] lic 或 mid 为空\n");
		return 0;
	}
	if (liclen != 99) {
		KI_ERR("drmn: [lic] liclen=%d 应为 99\n", liclen);
		return 0;
	}
	midlen = ki_strlen(mid);
	if (midlen != 16) {
		KI_ERR("drmn: [lic] midlen=%d 应为 16\n", midlen);
		return 0;
	}
	if (lic[0] != 'P' || lic[1] != 'L' || lic[2] != 'Q' ||
	    lic[3] != '3' || lic[4] != '-') {
		KI_ERR("drmn: [lic] 前缀不是 PLQ3-\n");
		return 0;
	}

	n = ki_b64u_decode(lic + 5, liclen - 5, raw, (int)sizeof(raw));
	if (n != 70) {
		KI_ERR("drmn: [lic] b64u 解出 %d 字节，应为 70\n", n);
		return 0;
	}

	/* msg = "PLQ3" + NUL + machine[16] + NUL + payload[6] */
	msg[0] = 'P'; msg[1] = 'L'; msg[2] = 'Q'; msg[3] = '3'; msg[4] = 0;
	for (i = 0; i < 16; i++)
		msg[5 + i] = (uint8_t)mid[i];
	msg[21] = 0;
	for (i = 0; i < 6; i++)
		msg[22 + i] = raw[i];

	/* ed25519_verify(signature, message, message_len, public_key) */
	if (!ed25519_verify(raw + 6, msg, sizeof(msg), ki_lic_pubkey)) {
		KI_ERR("drmn: [lic] ed25519 验签失败 msg0-8=%02x%02x%02x%02x%02x%02x%02x%02x sig0=%02x\n",
		       msg[0], msg[1], msg[2], msg[3], msg[4], msg[5], msg[6], msg[7],
		       raw[6]);
		return 0;
	}

	issued_days = ((uint32_t)raw[0] << 8) | (uint32_t)raw[1];
	expires_days = ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];

	if (!issued_days) {
		KI_ERR("drmn: [lic] issued_days=0\n");
		return 0;
	}
	if (expires_days && expires_days < issued_days) {
		KI_ERR("drmn: [lic] expires %u < issued %u\n", expires_days, issued_days);
		return 0;
	}

	return 1;
}

static void ki_sign(const char *salt, const char *uids, const char *dirs,
		    const char *argno, uint8_t out[32])
{
	struct ki_sha256_ctx c;
	uint8_t sep = '|';

	ki_sha256_init(&c);
	/* 只有 salt 非空才计入，否则用户态传入空 salt 会让两边算法不一致 */
	if (salt && salt[0]) {
		ki_sha256_update(&c, salt, (uint32_t)ki_strlen(salt));
		ki_sha256_update(&c, &sep, 1);
	}
	if (uids) {
		ki_sha256_update(&c, uids, (uint32_t)ki_strlen(uids));
		ki_sha256_update(&c, &sep, 1);
	}
	if (dirs) {
		ki_sha256_update(&c, dirs, (uint32_t)ki_strlen(dirs));
		ki_sha256_update(&c, &sep, 1);
	}
	if (argno)
		ki_sha256_update(&c, argno, (uint32_t)ki_strlen(argno));
	ki_sha256_final(&c, out);
}

static int ki_hexval(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

/* 期望签名是 hex 字符串；比对前 32 位十六进制（SHA256 全 64 位亦可） */
static int ki_verify_sig(const char *salt, const char *uids, const char *dirs,
			 const char *argno, const char *want, int wantlen)
{
	static const char hex[] = "0123456789abcdef";
	uint8_t mac[32];
	char got[65];
	int i;

	if (!want || wantlen < 32 || wantlen > 64)
		return 0;

	ki_sign(salt, uids, dirs, argno, mac);
	for (i = 0; i < 32; i++) {
		got[i * 2] = hex[mac[i] >> 4];
		got[i * 2 + 1] = hex[mac[i] & 0xF];
	}
	got[64] = 0;

	for (i = 0; i < wantlen; i++) {
		int a = ki_hexval(want[i]);
		int b = ki_hexval(got[i]);

		if (a < 0 || b < 0 || a != b)
			return 0;
	}
	return 1;
}

static long ki_init(const char *args, const char *event, void *__user reserved)
{
	const char *val;
	int len;
	uint64_t addr;
	hook_err_t herr;

	(void)reserved;

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
	ki_nsupp_want = 0;
	ki_nsupp_hooked = 0;
	ki_nwraps = 0;
	ki_hits = 0;
	ki_lookups = 0;
	ki_mutates = 0;
	ki_readlinks = 0;
	ki_cpu_hits = 0;
	ki_cpu_scanned = 0;
	ki_hooked_cpu = 0;
	ki_hooked_open = 0;
	ki_hooked_lookup = 0;
	ki_hooked_mutate = 0;
	ki_hooked_rename = 0;
	ki_hooked_symlink = 0;
	ki_hooked_readlink = 0;
	ki_enabled = 1;

	if (args && ki_take_value(args, "uids", &val, &len))
		ki_nuids = ki_parse_u32_list(val, val + len, ki_uids, KI_MAX_UIDS);

	if (args && ki_take_value(args, "dirs", &val, &len))
		ki_parse_csv(val, len, ki_dir_specs, KI_MAX_DIRS, &ki_ndirs_want);

	if (args && ki_take_value(args, "suppress", &val, &len))
		ki_parse_supps(val, len);

	ki_want_open = 1;
	ki_want_lookup = 1;
	if (args && ki_take_value(args, "open", &val, &len)) {
		if (len >= 1 && val[0] == '0')
			ki_want_open = 0;
	}
	if (args && ki_take_value(args, "lookup", &val, &len)) {
		if (len >= 1 && val[0] == '0')
			ki_want_lookup = 0;
	}

	ki_hook_full = 0;
	if (args && ki_take_value(args, "hooks", &val, &len)) {
		if (len >= 4 && val[0] == 'f' && val[1] == 'u' &&
		    val[2] == 'l' && val[3] == 'l')
			ki_hook_full = 1;
	}

	/*
	 * CPU 伪装档位：cpu=8t / cpu=16t / cpu=0（默认关）。
	 * 文本编译进 .rodata，这里只选指针。默认关是为了向后兼容——
	 * 老调用方式不传 cpu 就完全不受影响。
	 */
	ki_cpu_enabled = 0;
	ki_cpuinfo_text = 0;
	if (args && ki_take_value(args, "cpu", &val, &len) && len >= 2) {
		if (val[0] == '8' && val[1] == 't') {
			ki_cpuinfo_text = ki_cpuinfo_8t;
			ki_cpu_enabled = 1;
		} else if (val[0] == '1' && val[1] == '6') {
			ki_cpuinfo_text = ki_cpuinfo_16t;
			ki_cpu_enabled = 1;
		}
	}

	KI_LOG("drmn: uids=%d dirs=%d supp=%d\n", ki_nuids, ki_ndirs_want, ki_nsupp_want);

	/*
	 * 参数签名校验。
	 *
	 * 语义：
	 *   sig 必须提供且匹配，否则直接返回错误、一个 hook 都不挂。
	 *
	 * 校验失败时不"部分工作"，因为部分生效比完全不生效更危险：
	 * 用户会以为拦截在跑，实际却漏了目标应用。
	 */
	{
		const char *v_uids = 0, *v_dirs = 0, *v_salt = 0, *v_sig = 0, *v_argno = 0;
		/* 每个参数必须有独立的长度变量：共用一个会让后取的键覆盖前面的长度，
		 * 实测就是这么错的——l_uids 被 argno 的 1 覆盖后，uids 只拷到首字符，
		 * 算出的签名永远对不上。 */
		int l_uids = 0, l_dirs = 0, l_salt = 0, l_sig = 0, l_argno = 0;

		if (args) {
			ki_take_value(args, "uids", &v_uids, &l_uids);
			ki_take_value(args, "dirs", &v_dirs, &l_dirs);
			ki_take_value(args, "salt", &v_salt, &l_salt);
			ki_take_value(args, "argno", &v_argno, &l_argno);
			ki_take_value(args, "sig", &v_sig, &l_sig);
		}
		(void)l_salt;

		if (v_sig && l_sig > 0) {
			/* 把 uids/dirs/argno/salt 的原文按长度取出，避免依赖 NUL 结尾。
			 * 关键：args 是一整条字符串，各键的值指向它内部，后面还接着别的键，
			 * 直接 ki_strlen 会把后续键也吃进去——salt 就踩过这个坑。 */
			char b_uids[512], b_dirs[1024], b_argno[8], b_salt[160];
			int i;

			b_uids[0] = 0; b_dirs[0] = 0; b_argno[0] = 0; b_salt[0] = 0;
			if (v_uids) {
				for (i = 0; i < l_uids && i < 511 && v_uids[i] &&
					    v_uids[i] != ' ' && v_uids[i] != '\t'; i++)
					b_uids[i] = v_uids[i];
				b_uids[i] = 0;
			}
			if (v_dirs) {
				for (i = 0; i < l_dirs && i < 1023 && v_dirs[i] &&
					    v_dirs[i] != ' ' && v_dirs[i] != '\t'; i++)
					b_dirs[i] = v_dirs[i];
				b_dirs[i] = 0;
			}
			if (v_argno) {
				for (i = 0; i < 7 && v_argno[i] &&
					    v_argno[i] != ' ' && v_argno[i] != '\t'; i++)
					b_argno[i] = v_argno[i];
				b_argno[i] = 0;
			}
			if (v_salt) {
				for (i = 0; i < l_salt && i < 159 && v_salt[i] &&
					    v_salt[i] != ' ' && v_salt[i] != '\t'; i++)
					b_salt[i] = v_salt[i];
				b_salt[i] = 0;
			}

			if (!ki_verify_sig(b_salt, b_uids, b_dirs, b_argno, v_sig, l_sig)) {
				KI_ERR("drmn: 参数签名不匹配，拒绝加载\n");
				ki_enabled = 0;
				return -EACCES;
			}
			KI_LOG("drmn: 参数签名校验通过\n");
		} else {
			KI_ERR("drmn: 缺少参数签名，拒绝加载\n");
			ki_enabled = 0;
			return -EACCES;
		}
	}

	/*
	 * 许可证校验：必须提供 lic= 与 mid=，且 tag 与本机机器码匹配。
	 *
	 * 这一层是防「拷走即用」的关键。之前的「兼容模式」（不传 sig 就跳过）
	 * 等于没有验证——别人拷走 .kpm 直接 kpatch kpm load 就能用。
	 * 现在缺 lic 或缺 mid 一律拒绝，不再有跳过分支。
	 */
	{
		const char *v_lic = 0, *v_mid = 0;
		int l_lic = 0, l_mid = 0;

		if (args) {
			ki_take_value(args, "lic", &v_lic, &l_lic);
			ki_take_value(args, "mid", &v_mid, &l_mid);
		}
		(void)l_mid;

		if (!v_lic || l_lic <= 0 || !v_mid) {
			KI_ERR("drmn: 缺少许可证或机器码，拒绝加载\n");
			ki_enabled = 0;
			return -EACCES;
		}

		{
			/* PLQ3 卡密 99 字符，缓冲必须够。
			 * 早先是 b_lic[64] 且 i<63，把 99 字符截成 63，
			 * 表现为「正确卡密也报 liclen=63」——改格式时漏改这里。 */
			char b_lic[128], b_mid[24];
			int i;

			b_lic[0] = 0; b_mid[0] = 0;
			for (i = 0; i < l_lic && i < 127 && v_lic[i] &&
				    v_lic[i] != ' ' && v_lic[i] != '\t'; i++)
				b_lic[i] = v_lic[i];
			b_lic[i] = 0;
			for (i = 0; i < 23 && v_mid[i] &&
				    v_mid[i] != ' ' && v_mid[i] != '\t'; i++)
				b_mid[i] = v_mid[i];
			b_mid[i] = 0;

			if (!ki_verify_license(b_lic, ki_strlen(b_lic), b_mid)) {
				KI_ERR("drmn: 许可证无效或与本机不匹配，拒绝加载\n");
				ki_enabled = 0;
				return -EACCES;
			}
			KI_LOG("drmn: 许可证校验通过（mid=%s）\n", b_mid);
		}
	}

	if (ki_nuids <= 0)
		KI_LOG("drmn: 没有目标 uid，模块不会做任何事\n");

	/* hook do_filp_open：拦 open 族（open=0 可单独关掉，用于二分定位） */
	if (ki_want_open) {
		addr = kallsyms_lookup_name("do_filp_open");

		KI_LOG("drmn: do_filp_open=%llx\n", (unsigned long long)addr);
		if (addr) {
			herr = ki_register_wrap((void *)addr, 3,
					 (void *)ki_open_before, (void *)0, (void *)0);
			KI_LOG("drmn: do_filp_open hook rc=%d\n", (int)herr);
			if (herr == HOOK_NO_ERR)
				ki_hooked_open = 1;
		}
	} else {
		KI_LOG("drmn: open=0，跳过 do_filp_open\n");
	}

	/* hook filename_lookup：拦 stat/access 族（lookup=0 可单独关掉） */
	if (ki_want_lookup) {
		addr = kallsyms_lookup_name("filename_lookup");

		KI_LOG("drmn: filename_lookup=%llx\n", (unsigned long long)addr);
		if (addr) {
			herr = ki_register_wrap((void *)addr, 5,
					 (void *)ki_lookup_before, (void *)0, (void *)0);
			KI_LOG("drmn: filename_lookup hook rc=%d\n", (int)herr);
			if (herr == HOOK_NO_ERR)
				ki_hooked_lookup = 1;
		}
	} else {
		KI_LOG("drmn: lookup=0，跳过 filename_lookup\n");
	}

	if (ki_hook_full) {
	/*
	 * 写操作：mkdir / unlink / rmdir 是三个独立入口，必须各自 hook。
	 * 之前写成 kallsyms_lookup_name("do_mkdirat") 失败才去找 unlinkat、
	 * 再失败才找 rmdir —— 那是"三选一"，do_mkdirat 一存在就再也不 hook
	 * unlinkat，结果 rm 能穿过（实测 mut_hit 只在 mkdir 时增长）。
	 *
	 * argno 必须逐个对上：do_mkdirat 是 3 个参数，但 do_unlinkat 和
	 * do_rmdir 只有 2 个。统一按 3 传会让 KernelPatch 去保存不存在的
	 * 第 3 个参数、读到栈上垃圾 —— 实测这就是加载后随即崩溃的原因。
	 */
	{
		static const struct {
			const char *name;
			int argno;
		} _muts[] = {
			{ "do_mkdirat",  3 },
			{ "do_unlinkat", 2 },
			{ "do_rmdir",    2 },
		};
		int _mi;

		for (_mi = 0; _mi < 3; _mi++) {
			addr = kallsyms_lookup_name(_muts[_mi].name);
			if (!addr) {
				KI_LOG("drmn: %s 不存在，跳过\n", _muts[_mi].name);
				continue;
			}
			herr = ki_register_wrap((void *)addr, _muts[_mi].argno,
					 (void *)ki_mutate_before, (void *)0, (void *)0);
			KI_LOG("drmn: %s hook rc=%d (argno=%d)\n",
			       _muts[_mi].name, (int)herr, _muts[_mi].argno);
			if (herr == HOOK_NO_ERR)
				ki_hooked_mutate++;
		}
	}

	/* 双路径：rename / link —— filename 在 arg1 和 arg3 */
	addr = kallsyms_lookup_name("do_renameat2");
	if (!addr)
		addr = kallsyms_lookup_name("do_linkat");
	if (addr) {
		herr = ki_register_wrap((void *)addr, 5,
				 (void *)ki_rename_before, (void *)0, (void *)0);
		KI_LOG("drmn: do_renameat2/linkat hook rc=%d\n", (int)herr);
		if (herr == HOOK_NO_ERR)
			ki_hooked_rename = 1;
	}

	/* do_symlinkat(from, newdfd, to) —— arg0 和 arg2 */
	addr = kallsyms_lookup_name("do_symlinkat");
	if (addr) {
		herr = ki_register_wrap((void *)addr, 3,
				 (void *)ki_symlink_before, (void *)0, (void *)0);
		KI_LOG("drmn: do_symlinkat hook rc=%d\n", (int)herr);
		if (herr == HOOK_NO_ERR)
			ki_hooked_symlink = 1;
	}

	/* readlink：只有 dentry，用 dentry_path_raw 反查路径 */
	ki_dentry_path_raw = (char *(*)(void *, char *, int))
		kallsyms_lookup_name("dentry_path_raw");
	addr = kallsyms_lookup_name("vfs_readlink");
	if (addr && ki_dentry_path_raw) {
		herr = ki_register_wrap((void *)addr, 3,
				 (void *)ki_readlink_before, (void *)0, (void *)0);
		KI_LOG("drmn: vfs_readlink hook rc=%d\n", (int)herr);
		if (herr == HOOK_NO_ERR)
			ki_hooked_readlink = 1;
	} else {
		KI_LOG("drmn: vfs_readlink/dentry_path_raw 缺失，readlink 不拦\n");
	}
	} else {
		KI_LOG("drmn: hooks=core，只挂 2 个主 hook\n");
	}

	/*
	 * CPU 伪装：hook seq_open，把 op 表换成自己的。
	 * 与 KI/目录拦截相互独立——即使主 hook 失败，CPU 伪装只要能挂上
	 * 就应该继续工作，所以这段放在「主 hook 全失败就退出」之前。
	 */
	if (ki_cpu_enabled) {
		ki_seq_puts = (void *(*)(void *, const char *))
			kallsyms_lookup_name("seq_puts");
		ki_cpuinfo_op_addr = kallsyms_lookup_name("cpuinfo_op");
		ki_cpuinfo_ops.start = (void *(*)(void *, void *))ki_sp_start;
		ki_cpuinfo_ops.stop = (void (*)(void *, void *))ki_sp_stop;
		ki_cpuinfo_ops.next = (void *(*)(void *, void *, void *))ki_sp_next;
		ki_cpuinfo_ops.show = (int (*)(void *, void *))ki_sp_show;

		KI_LOG("drmn: seq_puts=%llx cpuinfo_op=%llx\n",
		       (unsigned long long)(uintptr_t)ki_seq_puts,
		       (unsigned long long)ki_cpuinfo_op_addr);
		if (!ki_seq_puts) {
			KI_ERR("drmn: 找不到 seq_puts，CPU 伪装无法输出\n");
			ki_cpu_enabled = 0;
		} else if (!ki_cpuinfo_op_addr) {
			KI_ERR("drmn: 找不到 cpuinfo_op，CPU 伪装无法识别目标\n");
			ki_cpu_enabled = 0;
		} else {
			addr = kallsyms_lookup_name("seq_open");
			KI_LOG("drmn: seq_open=%llx\n", (unsigned long long)addr);
			if (addr) {
				herr = ki_register_wrap((void *)addr, 2,
						 (void *)ki_seqopen_before, (void *)0, (void *)0);
				KI_LOG("drmn: seq_open hook rc=%d\n", (int)herr);
				if (herr == HOOK_NO_ERR)
					ki_hooked_cpu = 1;
			}
			if (!ki_hooked_cpu) {
				KI_ERR("drmn: seq_open 挂不上，CPU 伪装停用\n");
				ki_cpu_enabled = 0;
			}
		}
	}

	if (!ki_hooked_open && !ki_hooked_lookup) {
		ki_enabled = 0;
		ki_unregister_wraps();
		ki_cpu_enabled = 0;
		KI_ERR("drmn: 两个主 hook 点都挂不上，无法工作\n");
		return -EIO;
	}

	if (ki_nsupp_want > 0)
		ki_install_supps();

	KI_LOG("drmn: active (uids=%d dirs=%d open=%d lookup=%d supp=%d/%d cpu=%d)\n",
		ki_nuids, ki_ndirs_want, ki_hooked_open, ki_hooked_lookup,
		ki_nsupp_hooked, ki_nsupp_want, ki_hooked_cpu);
	return 0;
}

static long ki_control0(const char *args, char *__user out_msg, int outlen)
{
	char status[320];
	int n;

	/*
	 * 这版 kpatch 没有 ctl1 命令，所以把 lock/unlock 合并进 ctl0：
	 *   kpatch kpm ctl0 <name> toggle   -> 切换拦截开关
	 *   kpatch kpm ctl0 <name> 任意非空 -> 只读状态
	 * 注意 CTL_ARGS 必须非空，否则 kpatch 自己就返回 rc=234 了。
	 * 这里用 args[0]=='t' 判断，避免引入 strcmp 依赖。
	 */
	if (args && args[0] == 't' && args[1] == 'o')
		ki_enabled = !ki_enabled;

	n = ki_fill_status(status, (int)sizeof(status));
	KI_LOG("drmn: ctl0 locked=%d %s\n", ki_enabled, status);

	/*
	 * 严格按 outlen 截断。上一版这里硬拷 sizeof(status)=320 字节，
	 * 而 kpatch 侧给的用户缓冲远小于此 —— 越界写导致内核崩（实测重启两次）。
	 */
	if (out_msg && outlen > 0) {
		if (n > outlen)
			n = outlen;
		compat_copy_to_user(out_msg, status, n);
	}

	return 0;
}

/* ctl1 切换 lock/unlock */
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
	int i;

	(void)reserved;
	ki_enabled = 0;
	ki_unregister_wraps();
	ki_hooked_open = 0;
	ki_hooked_lookup = 0;
	ki_hooked_mutate = 0;
	ki_hooked_rename = 0;
	ki_hooked_symlink = 0;
	ki_hooked_readlink = 0;

	for (i = 0; i < ki_nsupp_hooked; i++) {
		if (ki_supp_funcs[i])
			unhook((void *)ki_supp_funcs[i]);
		ki_supp_funcs[i] = 0;
	}
	ki_nsupp_hooked = 0;

	KI_LOG("drmn: unloaded (open_hit=%llu look_hit=%llu)\n",
		(unsigned long long)ki_hits, (unsigned long long)ki_lookups);
	return 0;
}

KPM_INIT(ki_init);
KPM_CTL0(ki_control0);
KPM_CTL1(ki_control1);
KPM_EXIT(ki_exit);
