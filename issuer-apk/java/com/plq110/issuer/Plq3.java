package com.plq110.issuer;

import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.util.Calendar;
import java.util.TimeZone;

/**
 * PLQ3 卡密编解码。与电脑端 issue_plq3.py、内核侧 ki_guard_kpm.c 三处必须完全一致。
 *
 *   code    = "PLQ3-" + base64url( payload[6] || sig[64] )      共 99 字符
 *   payload = issued_days:u16be | expires_days:u16be | serial:u16be
 *   msg     = "PLQ3\0" | machine[16] | "\0" | payload[6]       共 28 字节
 *   sig     = Ed25519_sign(私钥, msg)
 *
 * issued_days 以 2020-01-01 UTC 为原点，expires_days=0 表示永久。
 * 天数只用 2 字节——比 4 字节时间戳省 4 字节，卡密短一点。
 */
public final class Plq3 {

    public static final String PREFIX = "PLQ3-";
    public static final int CODE_LEN = 99;
    public static final long EPOCH_MILLIS = 1577836800000L;  // 2020-01-01T00:00:00Z

    private Plq3() {}

    public static int daysSinceEpoch() {
        long now = System.currentTimeMillis();
        return (int) ((now - EPOCH_MILLIS) / 86400000L);
    }

    public static String dateOf(int days) {
        Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        c.setTimeInMillis(EPOCH_MILLIS + days * 86400000L);
        return String.format("%04d-%02d-%02d",
                c.get(Calendar.YEAR), c.get(Calendar.MONTH) + 1, c.get(Calendar.DAY_OF_MONTH));
    }

    private static byte[] message(String machine, byte[] payload) {
        byte[] m = machine.getBytes(StandardCharsets.US_ASCII);
        if (m.length != 16) {
            throw new IllegalArgumentException("机器码必须 16 个字符，当前 " + m.length);
        }
        byte[] msg = new byte[28];
        msg[0] = 'P'; msg[1] = 'L'; msg[2] = 'Q'; msg[3] = '3'; msg[4] = 0;
        System.arraycopy(m, 0, msg, 5, 16);
        msg[21] = 0;
        System.arraycopy(payload, 0, msg, 22, 6);
        return msg;
    }

    private static byte[] payload(int issuedDays, int expiresDays, int serial) {
        if (issuedDays < 0 || issuedDays > 0xFFFF) throw new IllegalArgumentException("签发日超出 u16");
        if (expiresDays < 0 || expiresDays > 0xFFFF) throw new IllegalArgumentException("到期日超出 u16");
        if (serial < 0 || serial > 0xFFFF) throw new IllegalArgumentException("序号必须在 0..65535");
        return new byte[]{
                (byte) (issuedDays >> 8), (byte) issuedDays,
                (byte) (expiresDays >> 8), (byte) expiresDays,
                (byte) (serial >> 8), (byte) serial
        };
    }

    /** 签发。days=0 表示永久。 */
    public static String issue(byte[] seed, String machine, int serial, int days) {
        int issued = daysSinceEpoch();
        int expires = days == 0 ? 0 : issued + days;
        if (expires > 0xFFFF) throw new IllegalArgumentException("有效期过长");
        byte[] pl = payload(issued, expires, serial);
        byte[] sig = Ed25519.sign(seed, message(machine, pl));
        byte[] raw = new byte[70];
        System.arraycopy(pl, 0, raw, 0, 6);
        System.arraycopy(sig, 0, raw, 6, 64);
        return PREFIX + Base64.encodeToString(raw,
                Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    /** 校验结果。 */
    public static final class Result {
        public boolean ok;
        public String why = "";
        public int serial, issued, expires;
        public String issuedText = "", expiresText = "", daysLeft = "";
    }

    public static Result verify(byte[] pub, String code, String machine) {
        Result r = new Result();
        String c = code.replaceAll("[\\s]", "");
        if (!c.startsWith(PREFIX)) {
            r.why = "前缀不是 " + PREFIX;
            return r;
        }
        if (c.length() != CODE_LEN) {
            r.why = "长度 " + c.length() + "，应为 " + CODE_LEN;
            return r;
        }
        byte[] raw;
        try {
            raw = Base64.decode(c.substring(5),
                    Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
        } catch (Exception e) {
            r.why = "base64url 解码失败";
            return r;
        }
        if (raw.length != 70) {
            r.why = "载荷 " + raw.length + " 字节，应为 70";
            return r;
        }
        byte[] pl = new byte[6];
        byte[] sig = new byte[64];
        System.arraycopy(raw, 0, pl, 0, 6);
        System.arraycopy(raw, 6, sig, 0, 64);

        byte[] msg;
        try {
            msg = message(machine, pl);
        } catch (IllegalArgumentException e) {
            r.why = e.getMessage();
            return r;
        }
        if (!Ed25519.verify(pub, msg, sig)) {
            r.why = "签名不匹配（机器码不对，或卡密被改过）";
            return r;
        }
        r.serial = ((pl[4] & 0xff) << 8) | (pl[5] & 0xff);
        r.issued = ((pl[0] & 0xff) << 8) | (pl[1] & 0xff);
        r.expires = ((pl[2] & 0xff) << 8) | (pl[3] & 0xff);
        if (r.issued == 0) {
            r.why = "签发日为 0，非法";
            return r;
        }
        if (r.expires != 0 && r.expires < r.issued) {
            r.why = "到期日早于签发日";
            return r;
        }
        r.issuedText = dateOf(r.issued);
        r.expiresText = r.expires == 0 ? "永久" : dateOf(r.expires);
        int now = daysSinceEpoch();
        r.daysLeft = r.expires == 0 ? "∞" : String.valueOf(r.expires - now);
        r.ok = true;
        return r;
    }
}
