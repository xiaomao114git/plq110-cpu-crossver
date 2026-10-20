import com.plq110.issuer.Ed25519;

/**
 * 纯 JVM 自检：不开 Android 也能验证 Ed25519 实现。
 *
 * Ed25519.java 不依赖任何 Android API，所以可以直接用 javac/java 在本机跑。
 * CI 里先跑这个再打 APK——密码学代码错了就别打包了。
 */
public class Ed25519SelfTest {
    public static void main(String[] a) {
        String err = Ed25519.selfTest();
        if (err != null) {
            System.out.println("FAIL  " + err);
            System.exit(1);
        }
        System.out.println("PASS  RFC 8032 §7.1 三组向量全部通过（签名/验签/篡改拒绝）");

        // 内置私钥推导公钥，必须与 KPM 内置的一致
        byte[] seed = Ed25519.unhex(
                "6e5f3fa9b6c7c5b13997303f59c8d37ed90f04e2207167fff1c918da61ef4d14");
        String want = "0cb425e5a1e8af285f27d187e030acfa3fdafeddd7229246a14505f61b7ebdeb";
        String got = Ed25519.hex(Ed25519.publicKey(seed));
        if (!got.equals(want)) {
            System.out.println("FAIL  内置私钥公钥不符: " + got);
            System.exit(1);
        }
        System.out.println("PASS  内置私钥 → 公钥 " + got + "（与 KPM 内置一致）");
    }
}
