#!/usr/bin/env bash
# build-three-devices.sh —— 为三台待适配设备批量编译 .ko
#
# 需要：Linux（x86_64 即可，交叉编译 arm64）、make、clang、bc/flex/bison
#       sudo apt install -y build-essential bc flex bison libssl-dev libelf-dev \
#                         gcc-aarch64-linux-gnu python3
#
# 用法：
#   ./tools/build-three-devices.sh /path/to/extracted/packages
#
# 目录结构要求（就是采集包解开的形状）：
#   packages/p1/cr-target-info/{uname_r.txt,vmlinux.btf,...}
#   packages/p2/cr-target-info/...
#   packages/p3/cr-target-info/...
set -uo pipefail

PKGROOT="${1:-}"
[ -d "$PKGROOT" ] || { echo "用法: $0 <采集包解开的目录>" >&2; exit 1; }

SRC="$(cd "$(dirname "$0")/.." && pwd)"
ADAPT="$SRC/tools/adapt-new-device.sh"
[ -x "$ADAPT" ] || { echo "找不到 $ADAPT" >&2; exit 1; }

# 三台设备的编译策略
#   p1 6.1.177  PROC（无 RVH hook）
#   p2 6.1.90   PROC（无 RVH hook）
#   p3 6.12.38  RVH 无头文件模式（kallsyms 有 6 个 android_rvh_cpuinfo 符号）
declare -A STRATEGY=(
  [p1]="proc"
  [p2]="proc"
  [p3]="rvh-noheader"
)

# p3 走 RVH，不需要 BTF。如果没有采集包，把 uname -r 直接写进环境变量即可：
#   P3_UNAME=6.12.38-android16-5-g844001fb8721-ab14552068-4k ./tools/build-three-devices.sh ...

FAILED=()
OK=()

for key in p1 p2 p3; do
  d="$PKGROOT/$key/cr-target-info"
  # p3 允许没有采集包：RVH 模式只要 uname -r
  if [ ! -d "$d" ]; then
    if [ "$key" = "p3" ] && [ -n "${P3_UNAME:-}" ]; then
      d=""
    else
      echo "[跳过] $key：找不到 $d"; continue
    fi
  fi
  uv=$(cat "${d:-/dev/null}/uname_r.txt" 2>/dev/null | tr -d '\r\n')
  [ -n "$uv" ] || uv="${P3_UNAME:-}"
  btf="${d:-}/vmlinux.btf"
  btf="$d/vmlinux.btf"
  [ -n "$uv" ] || { echo "[跳过] $key：uname_r.txt 为空"; continue; }
  # RVH 后端不需要 BTF；PROC 后端必须有。没有 BTF 时把参数留空。
  [ -f "$btf" ] || btf=""

  echo "=============================================================="
  echo "### $key  $uv"
  echo "### 策略：${STRATEGY[$key]}"
  echo "=============================================================="

  case "${STRATEGY[$key]}" in
    rvh-noheader)
      # 这条路径不依赖 BTF（不碰 proc_dir_entry），只靠目标内核导出的 tracepoint
      if [ -n "$btf" ]; then
        "$ADAPT" "$uv" "$btf" --out "$SRC/dist" --rvh-noheader
      else
        "$ADAPT" "$uv" "" --out "$SRC/dist" --rvh-noheader
      fi
      ;;
    proc|*)
      "$ADAPT" "$uv" "$btf" --out "$SRC/dist"
      ;;
  esac

  if [ $? -eq 0 ]; then
    OK+=("$key")
    echo "[成功] $key"
  else
    FAILED+=("$key")
    echo "[失败] $key"
  fi
  echo
done

echo "=============================================================="
echo "汇总"
echo "=============================================================="
echo "成功：${OK[*]:-无}"
echo "失败：${FAILED[*]:-无}"
echo
echo "产物："
ls -1 "$SRC/dist"/*.ko 2>/dev/null | while read -r f; do
  echo "  $(basename "$f")  $(stat -c%s "$f" 2>/dev/null || echo '?') B"
done
