# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0

ifneq ($(KERNELRELEASE),)
obj-m += mooncake_hbm_p2p_probe.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all:
	@test -d "$(KDIR)" || { \
		echo "FAIL: kernel build directory not found: $(KDIR)"; \
		echo "Run on the host or mount the exact host kernel headers."; \
		exit 2; \
	}
	@test -f "$(KDIR)/Module.symvers" || { \
		echo "FAIL: kernel Module.symvers not found: $(KDIR)/Module.symvers"; \
		exit 2; \
	}
	@grep -qw '__symbol_get' "$(KDIR)/Module.symvers" || { \
		echo "FAIL: __symbol_get is not exported by the target kernel"; \
		exit 2; \
	}
	@grep -qw '__symbol_put' "$(KDIR)/Module.symvers" || { \
		echo "FAIL: __symbol_put is not exported by the target kernel"; \
		exit 2; \
	}
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

clean:
	@test -d "$(KDIR)" || exit 0
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
