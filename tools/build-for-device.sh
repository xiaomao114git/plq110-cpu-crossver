#!/usr/bin/env bash
# build-for-device.sh —— 为指定设备+内核编译 cpuinfo_redirect.ko
#
# 两种构建路径:
#   bazel  (默认, Android13+ / 5.10-6.18)  GKI 官方构建系统
#   legacy (5.10-Android12 或没装 bazel 时) make defconfig + modules_prepare
#
# 用法:
#   ./build-for-device.sh kx.tgz                 # 自动判断
#   ./build-for-device.sh kx.tgz --no-crc        # 空 __versions，绕开 Module.symvers
#   ./build-for-device.sh kx.tgz --legacy        # 强制走 make
#
# 关于 --no-crc:
#   CONFIG_MODVERSIONS=y 时，带 CRC 的符号必须与内核一致，否则拒绝加载。
#   但『没有 CRC 的符号』内核只报 no symbol version 警告、不拒绝。
#   因此隐藏 Module.symvers + KBUILD_MODPOST_WARN=1 可以产出 __versions 为空的模块，
#   加载门槛降到只剩 vermagic。代价是 ABI 不匹配不会在加载期暴露，
#   所以必须确保源码与目标内核 API 兼容。
set -euo pipefail

KX=""
USE_BAZEL=1
NO_CRC=0
for a in "$@"; do
  case "$a" in
    --no-crc) NO_CRC=1 ;;
    --legacy) USE_BAZEL=0 ;;
    --gki-branch) shift ;;
    --*) echo "未知参数 $a"; exit 1 ;;
    *) [ -z "$KX" ] && KX="$a" ;;
  esac
done
[ -n "$KX" ] || { echo "用法: $0 <kx.tgz> [--no-crc] [--legacy]"; exit 1; }
[ -f "$KX" ] || { echo "找不到 $KX"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar xzf "$KX" -C "$WORK"
ROOT=$(find "$WORK" -maxdepth 1 -mindepth 1 -type d | head -1)

echo "==> 解析目标内核"
UNAME_R=$(grep -m1 "^uname -r" "$ROOT/00_env.txt" 2>/dev/null | sed "s/^uname -r[ \t]*//" || true)
[ -n "$UNAME_R" ] || { echo "采集包缺 uname -r"; exit 1; }
KV=${UNAME_R%%-*}
KMAJ=${KV%%.*}
KMIN=$(echo "$KV" | cut -d. -f2)
LOCALVER=${UNAME_R#"$KV"}
echo "    内核: $UNAME_R"
echo "    LOCALVERSION: ${LOCALVER:-<空>}"

PS=4k
grep -q "16K_PAGES=y" "$ROOT/03_config_key.txt" 2>/dev/null && PS=16k
grep -q "64K_PAGES=y" "$ROOT/03_config_key.txt" 2>/dev/null && PS=64k
MODV=n
grep -q "^CONFIG_MODVERSIONS=y" "$ROOT/03_config_key.txt" 2>/dev/null && MODV=y
HAVE_RVH=n
grep -q "android_rvh_cpuinfo" "$ROOT/01_syms.txt" 2>/dev/null && HAVE_RVH=y
echo "    页大小=$PS  modversions=$MODV  RVH=$HAVE_RVH  no-crc=$NO_CRC"

case "$KMAJ.$KMIN" in
  5.10) BRANCH="${GKI_BRANCH:-android12-5.10}" ;;
  5.15) BRANCH="${GKI_BRANCH:-android13-5.15}" ;;
  6.1)  BRANCH="${GKI_BRANCH:-android14-6.1}" ;;
  6.6)  BRANCH="${GKI_BRANCH:-android15-6.6}" ;;
  6.12) BRANCH="${GKI_BRANCH:-android16-6.12}" ;;
  *) echo "未知内核 $KMAJ.$KMIN"; exit 1 ;;
esac
echo "    GKI 分支: $BRANCH"

KTREE=${KTREE:-"$PWD/kernel-$KMAJ.$KMIN"}
if [ ! -f "$KTREE/Makefile" ]; then
    echo "==> clone $BRANCH（浅克隆）"
    git clone --depth 1 -b "$BRANCH" https://android.googlesource.com/kernel/common "$KTREE"
fi

MAKEVARS=(ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CONFIG_MODULE_SIG_ALL=n)
[ -n "$LOCALVER" ] && MAKEVARS+=("LOCALVERSION=$LOCALVER")
[ "$HAVE_RVH" = n ] && MAKEVARS+=("KCFLAGS=-DCR_FORCE_PROC=1")

if [ "$USE_BAZEL" = 1 ] && [ -x "$KTREE/tools/bazel" ]; then
    echo "==> Bazel 准备内核环境（首次较慢，会编译整套内核）"
    ( cd "$KTREE" && tools/bazel build //common:kernel_aarch64 //common:kernel_aarch64_modules_prepare )
    BAZEL_OUT=$(readlink -f "$KTREE/bazel-bin/common/kernel_aarch64")
    [ -d "$BAZEL_OUT" ] || { echo "Bazel 输出目录不存在"; exit 1; }
    # modules_prepare 的产物是 tar.gz，需解压到输出目录
    PREP_TGZ="$KTREE/bazel-bin/common/kernel_aarch64_modules_prepare/modules_prepare_outdir.tar.gz"
    if [ -f "$PREP_TGZ" ]; then
        echo "==> 解压 modules_prepare 环境"
        tar -xzf "$PREP_TGZ" -C "$BAZEL_OUT"
    fi
    OUTD="$BAZEL_OUT"
    KSRC="$KTREE/common"
else
    echo "==> Legacy: make defconfig + modules_prepare"
    OUTD=${OUTD:-"$PWD/out-$KMAJ.$KMIN"}
    make -C "$KTREE" O="$OUTD" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
    make -C "$KTREE" O="$OUTD" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
    KSRC="$KTREE"
fi

# --- 无 CRC 构建: 隐藏 Module.symvers ---
RESTORE=""
if [ "$NO_CRC" = 1 ] && [ "$MODV" = y ]; then
    SYM="$OUTD/Module.symvers"
    if [ -f "$SYM" ]; then
        mv "$SYM" "$SYM.no_crc_bak.$$"
        RESTORE="$SYM.no_crc_bak.$$"
        echo "==> 已隐藏 Module.symvers（空 __versions 构建）"
    fi
    MAKEVARS+=(KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n)
fi

echo "==> 生成偏移表"
if [ -f "$ROOT/vmlinux.btf" ]; then
    # 首选：设备回传的 BTF（与运行内核完全一致）
    python3 ./tools/gen_offsets.py --btf "$ROOT/vmlinux.btf" --out ./src/cr_offsets.h
elif [ "$HAVE_RVH" = n ] && [ -f "$OUTD/vmlinux" ]; then
    # 备选：从编译出的 vmlinux 抽 .BTF 段
    if objcopy -O binary --only-section=.BTF "$OUTD/vmlinux" btf.bin 2>/dev/null && [ -s btf.bin ]; then
        python3 ./tools/gen_offsets.py --btf btf.bin --out ./src/cr_offsets.h
    else
        echo "    [!] vmlinux 无 .BTF 段，proc_ops 后端会因为缺偏移而编译失败"
    fi
elif [ "$HAVE_RVH" = y ]; then
    echo "    RVH 后端，不需要偏移"
else
    echo "    [!] 无 BTF 且无 RVH：proc_ops 后端会因 static_assert 编译失败"
    echo "        请在设备上 dd /sys/kernel/btf/vmlinux 后重试"
fi

echo "==> 编译模块"
set +e
make -C "$KSRC" O="$OUTD" M="$PWD/src" "${MAKEVARS[@]}" modules -j"$(nproc)"
RC=$?
set -e
[ -n "$RESTORE" ] && [ -f "$RESTORE" ] && mv "$RESTORE" "${RESTORE%.no_crc_bak.*}" && echo "    已恢复 Module.symvers"
[ $RC -eq 0 ] || { echo "编译失败"; exit 1; }

KO="$PWD/src/cpuinfo_redirect.ko"
[ -f "$KO" ] || { echo "无产物"; exit 1; }

echo "==> 校验"
echo "    产物 $(strings "$KO" | grep -m1 vermagic=)"
VC=$(strings "$KO" | grep -c "no symbol version" || true)
echo "    目标 vermagic 主串: $UNAME_R"
strings "$KO" | grep -q "$UNAME_R" && echo "    vermagic 匹配 ✓" || echo "    vermagic 不匹配 ✗"
mkdir -p dist
cp -f "$KO" "dist/cpuinfo_redirect-$UNAME_R${NO_CRC:+-nocrc}.ko"
echo "==> 完成"
