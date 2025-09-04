# RVV (RISC-V Vector Extension) optimization configuration
# Copyright (C) 2024 LZ4 contributors
# This file provides build configuration for RISC-V Vector Extension support

# RVV optimization flags
RVV_CFLAGS = -march=rv64gcv -DLZ4_RVV_ENABLED=1

# Test if compiler supports RVV
RVV_TEST = $(shell echo "int main(){return 0;}" | $(CC) -march=rv64gcv -x c - -o /dev/null 2>/dev/null && echo yes)

# Enable RVV optimization if supported
ifeq ($(RVV_TEST),yes)
    CFLAGS += $(RVV_CFLAGS)
    $(info LZ4: RISC-V Vector Extension optimization enabled)
else
    $(info LZ4: RISC-V Vector Extension not available or compiler doesn't support it)
endif

# RVV specific targets
.PHONY: rvv-test rvv-bench

rvv-test: CFLAGS += $(RVV_CFLAGS) -DLZ4_DEBUG=1
rvv-test:
	$(MAKE) -C tests test-lz4-basic

rvv-bench: CFLAGS += $(RVV_CFLAGS) -O3 -DNDEBUG
rvv-bench:
	$(MAKE) -C programs bench