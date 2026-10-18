#!/usr/bin/env bash
# adapt-new-device.sh —— 给一台新设备产出可加载的 cpuinfo_redirect.ko
#
# 只需要两样东西：目标内核的 uname -r，和它的 BTF（PROC 后端才需要）。
# 不需要 OEM 完整源码树，不需要 Module.symvers，不需要设备参与编译。
#
# 用法:
#   ./tools/adapt-new-device.sh <uname-r> [vmlinux.btf] [选项]
#
# 选项:
#   --out DIR        产物目录（默认 dist）
#   --tree DIR       内核源码树位置（默认 kernel-<主版本>，不存在会自动 clone）
#   --keep-crc       保留 Module.symvers（产出的 ko 带 CRC，只在同固件上可用）
#   --force-rvh      强制走 RVH 后端（默认自动判断）
#   --rvh-noheader   目标内核有 android_rvh_cpuinfo 符号但源码树无头文件时用
#                    （标准 GKI 6.12/6.18 常见；kallsyms 里能看到该符号就用这个）
#
# 示例:
#   ./tools/adapt-new-device.sh 6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k vmlinux.btf
#   ./tools/adapt-new-device.sh 5.10.245
set -euo pipefail

ARCH_MAKE=(ARCH=arm64 LLVM=1 LLVM_IAS=1)
OUTD_NAME="dist"
KTREE=""
KEEP_CRC=0
FORCE_RVH=0
RVH_NOHEADER=0
POS=()
POSN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUTD_NAME="$2"; shift 2 ;;
    --tree) KTREE="$2"; shift 2 ;;
    --keep-crc) KEEP_CRC=1; shift ;;
    --force-rvh) FORCE_RVH=1; shift ;;
    --rvh-noheader) RVH_NOHEADER=1; shift ;;
    -h|--help) sed -n '3,20p' "$0"; exit 0 ;;
    -*) echo "未知选项: $1" >&2; exit 1 ;;
    *) POS+=("$1"); POSN=$((POSN+1)); shift ;;
  esac
done

if [ "$POSN" -lt 1 ]; then
  echo "用法: $0 <uname-r> [vmlinux.btf] [--out DIR] [--tree DIR] [--keep-crc] [--force-rvh]" >&2
  exit 1
fi

KVER_FULL="${POS[0]}"
BTF="${POS[1]:-}"
# python3 在部分环境是转发到应用商店的 stub，必须真正执行一次才算可用
PY=""
for _c in python3 python; do
  if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "import sys; sys.exit(0)" >/dev/null 2>&1; then
    PY="$_c"; break
  fi
done
[ -n "$PY" ] || { echo "错误: 需要可用的 python3 或 python" >&2; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
OUTD="$SRC/$OUTD_NAME"

die() { echo "错误: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ---------- 1. 解析版本 ----------
KV="${KVER_FULL%%-*}"                        # 6.6.89
KMAJ="${KV%%.*}"                             # 6
KMIN="$(echo "$KV" | cut -d. -f2)"           # 6
KMIMIN="$KMAJ.$KMIN"                         # 6.6
LOCALVER="${KVER_FULL#"$KV"}"                # -android15-8-...

info "目标内核 : $KVER_FULL"
info "主版本   : $KMIMIN"
info "LOCALVER : ${LOCALVER:-<空>}"

case "$KMIMIN" in
  5.10) BRANCH=android12-5.10 ;;
  5.15) BRANCH=android13-5.15 ;;
  6.1)  BRANCH=android14-6.1 ;;
  6.6)  BRANCH=android15-6.6 ;;
  6.12) BRANCH=android16-6.12 ;;
  6.18) BRANCH=android17-6.18 ;;
  5.5|5.6|5.7|5.8|5.9) die "$KMIMIN 没有 Android GKI 分支（GKI 从 5.10 开始），无法通用适配" ;;
  *) die "不支持的版本 $KMIMIN（支持 5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18）" ;;
esac
info "GKI 分支 : $BRANCH"

# ---------- 2. 源码树 ----------
[ -n "$KTREE" ] || KTREE="$SRC/kernel-$KMIMIN"
if [ ! -f "$KTREE/Makefile" ]; then
  info "clone $BRANCH 到 $KTREE（浅克隆，约 300MB）"
  git clone --depth 1 -b "$BRANCH" https://android.googlesource.com/kernel/common "$KTREE" \
    || die "clone 失败（googlesource 不可达时改用 https://github.com/aosp-mirror/kernel_common）"
else
  info "复用源码树 $KTREE"
fi

# ---------- 3. 打补丁（五个已知坑） ----------
info "应用补丁"

# 3.1 elf_compat.h（sorttable / modpost 共用）
cat > "$KTREE/scripts/elf_compat.h" <<'EOF'
/* bionic linux/elf.h 与内核宏互递归，并缺 ARM 重定位常量。 */
#ifndef ELF_COMPAT_H
#define ELF_COMPAT_H
#undef ELF32_ST_BIND
#undef ELF32_ST_TYPE
#undef ELF64_ST_BIND
#undef ELF64_ST_TYPE
#define ELF32_ST_BIND(v) (((unsigned char)(v)) >> 4)
#define ELF32_ST_TYPE(v) ((v) & 0xf)
#define ELF64_ST_BIND(v) (((unsigned int)(v)) >> 4)
#define ELF64_ST_TYPE(v) ((v) & 0xf)
#endif
EOF
echo "   elf_compat.h"

# 3.2 sorttable.c 注入
if ! grep -q 'elf_compat.h' "$KTREE/scripts/sorttable.c" 2>/dev/null; then
  sed -i.bak 's|#include <elf.h>|#include <elf.h>\n#include "elf_compat.h"|' "$KTREE/scripts/sorttable.c"
  echo "   sorttable.c 注入 elf_compat"
fi

# 3.3 摘掉 asn1_compiler（bionic bits/wait.h 污染链）
if grep -q 'asn1_compiler' "$KTREE/scripts/Makefile" 2>/dev/null && \
   ! grep -q 'asn1_compiler dropped' "$KTREE/scripts/Makefile"; then
  sed -i.bak 's|^hostprogs-always-\$(CONFIG_ASN1).*asn1_compiler.*|# asn1_compiler dropped|' "$KTREE/scripts/Makefile"
  echo "   scripts/Makefile 摘 asn1_compiler"
fi

# 3.4 modpost.h：ELF_ST_* 展开 + ARM 重定位常量
if [ -f "$KTREE/scripts/mod/modpost.h" ] && ! grep -q 'R_ARM_MOVW_ABS_NC' "$KTREE/scripts/mod/modpost.h"; then
  "$PY" - "$KTREE/scripts/mod/modpost.h" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='replace').read()
rel = [('NONE',0),('PC24',1),('ABS32',2),('REL32',3),('PC13',4),('ABS16',5),('ABS12',6),
       ('THM_ABS5',7),('ABS8',8),('SBREL32',9),('THM_CALL',10),('THM_PC8',11),('SWI24',13),
       ('THM_SWI8',14),('XPC25',15),('THM_XPC22',16),('COPY',20),('GLOB_DAT',21),('JUMP_SLOT',22),
       ('RELATIVE',23),('GOTOFF32',24),('BASE_PREL',25),('GOT_BREL',26),('PLT32',27),('CALL',28),
       ('JUMP24',29),('THM_JUMP24',30),('TARGET1',38),('V4BX',40),('TARGET2',41),('PREL31',42),
       ('MOVW_ABS_NC',43),('MOVT_ABS',44),('MOVW_PREL_NC',45),('MOVT_PREL',46),
       ('THM_MOVW_ABS_NC',47),('THM_MOVT_ABS',48),('THM_MOVW_PREL_NC',49),('THM_MOVT_PREL',50),
       ('THM_JUMP19',51),('THM_JUMP6',52)]
blk = ['', '/* bionic: ELF64_ST_TYPE <-> ELF_ST_TYPE recursive; ARM relocs missing */',
       '#pragma clang diagnostic push',
       '#pragma clang diagnostic ignored "-Wmacro-redefined"',
       '#undef ELF_ST_BIND', '#undef ELF_ST_TYPE',
       '#define ELF_ST_BIND(x) (((unsigned int)(x)) >> 4)',
       '#define ELF_ST_TYPE(x) ((x) & 0xf)']
for n, v in rel:
    blk.append('#undef  R_ARM_%s' % n)
    blk.append('#define R_ARM_%s %d' % (n, v))
blk.append('#pragma clang diagnostic pop')
open(p, 'w', encoding='utf-8', newline='\n').write(s + '\n'.join(blk) + '\n')
PYEOF
  echo "   modpost.h 补 ELF 宏 + ARM 重定位"
fi

# 3.5 ld-version.sh shebang（5.10 用 /usr/bin/awk）
if [ -f "$KTREE/scripts/ld-version.sh" ] && head -1 "$KTREE/scripts/ld-version.sh" | grep -q '^#!/usr/bin/awk'; then
  sed -i.bak '1s|.*|#!/usr/bin/awk -f|' "$KTREE/scripts/ld-version.sh" 2>/dev/null || true
  echo "   ld-version.sh 记录（非 bionic 环境无需修改）"
fi

# ---------- 4. 后端判定 ----------
HAS_RVH=0
if [ -f "$KTREE/include/trace/hooks/cpuinfo.h" ] && grep -q 'android_rvh_cpuinfo' "$KTREE/include/trace/hooks/cpuinfo.h" 2>/dev/null; then
  HAS_RVH=1
fi
if [ "$RVH_NOHEADER" = 1 ]; then
  # 目标内核有 android_rvh_cpuinfo 符号，但源码树没有对应头文件。
  # 模块自己声明符号，直接注册到 tracepoint。
  BACKEND=rvh; KCFLAG="-DCR_RVH_NOHEADER=1"
  info "后端     : RVH（无头文件模式，靠目标内核导出符号）"
  info "           前提：目标 kallsyms 里必须有 android_rvh_cpuinfo_c_show"
elif [ "$FORCE_RVH" = 1 ] && [ "$HAS_RVH" = 0 ]; then
  die "--force-rvh 指定了 RVH，但 $BRANCH 没有 trace/hooks/cpuinfo.h（改用 --rvh-noheader）"
elif [ "$HAS_RVH" = 1 ] && [ "$FORCE_RVH" = 1 ]; then
  BACKEND=rvh; KCFLAG=""
  info "后端     : RVH（源码树含 cpuinfo hook）"
else
  BACKEND=proc; KCFLAG="-DCR_FORCE_PROC=1"
  info "后端     : proc_ops（源码树无 RVH hook）"
fi

# ---------- 5. 结构偏移 ----------
OFFSETS="$SRC/src/cr_offsets.h"
if [ "$BACKEND" = proc ]; then
  if [ -n "$BTF" ] && [ -f "$BTF" ]; then
    info "从 BTF 生成 cr_offsets.h"
    "$PY" "$SRC/tools/gen_offsets.py" --btf "$BTF" --out "$OFFSETS" \
      || die "gen_offsets.py 失败"
  else
    die "PROC 后端需要目标内核的 BTF。请从设备 dd /sys/kernel/btf/vmlinux 后作为第二个参数传入。"
  fi
else
  info "RVH 后端不需要结构偏移"
fi

# ---------- 6. 编译 ----------
BUILD="$SRC/build-$KMIMIN"
mkdir -p "$BUILD"
info "配置内核"
make -C "$KTREE" O="$BUILD" "${ARCH_MAKE[@]}" defconfig >/dev/null
make -C "$KTREE" O="$BUILD" "${ARCH_MAKE[@]}" modules_prepare >/dev/null
make -C "$KTREE" O="$BUILD" "${ARCH_MAKE[@]}" prepare >/dev/null

MAKEVARS=("${ARCH_MAKE[@]}" CONFIG_MODULE_SIG_ALL=n)
[ -n "$LOCALVER" ] && MAKEVARS+=("LOCALVERSION=$LOCALVER")
[ -n "$KCFLAG" ] && MAKEVARS+=("KCFLAGS=$KCFLAG")

SYMV="$BUILD/Module.symvers"
RESTORE=""
if [ "$KEEP_CRC" = 0 ] && [ -f "$SYMV" ]; then
  info "--no-crc：隐藏 Module.symvers（产出空 __versions）"
  mv "$SYMV" "$SYMV.bak.$$"
  RESTORE="$SYMV.bak.$$"
  MAKEVARS+=(KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n)
fi

info "编译模块"
set +e
make -C "$BUILD" M="$SRC/src" "${MAKEVARS[@]}" modules -j"$(nproc)" 2>&1 | tail -20
RC=${PIPESTATUS[0]}
set -e

# 6.1 modpost 在无 symvers 时会生成非法初始化 "= ;"，修补后重链
MODC="$SRC/src/cpuinfo_redirect.mod.c"
if [ -f "$MODC" ] && grep -q '__version_ext_names") =$' "$MODC"; then
  info "修补 .mod.c 空初始化后重链"
  "$PY" - "$MODC" <<'PYEOF'
import sys, re
p = sys.argv[1]
lines = open(p, encoding='utf-8', errors='replace').read().splitlines()
out = []
skip = False
for l in lines:
    if skip:
        skip = False
        continue
    if l.rstrip().endswith('__version_ext_names") ='):
        out.append(l)
        out.append('\t"";')
        skip = True
        continue
    out.append(l)
open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
PYEOF
  touch "$MODC"
  make -C "$BUILD" M="$SRC/src" "${MAKEVARS[@]}" modules -j"$(nproc)" 2>&1 | tail -10
  RC=0
fi

[ -n "$RESTORE" ] && [ -f "$RESTORE" ] && mv "$RESTORE" "$SYMV"

KO="$SRC/src/cpuinfo_redirect.ko"
[ -f "$KO" ] || die "编译失败，无产物（见上方输出）"

# ---------- 7. 校验 ----------
info "校验 vermagic"
VM="$(strings "$KO" | grep -m1 'vermagic=' | sed 's/^vermagic=//')"
echo "    产物 : $VM"
echo "    目标 : (前缀应为 $KVER_FULL)"
case "$VM" in
  "$KVER_FULL"*) echo "    ✓ vermagic 前缀匹配" ;;
  *) echo "    ✗ vermagic 不匹配 —— LOCALVERSION 可能未生效" ;;
esac

mkdir -p "$OUTD"
TARGET="$OUTD/cpuinfo_redirect-$KVER_FULL.ko"
cp -f "$KO" "$TARGET"
info "产物: $TARGET"

cat <<EOF

---- 部署 ----
adb push "$TARGET" /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/$(basename "$TARGET")'
adb shell 'grep -m1 "CPU implementer" /proc/cpuinfo'      # 0x48 = Kirin 生效，0x51 = 未生效
adb shell su -c 'dmesg | grep cpuinfo_redirect | tail -3'
EOF
