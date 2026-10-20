#!/usr/bin/env bash
# 手工打包 APK：aapt2 + javac + d8 + zipalign + apksigner，不依赖 gradle。
# 项目只有一个 Activity 和一个 assets 页面，用 gradle 纯属负担。
set -euo pipefail

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/usr/local/lib/android/sdk}}"
BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
PLAT="$(ls -d "$SDK"/platforms/android-* 2>/dev/null | sort -V | tail -1)"
AJ="$PLAT/android.jar"

echo "SDK      : $SDK"
echo "build-tools: $BT"
echo "platform : $PLAT"

test -f "$AJ" || { echo "::error::找不到 android.jar"; exit 1; }
test -x "$BT/aapt2" || { echo "::error::找不到 aapt2"; exit 1; }

rm -rf build && mkdir -p build/obj build/dex

echo "--- javac ---"
javac -source 8 -target 8 -encoding UTF-8 \
  -classpath "$AJ" -d build/obj \
  java/com/plq110/issuer/MainActivity.java 2>&1 | grep -v 'bootstrap class path' || true
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
  -A assets \
  --min-sdk-version 21 \
  --target-sdk-version 34 \
  --version-code 1 --version-name 1.0 \
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
  --out issuer.apk build/aligned.apk

echo "--- 校验 ---"
"$BT/apksigner" verify --print-certs issuer.apk | head -5
ls -l issuer.apk
echo "sha256: $(sha256sum issuer.apk | cut -d' ' -f1)"
unzip -l issuer.apk | head -20
