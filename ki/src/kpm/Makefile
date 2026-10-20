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
else
  CC := clang --target=aarch64-linux-gnu
  STRIP ?= llvm-strip
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

OBJS := ki_guard_kpm.o

# 产物名刻意不用 ki_guard：这个文件在 /data/adb 下，厂商内核态检测读得到。
# 名字取成骁龙显示栈里常见的样子，融进背景。
MOD := msm_drm_notify.kpm

all: $(MOD)

# -fuse-ld=lld 是必须的：系统 clang 默认调 /usr/bin/ld（x86_64 的 GNU ld），
# 它不认识 aarch64 emulation，会报 "unrecognised emulation mode: aarch64linux"。
# NDK 的 clang 自带这个默认值，系统 clang 没有。
#
# -r 保留重定位，KernelPatch 运行时自己做 relo
$(MOD): $(OBJS)
	$(CC) -fuse-ld=lld -r -o $@ $^
	$(STRIP) -g --strip-unneeded --strip-debug \
		--remove-section=.comment --remove-section=.note.GNU-stack $@

ki_guard_kpm.o: ki_guard_kpm.c ki_guard_kpm.lds ki_cpuinfo_gen.h Makefile
	$(CC) $(CFLAGS) -T ki_guard_kpm.lds -c -o $@ $<

clean:
	rm -f $(OBJS) $(MOD)

.PHONY: all clean
