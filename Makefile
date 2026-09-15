KDIR      ?=
WORKSPACE ?=
ARCH      := arm64

obj-m := overclock_mt6855.o

# Beryl 6.12 build must provide the exact target kernel tree/output and Module.symvers.
# Optional: KBUILD_EXTRA_SYMBOLS=/path/to/extra/Module.symvers

CLANG_PREBUILT_BIN := $(shell \
  grep -E "^CLANG_PREBUILT_BIN=" $(WORKSPACE)/common/build.config.common \
  2>/dev/null | cut -d= -f2 | head -1)

ifneq ($(CLANG_PREBUILT_BIN),)
  CLANG_PATH := $(WORKSPACE)/$(CLANG_PREBUILT_BIN)
else
  # Newer branches (android16-6.12+) don't set CLANG_PREBUILT_BIN; derive
  # the path from CLANG_VERSION instead of globbing clang-r*/bin, since the
  # workspace can hold more than one synced clang version and picking the
  # highest-numbered one (sort -V | tail -1) can silently diverge from the
  # clang that actually configured $(KDIR), causing a toolchain mismatch.
  CLANG_VERSION := $(shell \
    grep -E "^CLANG_VERSION=" $(WORKSPACE)/common/build.config.constants \
    2>/dev/null | cut -d= -f2 | head -1)
  CLANG_PATH := $(shell \
    if [ -n "$(CLANG_VERSION)" ] && [ -d "$(WORKSPACE)/prebuilts/clang/host/linux-x86/clang-$(CLANG_VERSION)/bin" ]; then \
      echo "$(WORKSPACE)/prebuilts/clang/host/linux-x86/clang-$(CLANG_VERSION)/bin"; \
    else \
      ls -d $(WORKSPACE)/prebuilts/clang/host/linux-x86/clang-r*/bin 2>/dev/null | sort -V | tail -1; \
    fi)
endif

CC            := $(CLANG_PATH)/clang
CLANG_TRIPLE  := aarch64-linux-gnu-
CROSS_COMPILE := aarch64-linux-gnu-
LLVM          := 1

EXTRA_CFLAGS  := \
  --target=aarch64-linux-gnu \
  -fno-sanitize=shadow-call-stack \
  -Wno-unused-variable \
  -Wno-unused-function \
  -Wno-int-to-pointer-cast \
  -Wno-pointer-to-int-cast \
  -Wno-unused-command-line-argument

.PHONY: all clean install uninstall check-clang

all:
	@if [ -z "$(KDIR)" ]; then echo "ERROR: KDIR must point to the exact Beryl 6.12 kernel tree/output."; exit 1; fi
	@if [ ! -f "$(KDIR)/Makefile" ]; then echo "ERROR: invalid KDIR: $(KDIR)"; exit 1; fi
	@if [ ! -f "$(KDIR)/Module.symvers" ] && [ -z "$(KBUILD_EXTRA_SYMBOLS)" ]; then echo "NOTE: Module.symvers not present under KDIR (fine for modules_prepare-only builds; this module resolves vendor symbols at runtime, not link-time)."; fi
	@if [ -z "$(CLANG_PATH)" ]; then \
		echo "ERROR: Clang not found under GKI prebuilts — check WORKSPACE ($(WORKSPACE)) and repo sync."; \
		exit 1; \
	fi
	@echo "  [overclock_mt6855] Using Clang: $(CLANG_PATH)/clang"
	@$(CLANG_PATH)/clang --version 2>/dev/null | head -1
	PATH="$(CLANG_PATH):$$PATH" $(MAKE) -C $(KDIR) M=$(CURDIR) \
	  ARCH=$(ARCH) \
	  CC=$(CC) \
	  CLANG_TRIPLE=$(CLANG_TRIPLE) \
	  CROSS_COMPILE=$(CROSS_COMPILE) \
	  LLVM=1 \
	  LLVM_IAS=1 \
	  KBUILD_EXTRA_SYMBOLS="$(KBUILD_EXTRA_SYMBOLS)" \
	  EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	  KBUILD_MODPOST_WARN=1 \
		  modules
	@if [ -x "$(CLANG_PATH)/llvm-strip" ]; then \
		$(CLANG_PATH)/llvm-strip --strip-debug overclock_mt6855.ko; \
		echo "  [overclock_mt6855] Stripped debug info:"; \
		ls -lh overclock_mt6855.ko; \
	else \
		echo "WARNING: llvm-strip not found at $(CLANG_PATH) — shipping unstripped .ko."; \
	fi

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install:
	adb push overclock_mt6855.ko /data/local/tmp/
	adb shell su -c "insmod /data/local/tmp/overclock_mt6855.ko"
	@sleep 1
	adb shell su -c "dmesg | grep overclock_mt6855 | tail -20"

uninstall:
	adb shell su -c "rmmod overclock_mt6855 && echo OK"

check-clang:
	@echo "WORKSPACE    : $(WORKSPACE)"
	@if [ -z "$(KDIR)" ]; then echo "ERROR: KDIR must point to the exact Beryl 6.12 kernel tree/output."; exit 1; fi
	@if [ ! -f "$(KDIR)/Makefile" ]; then echo "ERROR: invalid KDIR: $(KDIR)"; exit 1; fi
	@if [ ! -f "$(KDIR)/Module.symvers" ] && [ -z "$(KBUILD_EXTRA_SYMBOLS)" ]; then echo "NOTE: Module.symvers not present under KDIR (fine for modules_prepare-only builds; this module resolves vendor symbols at runtime, not link-time)."; fi
	@if [ -z "$(CLANG_PATH)" ]; then \
		echo "CLANG_PATH   : NOT FOUND — check WORKSPACE and repo sync"; \
		echo "Clang version: n/a"; \
	else \
		echo "CLANG_PATH   : $(CLANG_PATH)"; \
		echo "Clang version: $$($(CLANG_PATH)/clang --version 2>/dev/null | head -1)"; \
	fi
	@echo "KDIR         : $(KDIR)"
	@echo ""
	@CFI_LINE=$$(grep -E "^CONFIG_CFI_CLANG=" $(KDIR)/.config 2>/dev/null); \
	if [ -f "$(KDIR)/.config" ]; then \
		if [ "$$CFI_LINE" = "CONFIG_CFI_CLANG=y" ]; then \
			echo "CONFIG_CFI_CLANG: y (kbuild will inject KCFI flags as configured)"; \
		else \
			echo "WARNING: CONFIG_CFI_CLANG is not 'y' in $(KDIR)/.config."; \
		fi; \
	else \
		echo "WARNING: $(KDIR)/.config not found — cannot verify CONFIG_CFI_CLANG."; \
	fi
