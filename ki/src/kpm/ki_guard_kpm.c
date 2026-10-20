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

static int ki_enabled = 1;

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
	if (!fn || !fn->name)
		return 0;
	if (!ki_uid_hit())
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

static int ki_hooked_open;
static int ki_hooked_lookup;

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
	ki_hits = 0;
	ki_lookups = 0;
	ki_hooked_open = 0;
	ki_hooked_lookup = 0;
	ki_enabled = 1;

	if (args && ki_take_value(args, "uids", &val, &len))
		ki_nuids = ki_parse_u32_list(val, val + len, ki_uids, KI_MAX_UIDS);

	if (args && ki_take_value(args, "dirs", &val, &len))
		ki_parse_csv(val, len, ki_dir_specs, KI_MAX_DIRS, &ki_ndirs_want);

	if (args && ki_take_value(args, "suppress", &val, &len))
		ki_parse_supps(val, len);

	KI_LOG("drmn: uids=%d dirs=%d supp=%d\n", ki_nuids, ki_ndirs_want, ki_nsupp_want);

	if (ki_nuids <= 0)
		KI_LOG("drmn: 没有目标 uid，模块不会做任何事\n");

	/* hook do_filp_open：拦 open 族 */
	addr = kallsyms_lookup_name("do_filp_open");
	if (!addr)
		addr = kallsyms_lookup_name("path_openat");
	KI_LOG("drmn: do_filp_open=%llx\n", (unsigned long long)addr);
	if (addr) {
		herr = hook_wrap((void *)addr, 3,
				 (void *)ki_open_before, (void *)0, (void *)0);
		KI_LOG("drmn: do_filp_open hook rc=%d\n", (int)herr);
		if (herr == HOOK_NO_ERR)
			ki_hooked_open = 1;
	}

	/* hook filename_lookup：拦 stat/access 族 */
	addr = kallsyms_lookup_name("filename_lookup");
	if (!addr)
		addr = kallsyms_lookup_name("path_lookupat");
	KI_LOG("drmn: filename_lookup=%llx\n", (unsigned long long)addr);
	if (addr) {
		herr = hook_wrap((void *)addr, 5,
				 (void *)ki_lookup_before, (void *)0, (void *)0);
		KI_LOG("drmn: filename_lookup hook rc=%d\n", (int)herr);
		if (herr == HOOK_NO_ERR)
			ki_hooked_lookup = 1;
	}

	if (!ki_hooked_open && !ki_hooked_lookup) {
		KI_ERR("drmn: 两个 hook 点都挂不上，无法工作\n");
		return -EIO;
	}

	if (ki_nsupp_want > 0)
		ki_install_supps();

	KI_LOG("drmn: active (uids=%d dirs=%d open=%d lookup=%d supp=%d/%d)\n",
		ki_nuids, ki_ndirs_want, ki_hooked_open, ki_hooked_lookup,
		ki_nsupp_hooked, ki_nsupp_want);
	return 0;
}

static long ki_control0(const char *args, char *__user out_msg, int outlen)
{
	char status[320];
	int n;

	(void)args;

	ki_fill_status(status, (int)sizeof(status));
	KI_LOG("drmn: ctl0 %s\n", status);

	/*
	 * 必须按 outlen 截断：kpatch 侧给的用户缓冲通常只有几十到几百字节，
	 * 直接拷 sizeof(status)=320 会越界，结果就是用户态读到空串
	 * （实测 kpatch kpm ctl0 输出为空，就是这个原因）。
	 * 也不拷结尾 NUL —— 让调用方按自己缓冲的大小处理。
	 */
	n = (int)strlen(status);
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
	uint64_t addr;
	int i;

	(void)reserved;

	if (ki_hooked_open) {
		addr = kallsyms_lookup_name("do_filp_open");
		if (!addr)
			addr = kallsyms_lookup_name("path_openat");
		if (addr)
			unhook((void *)addr);
		ki_hooked_open = 0;
	}

	if (ki_hooked_lookup) {
		addr = kallsyms_lookup_name("filename_lookup");
		if (!addr)
			addr = kallsyms_lookup_name("path_lookupat");
		if (addr)
			unhook((void *)addr);
		ki_hooked_lookup = 0;
	}

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
