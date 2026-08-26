# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0

ifneq ($(KERNELRELEASE),)
obj-m += mooncake_hbm_p2p_probe.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

KBUILD_ARGS :=
ifneq ($(strip $(ASCEND_SYMVERS)),)
KBUILD_ARGS += KBUILD_EXTRA_SYMBOLS=$(abspath $(ASCEND_SYMVERS))
endif

.PHONY: all clean

all:
	@test -d "$(KDIR)" || { \
		echo "FAIL: kernel build directory not found: $(KDIR)"; \
		echo "Run on the host or mount the exact host kernel headers."; \
		exit 2; \
	}
	@if [ -n "$(ASCEND_SYMVERS)" ] && [ ! -f "$(ASCEND_SYMVERS)" ]; then \
		echo "FAIL: ASCEND_SYMVERS not found: $(ASCEND_SYMVERS)"; \
		exit 2; \
	fi
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" $(KBUILD_ARGS) modules

clean:
	@test -d "$(KDIR)" || exit 0
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
