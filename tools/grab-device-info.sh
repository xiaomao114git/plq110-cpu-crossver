#!/system/bin/sh
# grab-device-info.sh —— 采集编译所需的全部信息，打成一个小包
#
# 用法（设备上）：
#   su -c 'sh /sdcard/grab-device-info.sh'
#
# 产物：/sdcard/cr-device-info.tgz
#   里面只有三样东西：
#     uname_r.txt     内核版本串（决定 vermagic 与 GKI 分支）
#     vmlinux.btf     内核 BTF（决定 proc_dir_entry 结构偏移）
#     config.gz       /proc/config.gz（有就带，用于判断 MODVERSIONS/RANDSTRUCT）
#
# 就这些。不需要源码树、不需要 Module.symvers、不需要厂商固件。
set -u

OUT=/sdcard/cr-device-info
PKG=/sdcard/cr-device-info.tgz

[ "$(id -u)" = "0" ] || { echo "需要 root：su -c 'sh $0'"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT" || { echo "无法写 /sdcard"; exit 1; }

# 1. 内核版本
uname -r > "$OUT/uname_r.txt"
echo "[1/3] uname -r = $(cat "$OUT/uname_r.txt")"

# 2. BTF
if [ -r /sys/kernel/btf/vmlinux ]; then
  dd if=/sys/kernel/btf/vmlinux of="$OUT/vmlinux.btf" 2>/dev/null
  SZ=$(wc -c < "$OUT/vmlinux.btf")
  echo "[2/3] BTF = $SZ 字节"
  [ "$SZ" -gt 1024 ] || { echo "  BTF 太小，可能读取失败"; exit 1; }
else
  echo "[2/3] 没有 /sys/kernel/btf/vmlinux —— 内核未开 CONFIG_DEBUG_INFO_BTF"
  echo "      如果目标内核有 RVH cpuinfo hook 仍可编译（不需要 BTF）"
fi

# 3. config
if [ -r /proc/config.gz ]; then
  cp /proc/config.gz "$OUT/config.gz"
  echo "[3/3] config.gz = $(wc -c < "$OUT/config.gz") 字节"
else
  echo "[3/3] 没有 /proc/config.gz（不影响编译）"
fi

# 附加：几个有用的判断信息，纯文本，便于排查
{
  echo "release=$(uname -r)"
  echo "page_size=$(getconf PAGE_SIZE 2>/dev/null)"
  echo "arch=$(uname -m)"
  echo "btf=$( [ -f "$OUT/vmlinux.btf" ] && echo yes || echo no )"
  echo "config=$( [ -f "$OUT/config.gz" ] && echo yes || echo no )"
  echo "selinux=$(getenforce 2>/dev/null)"
  echo "model=$(getprop ro.product.model)"
  echo "soc=$(getprop ro.soc.model)"
} > "$OUT/info.txt"

cd /sdcard && tar czf "$PKG" cr-device-info 2>/dev/null
rm -rf "$OUT"

if [ -s "$PKG" ]; then
  echo ""
  echo "=== 完成 ==="
  echo "包: $PKG  ($(wc -c < "$PKG") 字节)"
  echo ""
  echo "取回电脑："
  echo "  adb pull $PKG"
  echo ""
  echo "Linux 上编译："
  echo "  tar xzf cr-device-info.tgz"
  echo "  ./tools/adapt-new-device.sh \"\$(cat cr-device-info/uname_r.txt)\" cr-device-info/vmlinux.btf"
else
  echo "打包失败"
  exit 1
fi
