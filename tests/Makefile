## SPDX-License-Identifier: Apache-2.0
## Copyright (c) Bao Project and Contributors. All rights reserved.
##
## Unit tests for bao hypervisor components.
## Run on the host (x86/ARM macOS/Linux) — not cross-compiled.

CC      := cc
# Add include path for arch headers so <bao.h> resolves when building iommu test
CFLAGS  := -Wall -Wextra -Werror -std=c11 -g -O0 -I../src/arch/riscv/inc/arch
TESTS   := test_irq_rate_limit test_health_monitor test_iommu test_vm_recovery

.PHONY: all clean test

all: $(TESTS)

test_irq_rate_limit: test_irq_rate_limit.c ../src/core/inc/irq_rate_limit.h
	$(CC) $(CFLAGS) -DIRQ_RL_TEST -o $@ $<

test_health_monitor: test_health_monitor.c ../src/core/inc/health_monitor.h
	$(CC) $(CFLAGS) -DHEALTH_MONITOR_TEST -o $@ $<

test_vm_recovery: test_vm_recovery.c ../src/core/inc/health_monitor.h ../src/core/inc/vm_recovery.h ../src/core/vm_recovery.c
	$(CC) $(CFLAGS) -DVM_RECOVERY_TEST -DHEALTH_MONITOR_TEST -o $@ $<

test: $(TESTS)
	@echo ""
	@for t in $(TESTS); do echo ">>> Running $$t"; ./$$t || exit 1; done
	@echo ""
	@echo "All tests passed."

clean:
	rm -f $(TESTS)
