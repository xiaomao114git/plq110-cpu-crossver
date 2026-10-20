/* Ed25519 验签自检：RFC 8032 §7.1 标准测试向量。
 * 只有全部通过才允许集成进 KPM——手写或移植的密码学代码不验证等于没做。 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ed25519.h"

static int hex2bin(const char *hex, unsigned char *out, size_t outlen)
{
	size_t i;
	if (strlen(hex) != outlen * 2)
		return -1;
	for (i = 0; i < outlen; i++) {
		unsigned int v;
		if (sscanf(hex + i * 2, "%2x", &v) != 1)
			return -1;
		out[i] = (unsigned char)v;
	}
	return 0;
}

struct vec {
	const char *name;
	const char *pk;
	const char *msg;
	const char *sig;
};

/* RFC 8032 官方向量（TEST 1/2/3） */
static const struct vec vectors[] = {
	{
		"TEST 1 (empty msg)",
		"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
		"",
		"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
	},
	{
		"TEST 2 (1 byte)",
		"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
		"72",
		"92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
	},
	{
		"TEST 3 (2 bytes)",
		"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
		"af82",
		"6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
	},
};

int main(void)
{
	int passed = 0, failed = 0;
	size_t i;

	printf("Ed25519 verify self-test (RFC 8032)\n");
	printf("===================================\n");

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		const struct vec *v = &vectors[i];
		unsigned char pk[32], sig[64], msg[256];
		size_t mlen = strlen(v->msg) / 2;
		int ok;

		if (hex2bin(v->pk, pk, 32) != 0) {
			printf("  %-22s FAIL (bad pk hex)\n", v->name);
			failed++;
			continue;
		}
		if (hex2bin(v->sig, sig, 64) != 0) {
			printf("  %-22s FAIL (bad sig hex)\n", v->name);
			failed++;
			continue;
		}
		if (mlen > 0 && hex2bin(v->msg, msg, mlen) != 0) {
			printf("  %-22s FAIL (bad msg hex)\n", v->name);
			failed++;
			continue;
		}

		ok = ed25519_verify(sig, msg, mlen, pk);
		printf("  %-22s %s\n", v->name, ok == 1 ? "PASS" : "FAIL (should verify)");
		if (ok == 1) passed++; else failed++;

		/* 篡改一字节必须失败 */
		sig[0] ^= 0x01;
		ok = ed25519_verify(sig, msg, mlen, pk);
		printf("  %-22s %s\n", "  tampered sig", ok == 0 ? "PASS (rejected)" : "FAIL (accepted!)");
		if (ok == 0) passed++; else failed++;
		sig[0] ^= 0x01;

		/* 错误公钥必须失败 */
		pk[0] ^= 0x01;
		ok = ed25519_verify(sig, msg, mlen, pk);
		printf("  %-22s %s\n", "  wrong pk", ok == 0 ? "PASS (rejected)" : "FAIL (accepted!)");
		if (ok == 0) passed++; else failed++;
		pk[0] ^= 0x01;

		/* 消息改动必须失败 */
		if (mlen > 0) {
			msg[0] ^= 0x01;
			ok = ed25519_verify(sig, msg, mlen, pk);
			printf("  %-22s %s\n", "  tampered msg", ok == 0 ? "PASS (rejected)" : "FAIL (accepted!)");
			if (ok == 0) passed++; else failed++;
			msg[0] ^= 0x01;
		}
	}

	printf("===================================\n");
	printf("passed=%d failed=%d\n", passed, failed);

	if (failed) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
