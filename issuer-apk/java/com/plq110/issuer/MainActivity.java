package com.plq110.issuer;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.text.InputType;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * 小猫发卡器（PLQ3 / Ed25519）。
 *
 * 界面全部用代码搭：这样 APK 不需要 res/ 目录，aapt2 只处理 manifest，
 * 打包脚本能保持「无 gradle」的极简形态。
 *
 * 安全约束：
 *   - 清单里没有 INTERNET 权限，卡密和私钥都不可能经网络出去。
 *   - 私钥以异或混淆的字节数组内置；`strings` 抓不到明文。
 *     但要清楚：谁能拿到这个 APK 并反编译，谁就拿到私钥。它只适合留在自己手机上。
 *   - 生成结果不上屏以外的任何地方（不写日志、不写文件）。
 */
public class MainActivity extends Activity {

    // 私钥 = 内置字节 ^ MASK。与 keys/plq3_signing.key 对应。
    private static final byte[] MASK = {
            (byte) 0x5a, 0x17, (byte) 0xc4, (byte) 0x91, 0x3e, 0x6d, 0x08, (byte) 0xf2,
            (byte) 0xb3, 0x4a, 0x77, (byte) 0xd9, 0x26, (byte) 0x8e, 0x51, 0x0b,
            (byte) 0x9c, (byte) 0xe0, 0x35, 0x72, 0x4f, 0x1b, (byte) 0xa6, (byte) 0xd8,
            0x63, 0x2c, (byte) 0x80, (byte) 0xf5, 0x09, (byte) 0xb7, 0x4e, 0x21
    };
    private static final byte[] OBF = {
            0x34, 0x48, (byte) 0xfb, 0x38, (byte) 0x88, (byte) 0xaa, (byte) 0xcd, 0x43,
            (byte) 0x8a, (byte) 0xdd, 0x47, (byte) 0xe6, 0x7f, 0x46, (byte) 0x82, 0x75,
            0x45, (byte) 0xef, 0x31, (byte) 0x90, 0x6f, 0x6a, (byte) 0xc1, 0x27,
            (byte) 0x92, (byte) 0xe5, (byte) 0x98, 0x2f, 0x68, 0x58, 0x03, 0x35
    };
    /** 发卡端公钥，应与 KPM 内置 ki_lic_pubkey 一致 */
    private static final String PUBKEY_HEX =
            "0cb425e5a1e8af285f27d187e030acfa3fdafeddd7229246a14505f61b7ebdeb";

    private static final int BG = 0xFF11151C;
    private static final int CARD = 0xFF1A2028;
    private static final int FG = 0xFFE6EDF3;
    private static final int DIM = 0xFF8B98A5;
    private static final int ACCENT = 0xFF4B9FFF;
    private static final int OKC = 0xFF3FB950;
    private static final int ERRC = 0xFFF85149;

    private EditText machineIn, serialIn, daysIn, codeIn;
    private TextView out, selfTest, verifyOut;
    private final ExecutorService pool = Executors.newSingleThreadExecutor();

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        buildUi();
        applyIntentExtras();
        runSelfTest();
    }

    /**
     * 支持带参数启动，便于脚本化出卡与自测：
     *   adb shell am start -n com.plq110.issuer/.MainActivity \
     *       --es machine 3B15CQ00X4C00000 --ei serial 7 --ei days 0 --ez issue true
     * 不带参数就是普通手动操作，行为不变。
     */
    private void applyIntentExtras() {
        android.content.Intent it = getIntent();
        if (it == null) {
            return;
        }
        String m = it.getStringExtra("machine");
        if (m == null || m.trim().isEmpty()) {
            return;
        }
        machineIn.setText(m.trim());
        if (it.hasExtra("serial")) {
            serialIn.setText(String.valueOf(it.getIntExtra("serial", 1)));
        }
        if (it.hasExtra("days")) {
            daysIn.setText(String.valueOf(it.getIntExtra("days", 0)));
        }
        if (it.getBooleanExtra("issue", false)) {
            doIssue();
        }
    }

    @Override
    protected void onDestroy() {
        pool.shutdownNow();
        super.onDestroy();
    }

    // ---------------- UI ----------------

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    private TextView label(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(DIM);
        t.setTextSize(12f);
        t.setPadding(0, dp(10), 0, dp(4));
        return t;
    }

    private EditText input(String hint, int inputType, String def) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setHintTextColor(DIM);
        e.setTextColor(FG);
        e.setTextSize(15f);
        e.setInputType(inputType);
        if (def != null) {
            e.setText(def);
        }
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF0D1117);
        bg.setCornerRadius(dp(8));
        bg.setStroke(dp(1), 0xFF2A333D);
        e.setBackground(bg);
        e.setPadding(dp(10), dp(8), dp(10), dp(8));
        return e;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(14), dp(12), dp(14), dp(14));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(CARD);
        bg.setCornerRadius(dp(12));
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        c.setLayoutParams(lp);
        return c;
    }

    private TextView title(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(FG);
        t.setTextSize(15f);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, 0, 0, dp(6));
        return t;
    }

    private Button btn(String s, int color) {
        Button b = new Button(this);
        b.setText(s);
        b.setTextColor(Color.WHITE);
        b.setTextSize(14f);
        b.setAllCaps(false);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(8));
        b.setBackground(bg);
        b.setPadding(dp(12), dp(6), dp(12), dp(6));
        return b;
    }

    private void buildUi() {
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(BG);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(20), dp(16), dp(28));
        sv.addView(root);

        TextView h = new TextView(this);
        h.setText("小猫发卡器");
        h.setTextColor(FG);
        h.setTextSize(21f);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(h);

        TextView sub = new TextView(this);
        sub.setText("PLQ3 · Ed25519 非对称签名");
        sub.setTextColor(DIM);
        sub.setTextSize(12f);
        sub.setPadding(0, dp(2), 0, dp(12));
        root.addView(sub);

        // 自检
        LinearLayout st = card();
        st.addView(title("签名实现自检"));
        selfTest = new TextView(this);
        selfTest.setTextSize(12f);
        selfTest.setTextColor(DIM);
        st.addView(selfTest);
        root.addView(st);

        // 签发
        LinearLayout issueCard = card();
        issueCard.addView(title("签发"));
        issueCard.addView(label("机器码（对方在模块 WebUI 首页读给你，16 位）"));
        machineIn = input("例如 3B15CQ00X4C00000", InputType.TYPE_CLASS_TEXT, "");
        issueCard.addView(machineIn);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout col1 = new LinearLayout(this);
        col1.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams c1lp = new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        c1lp.rightMargin = dp(10);
        col1.setLayoutParams(c1lp);
        col1.addView(label("序号（每张卡不同）"));
        serialIn = input("1", InputType.TYPE_CLASS_NUMBER, "1");
        col1.addView(serialIn);
        LinearLayout col2 = new LinearLayout(this);
        col2.setOrientation(LinearLayout.VERTICAL);
        col2.setLayoutParams(new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        col2.addView(label("有效天数（0 = 永久）"));
        daysIn = input("0", InputType.TYPE_CLASS_NUMBER, "0");
        col2.addView(daysIn);
        row.addView(col1);
        row.addView(col2);
        issueCard.addView(row);

        Button gen = btn("生成卡密", ACCENT);
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        glp.topMargin = dp(12);
        gen.setLayoutParams(glp);
        gen.setOnClickListener(v -> doIssue());
        issueCard.addView(gen);

        out = new TextView(this);
        out.setTextColor(OKC);
        out.setTextSize(13f);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextIsSelectable(true);
        out.setPadding(0, dp(12), 0, 0);
        out.setText("");
        issueCard.addView(out);

        Button copy = btn("复制卡密", 0xFF2A333D);
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.topMargin = dp(8);
        copy.setLayoutParams(clp);
        copy.setOnClickListener(v -> {
            String s = out.getText().toString().trim();
            int idx = s.indexOf("PLQ3-");
            if (idx < 0) {
                toast("还没有生成");
                return;
            }
            String code = s.substring(idx).split("\\s")[0];
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("PLQ3", code));
            toast("已复制 " + code.length() + " 字符");
        });
        issueCard.addView(copy);
        root.addView(issueCard);

        // 校验
        LinearLayout ver = card();
        ver.addView(title("校验"));
        ver.addView(label("卡密"));
        codeIn = input("PLQ3-…", InputType.TYPE_CLASS_TEXT, "");
        ver.addView(codeIn);
        Button vb = btn("校验这张卡", 0xFF2A333D);
        LinearLayout.LayoutParams vlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        vlp.topMargin = dp(10);
        vb.setLayoutParams(vlp);
        vb.setOnClickListener(v -> doVerify());
        ver.addView(vb);
        verifyOut = new TextView(this);
        verifyOut.setTextColor(DIM);
        verifyOut.setTextSize(12f);
        verifyOut.setPadding(0, dp(10), 0, 0);
        ver.addView(verifyOut);
        root.addView(ver);

        // 公钥
        LinearLayout pk = card();
        pk.addView(title("公钥"));
        TextView pkt = new TextView(this);
        pkt.setText(PUBKEY_HEX);
        pkt.setTextColor(FG);
        pkt.setTextSize(11f);
        pkt.setTypeface(Typeface.MONOSPACE);
        pkt.setTextIsSelectable(true);
        pk.addView(pkt);
        TextView pkn = new TextView(this);
        pkn.setText("必须与 KPM 内置的 ki_lic_pubkey 一致，否则所有卡都会被拒。");
        pkn.setTextColor(DIM);
        pkn.setTextSize(11f);
        pkn.setPadding(0, dp(6), 0, 0);
        pk.addView(pkn);
        root.addView(pk);

        TextView foot = new TextView(this);
        foot.setText("本机不联网 · 私钥仅存于此设备");
        foot.setTextColor(DIM);
        foot.setTextSize(11f);
        foot.setGravity(Gravity.CENTER);
        foot.setPadding(0, dp(6), 0, 0);
        root.addView(foot);

        setContentView(sv);
    }

    // ---------------- 逻辑 ----------------

    private byte[] seed() {
        byte[] s = new byte[32];
        for (int i = 0; i < 32; i++) {
            s[i] = (byte) (OBF[i] ^ MASK[i]);
        }
        return s;
    }

    private byte[] pub() {
        return Ed25519.unhex(PUBKEY_HEX);
    }

    private void runSelfTest() {
        selfTest.setText("正在跑 RFC 8032 向量…");
        pool.execute(() -> {
            long t0 = System.currentTimeMillis();
            String err = Ed25519.selfTest();
            long ms = System.currentTimeMillis() - t0;
            runOnUiThread(() -> {
                if (err == null) {
                    byte[] derived = Ed25519.publicKey(seed());
                    boolean match = java.util.Arrays.equals(derived, pub());
                    selfTest.setText("RFC 8032 三组向量全部通过（" + ms + " ms）\n"
                            + "内置私钥推导出的公钥" + (match ? "与 KPM 内置一致 ✓" : "★与 KPM 不一致"));
                    selfTest.setTextColor(match ? OKC : ERRC);
                } else {
                    selfTest.setText("★ 自检失败：" + err);
                    selfTest.setTextColor(ERRC);
                }
            });
        });
    }

    private void doIssue() {
        String machine = machineIn.getText().toString().trim();
        if (machine.length() != 16 || !machine.matches("[0-9A-Za-z]{16}")) {
            toast("机器码必须是 16 位字母数字");
            return;
        }
        int serial;
        int days;
        try {
            serial = Integer.parseInt(serialIn.getText().toString().trim());
            days = Integer.parseInt(TextUtils.isEmpty(daysIn.getText()) ? "0"
                    : daysIn.getText().toString().trim());
        } catch (NumberFormatException e) {
            toast("序号和天数必须是数字");
            return;
        }
        out.setText("计算中…");
        pool.execute(() -> {
            try {
                String code = Plq3.issue(seed(), machine, serial, days);
                Plq3.Result chk = Plq3.verify(pub(), code, machine);
                runOnUiThread(() -> {
                    out.setText(code);
                    out.setTextColor(OKC);
                    if (!chk.ok) {
                        out.setText(code + "\n★ 自验失败：" + chk.why);
                        out.setTextColor(ERRC);
                        return;
                    }
                    verifyOut.setText("刚生成的这张自验通过：序号 " + chk.serial
                            + "，到期 " + chk.expiresText);
                    verifyOut.setTextColor(OKC);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    out.setText("★ 生成失败：" + e.getMessage());
                    out.setTextColor(ERRC);
                });
            }
        });
    }

    private void doVerify() {
        String code = codeIn.getText().toString().trim();
        if (code.isEmpty()) {
            toast("先粘贴一张卡密");
            return;
        }
        String machine = machineIn.getText().toString().trim();
        if (machine.length() != 16) {
            toast("校验需要填机器码");
            return;
        }
        verifyOut.setText("校验中…");
        verifyOut.setTextColor(DIM);
        pool.execute(() -> {
            Plq3.Result r = Plq3.verify(pub(), code, machine);
            runOnUiThread(() -> {
                if (r.ok) {
                    verifyOut.setText("有效\n序号 " + r.serial
                            + "\n签发 " + r.issuedText
                            + "\n到期 " + r.expiresText
                            + "\n剩余 " + r.daysLeft + " 天");
                    verifyOut.setTextColor(OKC);
                } else {
                    verifyOut.setText("无效 — " + r.why);
                    verifyOut.setTextColor(ERRC);
                }
            });
        });
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }
}
