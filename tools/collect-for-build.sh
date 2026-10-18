#!/system/bin/sh
# collect-for-build.sh —— 设备侧采集，产出供 build-for-device.sh 使用的 kx.tgz
#
# 用法: su -c 'sh /sdcard/collect-for-build.sh'
# 产物: /sdcard/kx.tgz   （回传给编译方即可）
#
# 只读采集，唯一改动: 临时把 kptr_restrict 置 0 取真地址后立刻还原。
set -u

OUT=/sdcard/kx
LOG=$OUT/run.log

[ "$(id -u)" = "0" ] || { echo "需要 root"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT" || exit 1
: > "$LOG"
say() { echo "$*"; echo "$*" >> "$LOG"; }

say "=== 1/7 环境 ==="
for c in 'uname -a' 'uname -r' 'uname -m' 'cat /proc/version' 'cat /proc/cmdline' \
         'getconf PAGE_SIZE' 'getenforce'; do
  echo "--- $c" >> "$OUT/00_env.txt" 2>&1
  eval "$c" >> "$OUT/00_env.txt" 2>&1
done
for p in ro.product.device ro.product.model ro.product.board ro.board.platform \
         ro.boot.hardware ro.build.fingerprint ro.build.version.release \
         ro.build.version.sdk ro.kernel.version; do
  echo "$p=$(getprop $p)" >> "$OUT/00_env.txt" 2>&1
done
say "  00_env.txt"

say "=== 2/7 kallsyms（含 hook 点判定）==="
KP=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null)
( echo 0 > /proc/sys/kernel/kptr_restrict ) 2>/dev/null
cat /proc/kallsyms > "$OUT/kallsyms.txt" 2>/dev/null
( echo "$KP" > /proc/sys/kernel/kptr_restrict ) 2>/dev/null
[ -s "$OUT/kallsyms.txt" ] || cat /proc/kallsyms > "$OUT/kallsyms.txt" 2>/dev/null
{
  echo "kptr_restrict 原值 = $KP"
  echo "--- 关键 hook 符号"
  grep -E " (android_rvh_probe_register|android_vh_probe_register|register_kprobe|kallsyms_lookup_name|proc_create|seq_read)$" "$OUT/kallsyms.txt"
  echo "--- 含 rvh 的符号（前 40）"
  grep -i rvh "$OUT/kallsyms.txt" | head -40
  echo "--- 含 cpuinfo 的符号"
  grep -i cpuinfo "$OUT/kallsyms.txt"
} > "$OUT/01_syms.txt" 2>&1
say "  01_syms.txt / kallsyms.txt"

say "=== 3/7 kernel config ==="
if [ -r /proc/config.gz ]; then
  cp /proc/config.gz "$OUT/config.gz" 2>/dev/null
  zcat /proc/config.gz > "$OUT/kernel_config.txt" 2>/dev/null
  grep -E "^CONFIG_(MODVERSIONS|MODULES|MODULE_SIG|MODULE_SIG_FORCE|MODULE_UNLOAD|KALLSYMS|KALLSYMS_ALL|KPROBES|DEBUG_INFO_BTF|ARM64_4K_PAGES|ARM64_16K_PAGES|ARM64_64K_PAGES|ANDROID_VENDOR_HOOKS|VENDOR_HOOK|SECURITY_SELINUX)" \
    "$OUT/kernel_config.txt" > "$OUT/03_config_key.txt" 2>&1
say "  03_config_key.txt"
else
say "  无 /proc/config.gz"
fi

say "=== 4/7 厂商 ko（CRC 来源）==="
mkdir -p "$OUT/ko"
N=0; HIT=0
for d in /vendor/lib/modules /vendor_dlkm/lib/modules /odm/lib/modules /system/lib/modules; do
  [ -d "$d" ] || continue
  for f in "$d"/*.ko; do
    [ -f "$f" ] || continue
    N=$((N+1))
    if [ "$HIT" -lt 10 ]; then
      if grep -qa "register_kprobe\|kallsyms_lookup_name\|proc_create" "$f" 2>/dev/null; then
        cp "$f" "$OUT/ko/" 2>/dev/null && HIT=$((HIT+1))
      fi
    fi
  done
done
say "  扫到 $N 个 ko，收 $HIT 个"

say "=== 5/7 BTF（偏移来源，关键）==="
if [ -r /sys/kernel/btf/vmlinux ]; then
  dd if=/sys/kernel/btf/vmlinux of="$OUT/vmlinux.btf" bs=4096 2>>"$LOG"
  SZ=$(wc -c < "$OUT/vmlinux.btf" 2>/dev/null)
say "  vmlinux.btf = $SZ 字节"
else
say "  无 BTF（内核未开 CONFIG_DEBUG_INFO_BTF）"
fi

say "=== 6/7 boot 分区（备用 CRC 来源）==="
BY=""
for c in /dev/block/by-name /dev/block/bootdevice/by-name; do
  [ -d "$c" ] && BY=$c && break
done
SLOT=$(getprop ro.boot.slot_suffix)
{
  echo "by-name = ${BY:-未找到}"
  echo "slot = ${SLOT:-无}"
  [ -n "$BY" ] && ls -l "$BY" 2>&1
} > "$OUT/02_boot.txt" 2>&1
say "  02_boot.txt"

say "=== 7/7 打包 ==="
ls -l "$OUT" > "$OUT/04_files.txt" 2>&1
cd /sdcard && tar -czf /sdcard/kx.tgz kx 2>>"$LOG"
SZ=$(wc -c < /sdcard/kx.tgz 2>/dev/null)
say "  /sdcard/kx.tgz = $SZ 字节"
say "=== 完成：把 kx.tgz 回传给编译方 ==="
