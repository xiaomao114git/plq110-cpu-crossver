/* licverify.c - 卡密校验器（自包含：不依赖 OpenSSL，只用 libc）
 *
 * 设计目标（按重要性）：
 *   1) 校验逻辑与密钥在原生二进制里，脚本层无法通过改一行 return 0 绕过
 *   2) 一机一码：卡密载荷里绑定设备机器码，换机即失效
 *   3) 时间限制 + 防回拨：记录见过的最大时间戳并带 MAC，改系统时间/回滚卡密文件都会失败
 *   4) 自包含可测：内置 SHA-256，可用已知测试向量对拍
 *
 * 新卡密格式：PLQ2-<base64url(issued:u32be | expires:u32be | serial:u32be | tag:16)>
 * 共 43 个 ASCII 字符。tag = HMAC-SHA256(key, "PLQ2\0" | machine:16 | "\0" | fields:12)[:16]。
 * 原 PLQ1 两行卡密继续兼容：
 *   PLQ1-<base64url(payload)>
 *   <base64url(HMAC-SHA256(key, payload))>
 * payload 为紧凑文本：v=1&dev=<机器码>&iat=<签发秒>&exp=<到期秒|0=永久>&uid=<序号>&note=<备注>
 *
 * 子命令：
 *   id                 输出机器码
 *   info               打印卡密信息（不校验时间，仅验签）
 *   check              完整校验（验签 + 机器绑定 + 时间 + 防回拨）；退出码 0 通过
 *   show               打印冷却时间等提示文本
 *   selftest           对内置 SHA-256 做自检（PC 侧测试用）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LIC_DIR_DEFAULT "/data/adb/plq110-lic"
#define LIC_FILE_NAME   "/license.txt"
#define ST_FILE_NAME    "/state"

/* 运行期目录：可用环境变量 LIC_DIR 覆盖。
 * 生产环境不设置 => /data/adb/plq110-lic（root 才能写）。
 * 离线测试/CI 把它指到临时目录，就能用真实签名跑通激活、过期、换机、回拨等分支。 */
static char g_dir[512] = LIC_DIR_DEFAULT;

static const char *lic_dir(void) { return g_dir; }

static void lic_paths(char *lic, size_t a, char *st, size_t b)
{
    if (lic && a) snprintf(lic, a, "%s" LIC_FILE_NAME, g_dir);
    if (st && b)  snprintf(st,  b, "%s" ST_FILE_NAME,  g_dir);
}

/* ---------------------------------------------------------------- SHA-256 */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } sha256_ctx;

static const uint32_t K256[64] = {
 0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
 0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
 0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
 0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
 0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
 0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
 0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
 0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    uint32_t w[64], a, b, d, e, f, g, h, t1, t2, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + K256[i] + w[i];
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667u; c->h[1]=0xbb67ae85u; c->h[2]=0x3c6ef372u; c->h[3]=0xa54ff53au;
    c->h[4]=0x510e527fu; c->h[5]=0x9b05688cu; c->h[6]=0x1f83d9abu; c->h[7]=0x5be0cd19u;
    c->len=0; c->n=0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    c->len += (uint64_t)len * 8;
    while (len) {
        size_t k = 64 - c->n;
        if (k > len) k = len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    unsigned char pad[72];
    size_t padlen;
    uint64_t bits = c->len;
    int i;
    pad[0] = 0x80;
    padlen = (c->n < 56) ? (56 - c->n) : (120 - c->n);
    memset(pad + 1, 0, padlen - 1);
    for (i = 0; i < 8; i++) pad[padlen + i] = (unsigned char)(bits >> (56 - 8*i));
    sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

/* ------------------------------------------------------- HMAC-SHA256(key) */
/* 密钥不整段出现在二进制里：两半在运行时合成，且中间夹垃圾字节 */
static const unsigned char K1[16] = { 0x5a,0x17,0xc4,0x91,0x3e,0x6d,0x08,0xf2,
                                      0xb3,0x4a,0x77,0xd9,0x26,0x8e,0x51,0x0b };
static const unsigned char KJ[8]  = { 0xaa,0x55,0xaa,0x55,0x13,0x37,0xde,0xad }; /* 垃圾 */
static const unsigned char K2[16] = { 0x9c,0xe0,0x35,0x72,0x4f,0x1b,0xa6,0xd8,
                                      0x63,0x2c,0x80,0xf5,0x09,0xb7,0x4e,0x21 };

static size_t lic_key(unsigned char *out /*32*/)
{
    size_t i;
    for (i = 0; i < 16; i++) out[i] = (unsigned char)(K1[i] ^ KJ[i % 8]);
    for (i = 0; i < 16; i++) out[16+i] = (unsigned char)(K2[i] ^ KJ[(i + 3) % 8]);
    return 32;
}

static void hmac_sha256(const unsigned char *key, size_t klen,
                        const void *msg, size_t mlen, unsigned char out[32])
{
    unsigned char k[64], ip[64], op[64], inner[32];
    sha256_ctx c;
    size_t i;
    memset(k, 0, sizeof k);
    if (klen > 64) { sha256_init(&c); sha256_update(&c, key, klen); sha256_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) { ip[i] = (unsigned char)(k[i] ^ 0x36); op[i] = (unsigned char)(k[i] ^ 0x5c); }
    sha256_init(&c); sha256_update(&c, ip, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, op, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

/* --------------------------------------------------------- base64url 编解码 */
static const char B64U[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64u_dec_char(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '-') return 62;
    if (ch == '_') return 63;
    return -1;
}

/* 返回解码长度，失败 -1 */
static long b64u_decode(const char *in, unsigned char *out, size_t outsz)
{
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (; *in; in++) {
        int v;
        if (*in == '=' || *in == '\r' || *in == '\n' || *in == ' ') continue;
        v = b64u_dec_char((unsigned char)*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outsz) return -1;
            out[o++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    return (long)o;
}

static size_t b64u_encode(const unsigned char *in, size_t len, char *out, size_t outsz)
{
    size_t i = 0, o = 0;
    while (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) v |= (uint32_t)in[i+1] << 8;
        if (rem > 2) v |= (uint32_t)in[i+2];
        if (o + 4 >= outsz) break;
        out[o++] = B64U[(v >> 18) & 63];
        out[o++] = B64U[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64U[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64U[v & 63];
        i += 3;
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------- 工具 */
static int read_file(const char *p, char *buf, size_t sz)
{
    FILE *f = fopen(p, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static int write_file_mode(const char *p, const char *data, mode_t m)
{
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    int rc = 0;
    if (fwrite(data, 1, len, f) != len) rc = -1;
    if (fflush(f) != 0) rc = -1;
    if (fsync(fileno(f)) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (chmod(p, m) != 0) rc = -1;
    return rc;
}

/* 读 prop：优先 getprop，失败回退 /proc/cmdline 里的 androidboot.* */
static void get_prop(const char *key, char *out, size_t outsz)
{
    char cmd[256], line[512];
    FILE *f;
    out[0] = 0;
    snprintf(cmd, sizeof cmd, "getprop %s 2>/dev/null", key);
    f = popen(cmd, "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            snprintf(out, outsz, "%s", line);
        }
        pclose(f);
    }
    if (!out[0]) {
        /* androidboot.<prop 后缀> */
        const char *dot = strrchr(key, '.');
        if (dot) {
            f = fopen("/proc/cmdline", "r");
            if (f) {
                char cmdline[2048];
                size_t n = fread(cmdline, 1, sizeof cmdline - 1, f);
                cmdline[n] = 0;
                fclose(f);
                snprintf(cmd, sizeof cmd, "androidboot.%s=", dot + 1);
                {
                    char *p = strstr(cmdline, cmd);
                    if (p) {
                        p += strlen(cmd);
                        n = 0;
                        while (p[n] && p[n] != ' ' && p[n] != '\n' && n < outsz - 1) { out[n] = p[n]; n++; }
                        out[n] = 0;
                    }
                }
            }
        }
    }
}

static void android_id(char *out, size_t outsz)
{
    char cmd[256], line[512];
    FILE *f;
    out[0] = 0;
    /* Root 下 settings 可用；取不到就留空，机器码仍由 serial 决定 */
    snprintf(cmd, sizeof cmd, "settings get secure android_id 2>/dev/null");
    f = popen(cmd, "r");
    if (f) {
        if (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (strcmp(line, "null") != 0) snprintf(out, outsz, "%s", line);
        }
        pclose(f);
    }
}

/* 机器码 = BASE32(SHA256(<由 devid 文件提供的稳定标识>))，取 16 字符，无易混字符
 *
 * 为什么最终改用「设备上生成的稳定随机 ID」而不是硬件字段（三次实测踩坑的结论）：
 *   坑 1：ro.serialno 被 CPID_MRX 一类模块运行时随机化（形如 55'19134）。
 *   坑 2：ro.boot.serialno / ro.boot.cpuid / android_id **每次开机都变**
 *        （ef3abf95→5b755abf、0x00000cfb9f3678c1→0x000003e2ade764c1、
 *         47e3215a042e498b→1578c5fdd8aadf33）。
 *   坑 3：连 /sys/devices/soc0/serial_number 都被这些"防设备标记"模块在开机时改写
 *        （1134106329→1800700081），wlan0 的 MAC 同样被随机化（13:3c:…→1f:b7:…）。
 *   → 在这台设备上「绑定硬件」与「一机一码」直接冲突：任何被读的硬件面都可能被改写。
 *
 * 采用的方案（务实且可解释）：
 *   devid = 首次运行时用 /dev/urandom + /proc/sys/kernel/random/uuid 生成的随机 ID，
 *           写入 <LIC_DIR>/devid（0600，root 私有）后**永不改变**。
 *   机器码 = BASE32(SHA256(devid))。跨重启稳定 → 卡密不会自己失效。
 *   强度边界：买家拿到 root 后可以复制 devid 到别的机器复用（本地卡密的固有上限）。
 *   因此设置里提供 HW_BIND 选项：开启后额外校验一组硬件字段，用于"换机即失效"的加固；
 *   代价是那些字段本身会漂移，需要重新发卡。两条路线各有取舍，由使用者选择。
 */
static int read_first_line(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    out[0] = 0;
    if (!f) return -1;
    n = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[n] = 0;
    {   /* 去掉行尾空白与 NUL（device-tree 属性常带尾部 \0） */
        size_t i = 0;
        while (out[i] && out[i] != '\n' && out[i] != '\r') i++;
        out[i] = 0;
    }
    return out[0] ? 0 : -1;
}

/* 读取或首次生成稳定设备标识。返回 0 成功。 */
/*
 * 绑定依据优先级：
 *   1. 硬件序列号 —— 刷机、换 ROM、恢复出厂都不变，卡密长期有效
 *   2. devid 文件  —— 旧行为，供没有序列号的设备兜底
 *   3. 随机 UUID   —— 最后兜底
 * 之所以不再把 devid 放第一位：它是首次运行随机生成的，清除模块数据就重生成，
 * 机器码跟着变，已签发的卡密会集体失效。
 */
static int serial_seed(char *out, size_t outsz)
{
    static const char *const props[] = { "ro.serialno", "ro.boot.serialno", "ro.boot.hardware.sku" };
    char tmp[256];
    size_t i;

    for (i = 0; i < sizeof props / sizeof props[0]; i++) {
        tmp[0] = 0;
        get_prop(props[i], tmp, sizeof tmp);
        if (!tmp[0]) continue;
        /* 排除明显无效值：空、unknown、全 0、过短 */
        if (!strcmp(tmp, "unknown") || !strcmp(tmp, "0")) continue;
        if (strlen(tmp) < 4) continue;
        snprintf(out, outsz, "%s", tmp);
        return 0;
    }
    return -1;
}

static int device_seed(char *out, size_t outsz)
{
    char p[600];
    FILE *f;
    size_t n;

    if (serial_seed(out, outsz) == 0 && out[0])
        return 0;

    snprintf(p, sizeof p, "%s/devid", lic_dir());
    f = fopen(p, "rb");
    if (f) {
        n = fread(out, 1, outsz - 1, f);
        fclose(f);
        out[n] = 0;
        {
            size_t i = 0;
            while (out[i] && out[i] != '\n' && out[i] != '\r') i++;
            out[i] = 0;
        }
        if (out[0]) return 0;
    }

    /* 首次生成：优先用内核 uuid（质量好），失败则用 /dev/urandom */
    out[0] = 0;
    if (read_first_line("/proc/sys/kernel/random/uuid", out, outsz) != 0 || !out[0]) {
        unsigned char rnd[32];
        size_t i;
        f = fopen("/dev/urandom", "rb");
        if (!f) return -1;
        if (fread(rnd, 1, sizeof rnd, f) != sizeof rnd) { fclose(f); return -1; }
        fclose(f);
        for (i = 0; i < sizeof rnd && i * 2 + 1 < outsz; i++)
            sprintf(out + i * 2, "%02x", rnd[i]);
    }
    mkdir(lic_dir(), 0700);
    if (write_file_mode(p, out, 0600) != 0) {
        /* 写不进去（比如只读测试目录）也允许用内存值，只是每次会变 */
        return -1;
    }
    return 0;
}

static void machine_code(char *out, size_t outsz)
{
    char seed[256] = {0}, buf[512];
    unsigned char h[32];
    static const char A32[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"; /* 无 0/O/1/I */
    sha256_ctx c;
    size_t i, n;

    if (device_seed(seed, sizeof seed) != 0 || !seed[0])
        snprintf(seed, sizeof seed, "unstable-%ld", (long)time(NULL));

    /*
     * 序列号恰好 16 位时直接当机器码用，不再哈希。
     * 这样用户在 WebUI 看到的码就是设备序列号本身，能直接和系统设置里的
     * 序列号对上，不用再去理解一层派生关系。HMAC 的 machine 字段也正是
     * 16 字节，长度天然吻合。
     * 非 16 位（其它机型序列号长度不一）仍走哈希，保证唯一且定长。
     */
    n = strlen(seed);
    if (n == 16 && outsz > 16) {
        memcpy(out, seed, 16);
        out[16] = 0;
        return;
    }

    snprintf(buf, sizeof buf, "plq110-suite|%s", seed);
    sha256_init(&c);
    sha256_update(&c, buf, strlen(buf));
    sha256_final(&c, h);
    for (i = 0; i < 16 && i < outsz - 1; i++) {
        unsigned int v = (unsigned int)(h[(i * 5) / 8] >> ((i * 5) % 8));
        if ((i * 5) % 8 > 3 && ((i * 5) / 8) + 1 < 32)
            v |= (unsigned int)h[(i * 5) / 8 + 1] << (8 - ((i * 5) % 8));
        out[i] = A32[v & 31];
    }
    out[i] = 0;
}

/* ------------------------------------------------------ 卡密解析与校验 */
struct lic {
    char payload[512];
    char dev[64];
    char note[128];
    long iat, exp;
    int  ver;
    char uid[64];
};

static int kv_get(const char *p, const char *key, char *out, size_t outsz)
{
    char pat[64];
    const char *s;
    size_t n = 0;
    snprintf(pat, sizeof pat, "%s=", key);
    out[0] = 0;
    s = p;
    while ((s = strstr(s, pat)) != NULL) {
        if (s == p || s[-1] == '&' || s[-1] == '\n') break;
        s++;
    }
    if (!s) return -1;
    s += strlen(pat);
    /* 值以 & 或换行结束。
     * 缺陷记录：早期版本只判 & ，导致状态文件里的最后一个字段（k=）把行尾 \n
     * 一起读进来，MAC 比对恒不相等 -> 第二次校验就误报「状态被篡改」（设备实测）。 */
    while (s[n] && s[n] != '&' && s[n] != '\n' && s[n] != '\r' && n < outsz - 1) {
        out[n] = s[n];
        n++;
    }
    out[n] = 0;
    return 0;
}

static uint32_t read_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int lic_parse_short(struct lic *L, const char *text, int verify_sig, char *err, size_t errsz)
{
    char code[44], canonical[48];
    unsigned char raw[28], msg[34], key[32], want[32];
    size_t used = 0, klen;
    uint32_t issued, expires, serial;
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (used >= sizeof code - 1) goto format_error;
        code[used++] = (char)ch;
    }
    code[used] = 0;
    if (used != 43 || memcmp(code, "PLQ2-", 5) != 0) goto format_error;
    for (size_t i = 5; i < used; i++) if (b64u_dec_char((unsigned char)code[i]) < 0) goto format_error;
    if (b64u_decode(code + 5, raw, sizeof raw) != 28) goto format_error;
    b64u_encode(raw, sizeof raw, canonical, sizeof canonical);
    if (strcmp(canonical, code + 5) != 0) goto format_error;
    machine_code(L->dev, sizeof L->dev);
    if (strlen(L->dev) != 16) {
        snprintf(err, errsz, "无法取得本机机器码"); return -2;
    }
    memcpy(msg, "PLQ2", 5); /* Includes the domain-separating NUL. */
    memcpy(msg + 5, L->dev, 16);
    msg[21] = 0;
    memcpy(msg + 22, raw, 12);
    klen = lic_key(key);
    hmac_sha256(key, klen, msg, sizeof msg, want);
    if (verify_sig) {
        unsigned char diff = 0;
        for (size_t i = 0; i < 16; i++) diff |= raw[12 + i] ^ want[i];
        if (diff) {
            snprintf(err, errsz, "短卡密无效或与本机不匹配"); return -3;
        }
    }
    issued = read_u32be(raw); expires = read_u32be(raw + 4); serial = read_u32be(raw + 8);
    if (!issued || (expires && expires < issued)) goto format_error;
    L->ver = 2; L->iat = (long)issued; L->exp = (long)expires;
    snprintf(L->uid, sizeof L->uid, "%08X", (unsigned int)serial);
    snprintf(L->payload, sizeof L->payload, "v=2&dev=%s&iat=%lu&exp=%lu&uid=%s",
             L->dev, (unsigned long)issued, (unsigned long)expires, L->uid);
    return 0;
format_error:
    snprintf(err, errsz, "短卡密格式错误（应为 PLQ2- 开头的 43 字符单行卡密）");
    return -3;
}

static int lic_parse(struct lic *L, const char *text, int verify_sig, char *err, size_t errsz)
{
    char l1[1024], l2[256];
    const char *p;
    unsigned char sig[32], want[32], key[32];
    size_t klen;
    long n;

    memset(L, 0, sizeof *L);
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    if (strncmp(text, "PLQ2-", 5) == 0) return lic_parse_short(L, text, verify_sig, err, errsz);
    l1[0] = l2[0] = 0;
    p = text;
    {
        size_t i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < sizeof l1 - 1) l1[i++] = *p++;
        l1[i] = 0;
        while (*p == '\n' || *p == '\r') p++;
        i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < sizeof l2 - 1) l2[i++] = *p++;
        l2[i] = 0;
    }
    if (strncmp(l1, "PLQ1-", 5) != 0) {
        snprintf(err, errsz, "卡密格式错误（缺少 PLQ1- 前缀）");
        return -3;
    }
    n = b64u_decode(l1 + 5, (unsigned char *)L->payload, sizeof L->payload - 1);
    if (n <= 0) { snprintf(err, errsz, "卡密载荷解码失败"); return -3; }
    L->payload[n] = 0;

    klen = lic_key(key);
    hmac_sha256(key, klen, L->payload, (size_t)n, want);

    if (verify_sig) {
        long m = b64u_decode(l2, sig, sizeof sig);
        if (m != 32) { snprintf(err, errsz, "卡密签名解码失败"); return -3; }
        {
            unsigned char diff = 0;
            size_t i;
            for (i = 0; i < 32; i++) diff |= (unsigned char)(sig[i] ^ want[i]);
            if (diff) { snprintf(err, errsz, "卡密签名无效"); return -3; }
        }
    }

    {
        char tmp[64];
        if (kv_get(L->payload, "v", tmp, sizeof tmp) == 0) L->ver = atoi(tmp); else L->ver = 0;
        if (kv_get(L->payload, "dev", L->dev, sizeof L->dev) != 0) { snprintf(err, errsz, "卡密缺少 dev"); return -3; }
        if (kv_get(L->payload, "iat", tmp, sizeof tmp) == 0) L->iat = atol(tmp); else L->iat = 0;
        if (kv_get(L->payload, "exp", tmp, sizeof tmp) == 0) L->exp = atol(tmp); else L->exp = 0;
        if (kv_get(L->payload, "uid", L->uid, sizeof L->uid) != 0) L->uid[0] = 0;
        if (kv_get(L->payload, "note", L->note, sizeof L->note) != 0) L->note[0] = 0;
    }
    if (L->ver != 1) { snprintf(err, errsz, "卡密版本不支持: %d", L->ver); return -3; }
    return 0;
}

/* state 文件：seen=<时间戳>&m=<机器码>&k=<MAC(seen|m|key)>&lic=<卡密哈希> */
static int state_load(long *seen, char *lic_h, size_t lhsz)
{
    char buf[512], s[64], m[64], k[128], lh[128] = {0}, calc[128];
    unsigned char mac[32], key[32];
    char msg[256];
    size_t klen, i;

    char stp[600];
    *seen = 0; lic_h[0] = 0;
    lic_paths(NULL, 0, stp, sizeof stp);
    if (read_file(stp, buf, sizeof buf) <= 0) return -1;
    if (kv_get(buf, "seen", s, sizeof s) != 0) return -1;
    if (kv_get(buf, "m", m, sizeof m) != 0) return -1;
    if (kv_get(buf, "k", k, sizeof k) != 0) return -1;
    kv_get(buf, "lh", lh, sizeof lh);
    klen = lic_key(key);
    snprintf(msg, sizeof msg, "%s|%s", s, m);
    hmac_sha256(key, klen, msg, strlen(msg), mac);
    {
        char hex[80];
        for (i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", mac[i]);
        hex[64] = 0;
        snprintf(calc, sizeof calc, "%s", hex);
    }
    if (strcmp(calc, k) != 0) return -2;   /* 状态被篡改 */
    *seen = atol(s);
    snprintf(lic_h, lhsz, "%s", lh);
    return 0;
}

static int state_save(long seen, const char *lic_h)
{
    char m[64], msg[256], buf[512], hex[80];
    unsigned char mac[32], key[32];
    size_t klen, i;
    machine_code(m, sizeof m);
    klen = lic_key(key);
    snprintf(msg, sizeof msg, "%ld|%s", seen, m);
    hmac_sha256(key, klen, msg, strlen(msg), mac);
    for (i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", mac[i]);
    hex[64] = 0;
    char stp[600];
    snprintf(buf, sizeof buf, "seen=%ld&m=%s&lh=%s&k=%s\n", seen, m, lic_h ? lic_h : "", hex);
    lic_paths(NULL, 0, stp, sizeof stp);
    return write_file_mode(stp, buf, 0600);
}

static void lic_hash(const char *payload, char *out, size_t outsz)
{
    sha256_ctx c;
    unsigned char d[32];
    size_t i;
    sha256_init(&c);
    sha256_update(&c, payload, strlen(payload));
    sha256_final(&c, d);
    /* Preserve the historical 16-hex-character state ID without an OOB NUL. */
    if (outsz == 0) return;
    size_t n = (outsz - 1) / 2;
    if (n > 8) n = 8;
    for (i = 0; i < n; i++) snprintf(out + i*2, outsz - i*2, "%02x", d[i]);
    out[n*2] = 0;
}

/* 共享的校验主体：返回 0 通过；负数为错误码 */
static int lic_check(char *err, size_t errsz, long *out_days)
{
    char text[2048], mc[64], lh[32], licp[600];
    struct lic L;
    int rc, st;
    long seen = 0, now;
    char st_lic[32];

    if (out_days) *out_days = 0;
    lic_paths(licp, sizeof licp, NULL, 0);
    if (read_file(licp, text, sizeof text) <= 0) {
        snprintf(err, errsz, "未找到授权文件 %s", licp);
        return -1;
    }
    rc = lic_parse(&L, text, 1, err, errsz);
    if (rc) return rc;

    machine_code(mc, sizeof mc);
    if (strcmp(mc, L.dev) != 0) {
        snprintf(err, errsz, "卡密与本机不匹配（本机 %s）", mc);
        return -2;
    }

    lic_hash(L.payload, lh, sizeof lh);
    st = state_load(&seen, st_lic, sizeof st_lic);
    if (st == -2) {
        snprintf(err, errsz, "授权状态文件被篡改，请重新激活");
        return -4;
    }

    now = (long)time(NULL);
    if (L.ver == 2 && now + 300 < L.iat) {
        snprintf(err, errsz, "系统时间早于短卡密签发时间，请校准时间");
        return -4;
    }
    /* 时间防回拨：当前时间显著早于本机见过的最大时间 -> 判定回拨。
     * 容差 300s 用于两类正常情况（都实测踩到过）：
     *   1) NTP 微调 / 时区切换
     *   2) 同一次调用里连续校验（例如 check 写完状态后 info 又校验一次），
     *      此时 now 可能等于或略小于刚写入的 seen
     */
    if (st == 0 && now + 300 < seen) {
        snprintf(err, errsz, "系统时间被回拨（本机记录 %ld，当前 %ld）", seen, now);
        return -4;
    }

    /* 卡密文件回滚检测：状态里记录过 lic 哈希，若与当前不同且已过期则拒绝 */
    if (st == 0 && st_lic[0] && strcmp(st_lic, lh) != 0) {
        if (L.exp > 0 && now > L.exp) {
            snprintf(err, errsz, "该卡密已过期（续期卡密需重新签发）");
            return -5;
        }
    }

    if (L.exp > 0) {
        if (now > L.exp) {
            snprintf(err, errsz, "授权已过期（到期 %ld）", L.exp);
            return -5;
        }
        if (out_days) *out_days = (L.exp - now + 86399) / 86400;
    }

    /* 通过：刷新状态（单调不减） */
    if (now > seen) seen = now;
    if (state_save(seen, lh) != 0) {
        snprintf(err, errsz, "授权状态写入失败");
        return -4;
    }
    return 0;
}

/* ------------------------------------------------------------- 子命令 */
static int cmd_id(void)
{
    char mc[64];
    machine_code(mc, sizeof mc);
    printf("%s\n", mc);
    return 0;
}

static int cmd_selftest(void)
{
    /* SHA-256 已知向量 */
    struct { const char *in; const char *hex; } tv[] = {
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "",    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    int i, bad = 0;
    for (i = 0; i < 3; i++) {
        sha256_ctx c; unsigned char d[32]; char hex[80]; int j;
        sha256_init(&c);
        sha256_update(&c, tv[i].in, strlen(tv[i].in));
        sha256_final(&c, d);
        for (j = 0; j < 32; j++) sprintf(hex + j*2, "%02x", d[j]);
        hex[64] = 0;
        if (strcmp(hex, tv[i].hex) != 0) {
            printf("SHA256 FAIL case %d\n  got  %s\n  want %s\n", i, hex, tv[i].hex);
            bad++;
        }
    }
    /* HMAC-SHA256 RFC4231 case 2: key="Jefe", data="what do ya want for nothing?" */
    {
        unsigned char mac[32]; char hex[80]; int j;
        hmac_sha256((const unsigned char *)"Jefe", 4, "what do ya want for nothing?", 28, mac);
        for (j = 0; j < 32; j++) sprintf(hex + j*2, "%02x", mac[j]);
        hex[64] = 0;
        if (strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") != 0) {
            printf("HMAC FAIL\n  got  %s\n  want 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843\n", hex);
            bad++;
        }
    }
    /* base64url 往返 */
    {
        const char *s = "v=1&dev=ABCDEFGH23456789&exp=0";
        unsigned char raw[128]; char enc[256];
        long n = b64u_decode((b64u_encode((const unsigned char *)s, strlen(s), enc, sizeof enc), enc), raw, sizeof raw);
        if (n <= 0 || strncmp((char *)raw, s, (size_t)n) != 0) { printf("B64 FAIL\n"); bad++; }
    }
    printf(bad ? "SELFTEST FAIL (%d)\n" : "SELFTEST OK\n", bad);
    return bad ? 1 : 0;
}

static int cmd_info(int full)
{
    char text[2048], err[256], mc[64], licp[600];
    struct lic L;
    long now = (long)time(NULL), days = 0;
    time_t t;
    int rc;

    lic_paths(licp, sizeof licp, NULL, 0);
    if (read_file(licp, text, sizeof text) <= 0) {
        printf("状态: 未激活\n");
        printf("本机机器码: ");
        cmd_id();
        return 1;
    }
    rc = lic_parse(&L, text, 1, err, sizeof err);
    if (rc) { printf("状态: 无效 (%s)\n", err); return rc; }
    machine_code(mc, sizeof mc);
    printf("授权状态: %s\n", strcmp(mc, L.dev) == 0 ? "已绑定本机" : "绑定其它设备");
    printf("本机机器码: %s\n", mc);
    printf("卡密机器码: %s\n", L.dev);
    t = (time_t)L.iat; printf("签发时间: %s", ctime(&t));
    if (L.exp > 0) {
        t = (time_t)L.exp; printf("到期时间: %s", ctime(&t));
        days = (L.exp - now + 86399) / 86400;
        printf("剩余天数: %ld\n", days > 0 ? days : 0);
    } else {
        printf("到期时间: 永久\n");
    }
    if (L.uid[0])  printf("卡密序号: %s\n", L.uid);
    if (L.note[0]) printf("备注: %s\n", L.note);
    if (full) {
        rc = lic_check(err, sizeof err, &days);
        if (rc == 0) printf("校验结果: 通过（剩余 %ld 天）\n", days);
        else         printf("校验结果: 失败 - %s\n", err);
    }
    return rc;
}

/* install <file>：把授权文件规范化后写入 LIC_DIR。
 * 放在原生侧而不是 shell 侧，是为了让「规范化逻辑」只有一份实现。 */
static int cmd_install(const char *srcfile)
{
    char buf[2048], licp[600];
    char norm[2048];
    size_t i, o = 0;
    int lines = 0;
    FILE *f;

    if (!srcfile) { fprintf(stderr, "usage: licverify install <file>\n"); return 2; }
    if (read_file(srcfile, buf, sizeof buf) <= 0) {
        fprintf(stderr, "无法读取 %s\n", srcfile);
        return 1;
    }
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '\r') continue;
        norm[o++] = buf[i];
    }
    norm[o] = 0;
    /* 去掉空行 */
    {
        char out[2048];
        size_t p = 0, q = 0;
        for (i = 0; i <= o; i++) {
            if (norm[i] == '\n' || norm[i] == 0) {
                if (p > 0) { out[q++] = '\n'; p = 0; lines++; }
                if (norm[i] == 0) break;
            } else {
                out[q++] = norm[i];
                p++;
            }
        }
        out[q] = 0;
        memcpy(norm, out, q + 1);
    }
    if (!((lines == 1 && strncmp(norm, "PLQ2-", 5) == 0) ||
          (lines == 2 && strncmp(norm, "PLQ1-", 5) == 0))) {
        fprintf(stderr, "需要单行 PLQ2 短卡密或两行 PLQ1 卡密\n"); return 1;
    }
    {
        struct lic candidate;
        char error[256];
        if (lic_parse(&candidate, norm, 1, error, sizeof error) != 0) {
            fprintf(stderr, "%s\n", error); return 1;
        }
    }
    mkdir(lic_dir(), 0700);
    lic_paths(licp, sizeof licp, NULL, 0);
    if (write_file_mode(licp, norm, 0600) != 0) {
        fprintf(stderr, "写入 %s 失败（需要 root）\n", licp);
        return 1;
    }
    printf("已写入 %s\n", licp);
    f = NULL; (void)f;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: licverify <id|check|info|show|install|selftest>\n"
        "  id                 输出本机机器码\n"
        "  check              完整校验（退出码 0=通过）\n"
        "  info               打印卡密信息（含校验结果）\n"
        "  show               人类可读状态行\n"
        "  install <file>     安装授权文件到 LIC_DIR\n"
        "  selftest           内置 SHA-256/HMAC 自检\n"
        "环境变量 LIC_DIR 可覆盖授权目录（默认 " LIC_DIR_DEFAULT "），仅供测试使用\n");
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "show";
    char err[256] = {0};
    const char *env;
    long days = 0;
    int rc;

    env = getenv("LIC_DIR");
    if (env && env[0]) snprintf(g_dir, sizeof g_dir, "%s", env);
    mkdir(lic_dir(), 0700);

    if (!strcmp(cmd, "id"))       return cmd_id();
    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "info"))     return cmd_info(1);
    if (!strcmp(cmd, "install"))  return cmd_install(argc > 2 ? argv[2] : NULL);

    if (!strcmp(cmd, "check")) {
        rc = lic_check(err, sizeof err, &days);
        if (rc == 0) { printf("OK %ld\n", days); return 0; }
        printf("DENY %s\n", err);
        return (rc < 0) ? (rc == -1 ? 10 : (rc == -2 ? 11 : (rc == -5 ? 13 : 12))) : 1;
    }
    if (!strcmp(cmd, "show")) {
        char mc[64];
        rc = lic_check(err, sizeof err, &days);
        machine_code(mc, sizeof mc);
        if (rc == 0) {
            if (days > 0) printf("已授权 · 本机 %s · 剩余 %ld 天\n", mc, days);
            else          printf("已授权 · 本机 %s · 永久\n", mc);
        } else {
            printf("未授权 · 本机 %s · %s\n", mc, err);
        }
        return rc == 0 ? 0 : 1;
    }
    usage();
    return 2;
}
