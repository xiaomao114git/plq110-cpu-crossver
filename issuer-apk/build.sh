#!/usr/bin/env bash
# 手工打包 APK：aapt2 + javac + d8 + zipalign + apksigner，不依赖 gradle。
# 界面全部代码搭建，不需要 res/ 与 assets/，所以 aapt2 只处理 manifest。
set -euo pipefail

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
PLAT="$(ls -d "$SDK"/platforms/android-* 2>/dev/null | sort -V | tail -1)"
AJ="$PLAT/android.jar"

echo "SDK        : $SDK"
echo "build-tools: $BT"
echo "platform   : $PLAT"

test -f "$AJ" || { echo "::error::找不到 android.jar"; exit 1; }
test -x "$BT/aapt2" || { echo "::error::找不到 aapt2"; exit 1; }

rm -rf build && mkdir -p build/obj build/dex

echo "--- 先验 Ed25519（错了就不打包）---"
mkdir -p build/selftest
javac -encoding UTF-8 -d build/selftest \
  java/com/plq110/issuer/Ed25519.java test/Ed25519SelfTest.java
java -cp build/selftest Ed25519SelfTest

echo "--- javac ---"
javac -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
  -classpath "$AJ" -d build/obj \
  java/com/plq110/issuer/*.java 2>&1 | grep -v 'bootstrap class path' || true
ls -l build/obj/com/plq110/issuer/

echo "--- d8 ---"
"$BT/d8" --lib "$AJ" --min-api 21 --output build/dex \
  build/obj/com/plq110/issuer/*.class
ls -l build/dex/

echo "--- aapt2 link ---"
"$BT/aapt2" link \
  -o build/unsigned.apk \
  -I "$AJ" \
  --manifest AndroidManifest.xml \
  --min-sdk-version 21 \
  --target-sdk-version 34 \
  --version-code 3 --version-name 3.0 \
  --auto-add-overlay

echo "--- 放入 classes.dex ---"
( cd build/dex && zip -q ../unsigned.apk classes.dex )

echo "--- zipalign ---"
"$BT/zipalign" -f -p 4 build/unsigned.apk build/aligned.apk

echo "--- 签名 ---"
if [ ! -f build/keystore.jks ]; then
  keytool -genkeypair -keystore build/keystore.jks \
    -storepass plq110issuer -keypass plq110issuer \
    -alias issuer -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=plq110-issuer, OU=local, O=local, C=CN" 2>/dev/null
fi
"$BT/apksigner" sign \
  --ks build/keystore.jks \
  --ks-pass pass:plq110issuer \
  --key-pass pass:plq110issuer \
  --min-sdk-version 21 \
  --out issuer-plq3.apk build/aligned.apk

echo "--- 校验 ---"
"$BT/apksigner" verify --print-certs issuer-plq3.apk | head -5
ls -l issuer-plq3.apk
echo "sha256: $(sha256sum issuer-plq3.apk | cut -d' ' -f1)"
echo "--- 权限（应只有 manifest 里声明的，且无 INTERNET）---"
"$BT/aapt2" dump permissions issuer-plq3.apk || true
