package com.plq110.issuer;

import android.app.Activity;
import android.os.Bundle;
import android.view.KeyEvent;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * 卡密发卡器外壳。
 *
 * 只做一件事：把 assets/issuer.html 加载进 WebView。
 * 生成逻辑全在页面里的纯 JS（SHA256/HMAC 手写实现），
 * 因为 file:// 下 crypto.subtle 不可用，不能依赖 WebCrypto。
 *
 * 刻意不申请任何权限、不联网：
 *   - manifest 里没有 INTERNET 权限，WebView 就算想发请求也发不出去
 *   - 输出结果走剪贴板（页面内 execCommand 兜底），不落盘
 */
public class MainActivity extends Activity {

    private WebView web;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        web = new WebView(this);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        // 页面用 localStorage 存最近生成的记录，需要开 DOM storage
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        // Android 11+ 默认禁止 file:// 页面访问同源资源，
        // 这里 assets 与页面同源，显式放开以避免白屏。
        s.setAllowFileAccessFromFileURLs(true);
        s.setAllowUniversalAccessFromFileURLs(true);
        // 防止页面溢出后整页可缩放（手机上体验更好）
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);

        web.setWebViewClient(new WebViewClient());
        web.setBackgroundColor(0xFF080A0D);

        web.loadUrl("file:///android_asset/issuer.html");
        setContentView(web);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && web != null && web.canGoBack()) {
            web.goBack();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    protected void onDestroy() {
        if (web != null) {
            web.loadUrl("about:blank");
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
