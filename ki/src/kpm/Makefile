# ki_guard.kpm 构建
#
# 与 .ko 的根本区别：不需要内核源码树、不需要设备 /proc/config.gz、
# 不需要 vmlinux、不需要 pahole。只要 clang 和 KernelPatch 的头文件。
#
#   git clone --depth 1 --filter=blob:none --sparse \
#       https://github.com/bmax121/KernelPatch.git ../KernelPatch
#   (cd ../KernelPatch && git sparse-checkout set kernel)
#   make
#
# 想用 NDK 就加 NDK_PATH=/path/to/ndk；不设则用系统 clang 直接打
# aarch64 target，KPM 是内核代码不链 libc，不需要 sysroot。

KP_DIR ?= ../KernelPatch

ifdef NDK_PATH
  TOOLCHAIN := $(NDK_PATH)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-
  CC := $(TOOLCHAIN)clang
  STRIP := $(TOOLCHAIN)llvm-strip
  OBJCOPY := $(TOOLCHAIN)llvm-objcopy
else
  CC := clang --target=aarch64-linux-gnu
  STRIP ?= llvm-strip
  OBJCOPY ?= llvm-objcopy
endif

INCLUDE_DIRS := include patch/include linux/include \
                linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

# 与 KPM-Build-Anywhere 一致的选项：位置无关关掉、unwind 关掉、栈保护关掉，
# 因为这个二进制将来是跑在内核里的，不是用户态 ELF。
CFLAGS := $(INCLUDE_FLAGS) -O2 -Wall \
          -fno-PIC -fno-asynchronous-unwind-tables -fno-stack-protector \
          -fno-unwind-tables -fno-semantic-interposition -U_FORTIFY_SOURCE \
          -fno-common -fvisibility=hidden \
          -mgeneral-regs-only -ffixed-x18 -ffreestanding -fno-builtin

# RELEASE=1（默认）时把 KI_LOG/KI_ERR 的格式串整体编译掉。
# 那些串写着授权与 hook 的细节，发布版不该带。开发调试用 make RELEASE=0。
RELEASE ?= 1
ifeq ($(RELEASE),1)
  CFLAGS += -DKI_RELEASE
endif

# Ed25519 验签（来自 orlp/ed25519，无 libc、无 SIMD）。
# 只编验证路径：verify/sc/fe/ge/sha512。签名用不到，故不含 sign.c/keypair.c。
# precomp_data.h 是 ge_scalarmult_base 的预计算表，链接时若无人引用会被丢弃，
# 但源码保留能让 ge.c 不必裁剪——裁剪过的密码学代码风险太高。
ED_DIR := ed25519
ED_SRCS := $(ED_DIR)/verify.c $(ED_DIR)/fe.c $(ED_DIR)/ge.c $(ED_DIR)/sc.c $(ED_DIR)/sha512.c
ED_OBJS := $(ED_SRCS:.c=.o)

OBJS := ki_guard_kpm.o $(ED_OBJS)

# 产物名刻意不用 ki_guard：这个文件在 /data/adb 下，厂商内核态检测读得到。
# 名字取成骁龙显示栈里常见的样子，融进背景。
MOD := msm_drm_notify.kpm

all: $(MOD)

# -fuse-ld=lld 是必须的：系统 clang 默认调 /usr/bin/ld（x86_64 的 GNU ld），
# 它不认识 aarch64 emulation，会报 "unrecognised emulation mode: aarch64linux"。
# NDK 的 clang 自带这个默认值，系统 clang 没有。
#
# -r 保留重定位，KernelPatch 运行时自己做 relo
#
# --localize-hidden 这一步很关键：我们用了 -fvisibility=hidden，但 ld -r 会把
# 跨编译单元的符号留成 GLOBAL（哪怕它们实际是 HIDDEN 的），于是 ed25519 的
# fe_mul / ge_frombytes_negate_vartime 这些名字就躺在 .symtab 里，
# 等于告诉对方「这里用了标准 Ed25519」。先本地化再 strip 就能清掉。
$(MOD): $(OBJS)
	$(CC) -fuse-ld=lld -r -o $@ $^
	$(OBJCOPY) --localize-hidden $@
	$(STRIP) -g --strip-unneeded --strip-debug \
		--remove-section=.comment --remove-section=.note.GNU-stack $@

ki_guard_kpm.o: ki_guard_kpm.c ki_guard_kpm.lds ki_obf_gen.h Makefile
	$(CC) $(CFLAGS) -T ki_guard_kpm.lds -c -o $@ $<

# ed25519 源文件用同一套 CFLAGS，但不需要链接脚本（-T 只对最终布局有意义）。
# -I$(ED_DIR) 让内部 "#include \"fe.h\"" 这类相对引用能找到。
$(ED_DIR)/%.o: $(ED_DIR)/%.c
	$(CC) $(CFLAGS) -I$(ED_DIR) -c -o $@ $<

clean:
	rm -f $(OBJS) $(MOD)

.PHONY: all clean
