# 跨版本内核模块构建
#
# 目标内核源码树 KERNEL_SRC 必须与设备运行的内核同源，否则 vermagic 对不上，
# insmod 会直接拒绝。换内核后的第一件事是重新生成偏移表：
#
#   python tools/gen_offsets.py --device --out src/cr_offsets.h
#   make KERNEL_SRC=/path/to/kernel OUT=build/6.1 LOCALVERSION=-android14-11-...
#
KERNEL_SRC ?=
ARCH       ?= arm64
CROSS      ?= aarch64-linux-gnu-
OUT        ?= build
LOCALVERSION ?=

all:
	@test -n "$(KERNEL_SRC)" || { echo "需要 KERNEL_SRC=<内核源码树>"; exit 1; }
	@test -f src/cr_offsets.h || { echo "缺 src/cr_offsets.h，先跑 tools/gen_offsets.py"; exit 1; }
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR)/src ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS) \
		CONFIG_MODULE_SIG_ALL=n \
		LOCALVERSION=$(LOCALVERSION) modules
	@mkdir -p $(OUT)
	@cp -f src/cpuinfo_redirect.ko $(OUT)/ 2>/dev/null || true
	@echo "产物: $(OUT)/cpuinfo_redirect.ko"

clean:
	-$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR)/src clean 2>/dev/null || true
	rm -rf $(OUT)
	rm -f src/*.o src/*.ko src/.*.cmd src/Module.symvers src/modules.order 2>/dev/null || true

.PHONY: all clean
