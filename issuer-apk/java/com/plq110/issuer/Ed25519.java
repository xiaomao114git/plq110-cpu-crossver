package com.plq110.issuer;

import java.math.BigInteger;
import java.security.MessageDigest;

/**
 * Ed25519 签名/验签（纯 Java，BigInteger 实现）。
 *
 * 为什么不用系统 API：Android 的 Ed25519 到 API 33 才有，而本工具不该挑系统版本。
 * 为什么不搬 ref10：那套 radix 25.5 的 limb 运算在 Java 里毫无性能收益，
 * 反而难核对。这里用 BigInteger 写「教科书版」，慢一个数量级但一眼能看懂，
 * 离线发卡一次几十毫秒完全够。
 *
 * 正确性由 RFC 8032 §7.1 的三组官方向量在自检里强制验证（见 selfTest）。
 * Ed25519 签名是确定性的——同样的密钥和消息必然得到同样的签名，
 * 因此也能和电脑端的 Python 实现逐字节比对。
 */
public final class Ed25519 {

    private static final BigInteger TWO = BigInteger.valueOf(2);
    private static final BigInteger THREE = BigInteger.valueOf(3);
    private static final BigInteger EIGHT = BigInteger.valueOf(8);

    /** p = 2^255 - 19 */
    public static final BigInteger P = TWO.pow(255).subtract(BigInteger.valueOf(19));
    /** L = 2^252 + 27742317777372353535851937790883648493，群的阶 */
    public static final BigInteger L =
            TWO.pow(252).add(new BigInteger("27742317777372353535851937790883648493"));
    /** d = -121665/121666 mod p */
    public static final BigInteger D = BigInteger.valueOf(-121665)
            .multiply(BigInteger.valueOf(121666).modInverse(P)).mod(P);
    /** 2*d mod p */
    private static final BigInteger D2 = D.multiply(TWO).mod(P);
    /** sqrt(-1) mod p */
    public static final BigInteger SQRT_M1 = TWO.modPow(P.subtract(BigInteger.ONE).divide(BigInteger.valueOf(4)), P);

    private Ed25519() {}

    // ---------------- 基础域运算 ----------------

    private static BigInteger inv(BigInteger x) {
        return x.modPow(P.subtract(TWO), P);
    }

    private static BigInteger xrecover(BigInteger y) {
        // x^2 = (y^2 - 1) / (d*y^2 + 1)
        BigInteger y2 = y.multiply(y).mod(P);
        BigInteger num = y2.subtract(BigInteger.ONE).mod(P);
        BigInteger den = D.multiply(y2).add(BigInteger.ONE).mod(P);
        BigInteger x2 = num.multiply(inv(den)).mod(P);
        BigInteger x = x2.modPow(P.add(THREE).divide(EIGHT), P);
        if (!x.multiply(x).subtract(x2).mod(P).equals(BigInteger.ZERO)) {
            x = x.multiply(SQRT_M1).mod(P);
        }
        if (!x.multiply(x).subtract(x2).mod(P).equals(BigInteger.ZERO)) {
            return null;  // 不是曲线上的点
        }
        if (x.testBit(0)) {
            x = P.subtract(x);
        }
        return x;
    }

    // ---------------- 点运算（扩展坐标 X,Y,Z,T） ----------------

    private static final BigInteger BY = BigInteger.valueOf(4)
            .multiply(BigInteger.valueOf(5).modInverse(P)).mod(P);
    private static final BigInteger BX = xrecover(BY);

    /** 基点 B 的扩展坐标 */
    private static BigInteger[] basePoint() {
        return new BigInteger[]{BX, BY, BigInteger.ONE, BX.multiply(BY).mod(P)};
    }

    private static BigInteger[] identity() {
        return new BigInteger[]{BigInteger.ZERO, BigInteger.ONE, BigInteger.ONE, BigInteger.ZERO};
    }

    private static BigInteger[] add(BigInteger[] p, BigInteger[] q) {
        BigInteger a = p[1].subtract(p[0]).multiply(q[1].subtract(q[0])).mod(P);
        BigInteger b = p[1].add(p[0]).multiply(q[1].add(q[0])).mod(P);
        BigInteger c = p[3].multiply(D2).multiply(q[3]).mod(P);
        BigInteger d = p[2].multiply(TWO).multiply(q[2]).mod(P);
        BigInteger e = b.subtract(a);
        BigInteger f = d.subtract(c);
        BigInteger g = d.add(c);
        BigInteger h = b.add(a);
        return new BigInteger[]{
                e.multiply(f).mod(P),
                g.multiply(h).mod(P),
                f.multiply(g).mod(P),
                e.multiply(h).mod(P)
        };
    }

    private static BigInteger[] mul(BigInteger[] p, BigInteger s) {
        BigInteger[] q = identity();
        BigInteger[] cur = p;
        BigInteger k = s;
        while (k.signum() > 0) {
            if (k.testBit(0)) {
                q = add(q, cur);
            }
            cur = add(cur, cur);
            k = k.shiftRight(1);
        }
        return q;
    }

    private static byte[] encode(BigInteger[] p) {
        BigInteger zinv = inv(p[2]);
        BigInteger x = p[0].multiply(zinv).mod(P);
        BigInteger y = p[1].multiply(zinv).mod(P);
        byte[] out = new byte[32];
        for (int i = 0; i < 32; i++) {
            out[i] = (byte) y.shiftRight(8 * i).intValue();
        }
        if (x.testBit(0)) {
            out[31] |= (byte) 0x80;
        }
        return out;
    }

    private static BigInteger[] decode(byte[] s) {
        if (s.length != 32) {
            return null;
        }
        byte[] copy = s.clone();
        int sign = (copy[31] & 0x80) != 0 ? 1 : 0;
        copy[31] &= 0x7f;
        BigInteger y = fromLittleEndian(copy);
        if (y.compareTo(P) >= 0) {
            return null;
        }
        BigInteger x = xrecover(y);
        if (x == null) {
            return null;
        }
        if (x.testBit(0) != (sign == 1)) {
            x = P.subtract(x);
        }
        if (x.equals(BigInteger.ZERO) && sign == 1) {
            return null;
        }
        return new BigInteger[]{x, y, BigInteger.ONE, x.multiply(y).mod(P)};
    }

    // ---------------- 字节/整数 ----------------

    private static BigInteger fromLittleEndian(byte[] b) {
        byte[] be = new byte[b.length];
        for (int i = 0; i < b.length; i++) {
            be[i] = b[b.length - 1 - i];
        }
        return new BigInteger(1, be);
    }

    private static byte[] toLittleEndian(BigInteger v, int len) {
        byte[] out = new byte[len];
        BigInteger x = v;
        for (int i = 0; i < len; i++) {
            out[i] = (byte) x.shiftRight(8 * i).intValue();
        }
        return out;
    }

    private static byte[] sha512(byte[]... parts) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-512");
            for (byte[] p : parts) {
                md.update(p);
            }
            return md.digest();
        } catch (Exception e) {
            throw new IllegalStateException("SHA-512 不可用", e);
        }
    }

    private static BigInteger hashModL(byte[]... parts) {
        return fromLittleEndian(sha512(parts)).mod(L);
    }

    // ---------------- 对外接口 ----------------

    /** 由 32 字节种子推出公钥（RFC 8032 §5.1.5）。 */
    public static byte[] publicKey(byte[] seed) {
        if (seed.length != 32) {
            throw new IllegalArgumentException("种子必须 32 字节");
        }
        byte[] h = sha512(seed);
        byte[] a = new byte[32];
        System.arraycopy(h, 0, a, 0, 32);
        a[0] &= (byte) 248;
        a[31] &= 127;
        a[31] |= 64;
        return encode(mul(basePoint(), fromLittleEndian(a)));
    }

    /** 签名。返回 64 字节（R || S）。 */
    public static byte[] sign(byte[] seed, byte[] message) {
        if (seed.length != 32) {
            throw new IllegalArgumentException("私钥必须 32 字节");
        }
        byte[] h = sha512(seed);
        byte[] aBytes = new byte[32];
        System.arraycopy(h, 0, aBytes, 0, 32);
        aBytes[0] &= (byte) 248;
        aBytes[31] &= 127;
        aBytes[31] |= 64;
        BigInteger a = fromLittleEndian(aBytes);

        byte[] prefix = new byte[32];
        System.arraycopy(h, 32, prefix, 0, 32);

        byte[] pub = encode(mul(basePoint(), a));

        BigInteger r = hashModL(prefix, message);
        byte[] rEnc = encode(mul(basePoint(), r));

        BigInteger k = hashModL(rEnc, pub, message);
        BigInteger s = r.add(k.multiply(a)).mod(L);

        byte[] sig = new byte[64];
        System.arraycopy(rEnc, 0, sig, 0, 32);
        System.arraycopy(toLittleEndian(s, 32), 0, sig, 32, 32);
        return sig;
    }

    /** 验签。 */
    public static boolean verify(byte[] pub, byte[] message, byte[] sig) {
        if (sig.length != 64 || pub.length != 32) {
            return false;
        }
        byte[] sBytes = new byte[32];
        System.arraycopy(sig, 32, sBytes, 0, 32);
        BigInteger s = fromLittleEndian(sBytes);
        if (s.compareTo(L) >= 0) {
            return false;
        }
        byte[] rEnc = new byte[32];
        System.arraycopy(sig, 0, rEnc, 0, 32);
        BigInteger[] rPoint = decode(rEnc);
        BigInteger[] aPoint = decode(pub);
        if (rPoint == null || aPoint == null) {
            return false;
        }
        BigInteger k = hashModL(rEnc, pub, message);
        // 检查 [S]B == R + [k]A
        BigInteger[] left = mul(basePoint(), s);
        BigInteger[] right = add(rPoint, mul(aPoint, k));
        return java.util.Arrays.equals(encode(left), encode(right));
    }

    // ---------------- 自检 ----------------

    public static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte v : b) {
            sb.append(String.format("%02x", v & 0xff));
        }
        return sb.toString();
    }

    public static byte[] unhex(String s) {
        s = s.replaceAll("[^0-9a-fA-F]", "");
        if (s.length() % 2 != 0) {
            throw new IllegalArgumentException("hex 长度必须是偶数");
        }
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }

    /** RFC 8032 §7.1 三组官方向量。任一不符就返回失败原因。 */
    public static String selfTest() {
        String[][] vectors = {
                {
                        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                        "",
                        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
                },
                {
                        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
                        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                        "72",
                        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
                },
                {
                        "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
                        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
                        "af82",
                        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
                }
        };
        for (int i = 0; i < vectors.length; i++) {
            byte[] seed = unhex(vectors[i][0]);
            byte[] wantPub = unhex(vectors[i][1]);
            byte[] msg = unhex(vectors[i][2]);
            byte[] wantSig = unhex(vectors[i][3]);

            byte[] gotPub = publicKey(seed);
            if (!java.util.Arrays.equals(gotPub, wantPub)) {
                return "TEST " + (i + 1) + " 公钥不符: " + hex(gotPub) + " != " + vectors[i][1];
            }
            byte[] gotSig = sign(seed, msg);
            if (!java.util.Arrays.equals(gotSig, wantSig)) {
                return "TEST " + (i + 1) + " 签名不符: " + hex(gotSig) + " != " + vectors[i][3];
            }
            if (!verify(gotPub, msg, gotSig)) {
                return "TEST " + (i + 1) + " 自验失败";
            }
            byte[] bad = gotSig.clone();
            bad[0] ^= 1;
            if (verify(gotPub, msg, bad)) {
                return "TEST " + (i + 1) + " 篡改签名竟通过了";
            }
        }
        return null;  // null = 全部通过
    }
}
