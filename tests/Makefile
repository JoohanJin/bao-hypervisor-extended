## SPDX-License-Identifier: Apache-2.0
## Copyright (c) Bao Project and Contributors. All rights reserved.
##
## Unit tests for bao hypervisor components.
## Run on the host (x86/ARM macOS/Linux) — not cross-compiled.

CC      := cc
CFLAGS  := -Wall -Wextra -Werror -std=c11 -g -O0
TESTS   := test_irq_rate_limit

.PHONY: all clean test

all: $(TESTS)

test_irq_rate_limit: test_irq_rate_limit.c ../src/core/inc/irq_rate_limit.h
	$(CC) $(CFLAGS) -DIRQ_RL_TEST -o $@ $<

test: $(TESTS)
	@echo ""
	@for t in $(TESTS); do echo ">>> Running $$t"; ./$$t || exit 1; done
	@echo ""
	@echo "All tests passed."

clean:
	rm -f $(TESTS)
