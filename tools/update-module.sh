#!/usr/bin/env bash
# update-module.sh —— 一条命令产出「针对目标设备」的可刷模块包
#
# 输入：目标内核的 uname -r、以及它的 BTF。
# 输出：一个 zip 模块，内含刚刚为这台设备编译好的 .ko + 精确匹配元数据。
#
# 用法:
#   ./tools/update-module.sh <uname-r> <vmlinux.btf> [选项]
#
# 选项:
#   --out DIR        输出目录（默认 dist）
#   --template DIR   provider 模块模板目录（默认 ../cpuinfo-redirect-driver）
#   --version VER    覆盖版本号（默认用 UTC 时间戳）
#   --keep-all       保留模板 payload 里的其它版本 .ko（默认只留本次目标的那一个）
#   --keep-crc       保留 CRC（默认 --no-crc）
#
# 示例:
#   ./tools/update-module.sh 6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k vmlinux.btf
set -euo pipefail

# Git Bash / MSYS 会把形如 /data/local/tmp/x 的参数自动改写成 Windows 路径，
# 而这是 Android 设备端路径，必须原样传给 adb。这里关掉参数转换。
if [ -n "${MSYSTEM:-}" ]; then
  export MSYS2_ARG_CONV_EXCL="*"
fi

# python3 在部分环境是转发到应用商店的 stub，必须真正执行一次才算可用
PY=""
for _c in python3 python; do
  if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "import sys; sys.exit(0)" >/dev/null 2>&1; then
    PY="$_c"; break
  fi
done
[ -n "$PY" ] || { echo "错误: 需要可用的 python3 或 python" >&2; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
ADAPT="$SRC/tools/adapt-new-device.sh"
OUTD="$SRC/dist"
TPL="$SRC/../cpuinfo-redirect-driver"
VER=""

# 这两个必须在使用前定义（自动采集段会用到）
die() { echo "错误: $*" >&2; exit 1; }
info() { echo "==> $*"; }

KEEP_ALL=0
KEEP_CRC_ARG=""
AUTO=0
FORCE_AUTO=0
DRY_RUN=0
POS=()
POSN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUTD="$2"; shift 2 ;;
    --template) TPL="$2"; shift 2 ;;
    --version) VER="$2"; shift 2 ;;
    --keep-all) KEEP_ALL=1; shift ;;
    --keep-crc) KEEP_CRC_ARG="--keep-crc"; shift ;;
    --auto) FORCE_AUTO=1; shift ;;
    --manual) AUTO=-1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) sed -n '3,22p' "$0"; exit 0 ;;
    -*) echo "未知选项: $1" >&2; exit 1 ;;
    *) POS+=("$1"); POSN=$((POSN+1)); shift ;;
  esac
done

# 模式判定：默认全自动。给了两个位置参数走手动，给一个参数报错。
if [ "$AUTO" = -1 ]; then
  [ "$POSN" -ge 2 ] || { echo "错误: --manual 需要 <uname-r> 与 <vmlinux.btf> 两个参数" >&2; exit 1; }
  AUTO=0
elif [ "$POSN" -ge 2 ]; then
  AUTO=0
elif [ "$POSN" -eq 0 ]; then
  AUTO=1
else
  echo "错误: 只给了一个参数。要么给两个（uname-r + BTF），要么一个都不给走自动模式。" >&2
  exit 1
fi

if [ "$AUTO" = 1 ]; then
  ADB="${ADB:-adb}"
  command -v "$ADB" >/dev/null 2>&1 || die "找不到 adb（用 ADB=/path/to/adb 指定）"
  info "[auto] 从设备采集"
  KVER_FULL="$("$ADB" shell uname -r 2>/dev/null | tr -d "\r\n")"
  [ -n "$KVER_FULL" ] || die "adb 读取 uname -r 失败（设备未连接或未授权）"
  info "      uname -r = $KVER_FULL"

  AUTO_BTF="$(mktemp -t vmlinux.XXXXXX.btf)"
  # adb 是宿主机原生程序（Windows 下是 adb.exe），不认识 MSYS 的 /tmp/... 路径，
  # 传参前必须转成它认识的绝对路径。Linux 上 cygpath 不存在，直接用原路径。
  if command -v cygpath >/dev/null 2>&1; then
    AUTO_BTF_XFER="$(cygpath -w "$AUTO_BTF")"
  else
    AUTO_BTF_XFER="$AUTO_BTF"
  fi
  # 注意引号层次：adb shell 收一个整体命令串，su -c 在里面再收一层
  "$ADB" shell "su -c 'dd if=/sys/kernel/btf/vmlinux of=/data/local/tmp/.cr_vmlinux.btf'" >/dev/null 2>&1 || true
  "$ADB" pull /data/local/tmp/.cr_vmlinux.btf "$AUTO_BTF_XFER" >/dev/null 2>&1 || true
  "$ADB" shell "su -c 'rm -f /data/local/tmp/.cr_vmlinux.btf'" >/dev/null 2>&1 || true
  if [ -s "$AUTO_BTF" ]; then
    BTF="$AUTO_BTF"
    info "      BTF = $(stat -c%s "$BTF" 2>/dev/null || wc -c < "$BTF") B"
  else
    BTF=""
    info "      BTF = 不可用（若目标内核有 RVH hook 仍可继续）"
  fi

  if [ "$DRY_RUN" = 1 ]; then
    KV="${KVER_FULL%%-*}"
    KMIMIN="$(echo "$KV" | cut -d. -f1,2)"
    echo
    echo "================ dry-run ================"
    echo "  目标内核 : $KVER_FULL"
    echo "  主版本   : $KMIMIN"
    echo "  LOCALVER : ${KVER_FULL#"$KV"}"
    echo "  BTF      : ${BTF:-<不可用>}"
    echo "  模板     : $TPL"
    echo "  输出     : $OUTD"
    echo "  将执行   : adapt-new-device.sh $KVER_FULL ${BTF:-<无>} --out $OUTD"
    echo "========================================"
    exit 0
  fi
else
  KVER_FULL="${POS[0]}"
  BTF="${POS[1]}"
fi


[ -x "$ADAPT" ] || die "找不到 $ADAPT"
if [ -n "$BTF" ]; then
  [ -f "$BTF" ] || die "BTF 文件不存在: $BTF"
fi
[ -d "$TPL" ] || die "模块模板目录不存在: $TPL（用 --template 指定）"
[ -f "$TPL/module.prop" ] || die "$TPL 不是模块目录（缺 module.prop）"

# ---------- 1. 编译目标设备的 .ko ----------
info "[1/5] 编译 $KVER_FULL 的模块"
mkdir -p "$OUTD"
"$ADAPT" "$KVER_FULL" "$BTF" --out "$OUTD" $KEEP_CRC_ARG \
  || die "编译失败"

KO="$OUTD/cpuinfo_redirect-$KVER_FULL.ko"
[ -f "$KO" ] || die "预期产物不存在: $KO"
KO_SHA="$(sha256sum "$KO" | cut -d' ' -f1)"
info "    产物 $(basename "$KO")  $KO_SHA"

# ---------- 2. 组装模块目录 ----------
info "[2/5] 组装模块"
[ -n "$VER" ] || VER="20261018.$(date -u +%H%M%S)"
STAGE="$OUTD/.stage-$$"
rm -rf "$STAGE"
mkdir -p "$STAGE/payload" "$STAGE/targets" "$STAGE/tools"

# 2.1 模板里的脚本与配置
for f in post-fs-data.sh service.sh uninstall.sh customize.sh README.md; do
  [ -f "$TPL/$f" ] && cp -f "$TPL/$f" "$STAGE/$f"
done
[ -d "$TPL/META-INF" ] && cp -rf "$TPL/META-INF" "$STAGE/META-INF"
[ -f "$TPL/tools/make-target-env.sh" ] && cp -f "$TPL/tools/make-target-env.sh" "$STAGE/tools/"

# 2.2 payload
if [ "$KEEP_ALL" = 1 ]; then
  cp -f "$TPL"/payload/*.ko "$STAGE/payload/" 2>/dev/null || true
fi
cp -f "$KO" "$STAGE/payload/cpuinfo_redirect-$(echo "$KVER_FULL" | cut -d. -f1,2).ko"

# 2.3 module.prop（保留模板其余字段，覆盖版本与描述）
sed -e "s|^version=.*|version=$VER|" \
    -e "s|^versionCode=.*|versionCode=$(date -u +%Y%m%d%H)|" \
    -e "s|^description=.*|description=Provider for $KVER_FULL. Verified build, sha256 ${KO_SHA:0:16}. Never insmods; a loader picks state/ko.path.|" \
    "$TPL/module.prop" > "$STAGE/module.prop"

# ---------- 3. 生成精确匹配元数据 ----------
info "[3/5] 生成 targets/<sha256>.env"
PAGE_SIZE="${PAGE_SIZE:-4096}"
BACKEND="${BACKEND_OVERRIDE:-proc}"
# 从 cr_offsets.h 是否被 adapt 脚本重写过无法判断后端，这里按 .ko 内是否含 RVH 符号粗判
if strings "$KO" | grep -q 'register_trace_android_rvh_cpuinfo' 2>/dev/null; then
  BACKEND=rvh
fi

cat > "$STAGE/targets/$KO_SHA.env" <<EOF
schema=2
protocol=3
backend=$BACKEND
release=$KVER_FULL
page_size=$PAGE_SIZE
config_sha256=-
btf_sha256=$(sha256sum "$BTF" | cut -d' ' -f1)
ko_file=cpuinfo_redirect-$(echo "$KVER_FULL" | cut -d. -f1,2).ko
ko_sha256=$KO_SHA
EOF
info "    backend=$BACKEND  page=$PAGE_SIZE"

# ---------- 4. 打包 ----------
info "[4/5] 打包 zip"
SAFE_VER="$(echo "$KVER_FULL" | tr -c 'A-Za-z0-9._-' '_')"
ZIP="$OUTD/cpuinfo-redirect-provider-$SAFE_VER.zip"
rm -f "$ZIP"

"$PY" - "$STAGE" "$ZIP" <<'PYEOF'
import os, sys, zipfile, stat
stage, zip_path = sys.argv[1], sys.argv[2]
EXEC = {'post-fs-data.sh','service.sh','uninstall.sh','customize.sh',
        'tools/make-target-env.sh','META-INF/com/google/android/update-binary'}
items = []
for root, dirs, files in os.walk(stage):
    for f in files:
        full = os.path.join(root, f)
        items.append((full, os.path.relpath(full, stage).replace(os.sep,'/')))
items.sort(key=lambda x: x[1])
with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
    for full, rel in items:
        zi = zipfile.ZipInfo.from_file(full, rel)
        zi.external_attr = ((0o755 if rel in EXEC else 0o644) | stat.S_IFREG) << 16
        zi.compress_type = zipfile.ZIP_DEFLATED
        with open(full,'rb') as fh: z.writestr(zi, fh.read())
print('packed %d entries' % len(items))
PYEOF

# ---------- 5. 校验 ----------
info "[5/5] 校验"
rm -rf "$STAGE"
[ -f "$ZIP" ] || die "打包失败"
ZIP_SHA="$(sha256sum "$ZIP" | cut -d' ' -f1)"

echo
echo "================ 完成 ================"
echo "  模块包 : $ZIP"
echo "  大小   : $(stat -c%s "$ZIP" 2>/dev/null || echo '?') B"
echo "  sha256 : $ZIP_SHA"
echo "  目标   : $KVER_FULL"
echo "  产物   : payload/cpuinfo_redirect-$(echo "$KVER_FULL" | cut -d. -f1,2).ko"
echo "  元数据 : targets/$KO_SHA.env"
echo "======================================"
echo
echo "部署："
echo "  1. 在 KernelSU / Magisk 里直接刷入上面的 zip"
echo "  2. 或手动：adb push \"$ZIP\" /sdcard/ && 从管理器安装"
echo "  3. 装完看状态：cat /data/adb/modules/cpuinfo-redirect-driver/state/status"
echo "     state=exact-match 表示六因子精确命中"
echo "     runtime_test=not-run 是固定值——本模块从不加载"
