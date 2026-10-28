# ##########################################################################
# LZ4 programs - Makefile
# Copyright (C) Yann Collet 2011-2020
#
# GPL v2 License
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# You can contact the author at :
#  - LZ4 homepage : http://www.lz4.org
#  - LZ4 source repository : https://github.com/lz4/lz4
# ##########################################################################
# fuzzer  : Test tool, to check lz4 integrity on target platform
# frametest  : Test tool, to check lz4frame integrity on target platform
# fullbench  : Precisely measure speed for each LZ4 function variant
# datagen : generates synthetic data samples for tests & benchmarks
# ##########################################################################

LIBDIR  := ../lib
PRGDIR  := ../programs
VERSIONSTESTDIR := versionsTest
PYTHON  ?= python3

DEBUGLEVEL?= 1
DEBUGFLAGS = -g -DLZ4_DEBUG=$(DEBUGLEVEL)
USERCFLAGS:= -O3 $(CFLAGS) # appended for higher priority
WFLAGS    = -Wall -Wextra -Wundef -Wcast-qual -Wcast-align -Wshadow \
            -Wswitch-enum -Wdeclaration-after-statement -Wstrict-prototypes \
            -Wpointer-arith -Wstrict-aliasing=1
CFLAGS    = $(WFLAGS) $(DEBUGFLAGS) $(JCC_FLAGS) $(USERCFLAGS)   # JCC_FLAGS : see build/make/lz4defs.make
CPPFLAGS += -I$(LIBDIR) -I$(PRGDIR) -Itools -DXXH_NAMESPACE=LZ4_
ALLFLAGS  = $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)

include ../build/make/lz4defs.make

LZ4 := $(PRGDIR)/lz4$(EXT)

# test sources : see README.md
C_SRCDIRS = $(LIBDIR) $(PRGDIR) . unit fuzz compat bench tools
include ../build/make/multiconf.make

# Default test parameters
TEST_FILES   := data/COPYING
FUZZER_TIME  := -T90s
NB_LOOPS     ?= -i1

.PHONY: default
default: all

.PHONY: all
all: fullbench fuzzer frametest unittest datagen checkFrame

.PHONY: all32
all32: CFLAGS+=-m32
all32: all

.PHONY: lz4
lz4: MAKEFLAGS=
lz4:
	$(MAKE) -C $(PRGDIR) $@ CFLAGS="$(CFLAGS)"

.PHONY: lib liblz4.pc
lib liblz4.pc: MAKEFLAGS=
lib liblz4.pc:
	CFLAGS="$(CFLAGS)" $(MAKE) -C $(LIBDIR) $@

lz4c unlz4 lz4cat: lz4
	$(LN_SF) $(LZ4) $(PRGDIR)/$@

.PHONY: lz4c32
lz4c32: MAKEFLAGS=
lz4c32:  # create a 32-bits version for 32/64 interop tests
	$(MAKE) -C $(PRGDIR) $@ CFLAGS="-m32 $(CFLAGS)"

# *.o objects are from library
%.o : $(LIBDIR)/%.c $(LIBDIR)/%.h
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $< -o $@

fullbench : DEBUGLEVEL=0
fullbench : CPPFLAGS += -DNDEBUG
$(eval $(call c_program,fullbench,fullbench.o lz4.o lz4hc.o lz4frame.o xxhash.o))

.PHONY: $(LIBDIR)/liblz4.a
$(LIBDIR)/liblz4.a: MAKEFLAGS=
$(LIBDIR)/liblz4.a:
	$(MAKE) -C $(LIBDIR) liblz4.a

CLEAN += fullbench-lib
fullbench-lib : DEBUGLEVEL=0
fullbench-lib : CPPFLAGS += -DNDEBUG
fullbench-lib: bench/fullbench.c $(LIBDIR)/liblz4.a
	$(CC) $(ALLFLAGS) $^ -o $@$(EXT)

# Note: Windows only
ifeq ($(WINBASED),yes)
CLEAN += fullbench-dll
fullbench-dll : DEBUGLEVEL=0
fullbench-dll : CPPFLAGS += -DNDEBUG
fullbench-dll : MAKEFLAGS=
fullbench-dll: bench/fullbench.c $(LIBDIR)/xxhash.c
	$(MAKE) -C $(LIBDIR) liblz4
	$(CC) $(ALLFLAGS) $^ -o $@$(EXT) -DLZ4_DLL_IMPORT=1 $(LIBDIR)/dll/$(LIBLZ4).dll
endif

# test LZ4_USER_MEMORY_FUNCTIONS
.PHONY: fullbench-wmalloc
fullbench-wmalloc: CPPFLAGS += -DLZ4_USER_MEMORY_FUNCTIONS
fullbench-wmalloc: fullbench

fuzzer:
$(eval $(call c_program_shared_o,fuzzer,lz4.o lz4hc.o xxhash.o fuzzer.o))

frametest:
$(eval $(call c_program_shared_o,frametest,lz4frame.o lz4.o lz4hc.o xxhash.o frametest.o))

UNITTEST_OBJS := lz4frame.o lz4file.o lz4.o lz4hc.o xxhash.o datagen.o timefn.o \
                 unittest.o lz4frame_helpers.o unittest_lz4.o unittest_lz4hc.o unittest_lz4frame.o unittest_lz4frameMT.o
unittest:
$(eval $(call c_program_shared_o,unittest,$(UNITTEST_OBJS)))

datagen: CPPFLAGS+=-DNDEBUG
$(eval $(call c_program,datagen,datagen.o lorem.o loremOut.o datagencli.o))

checkFrame:
$(eval $(call c_program_shared_o,checkFrame,lz4.o lz4hc.o xxhash.o lz4frame.o checkFrame.o))

# POSIX threads, except on MinGW and Windows : Windows threads, as in lib/Makefile
# (clang rejects an unused -pthread with -Werror)
PTHREAD_LDFLAGS := $(if $(filter MINGW% Windows%,$(TARGET_OS)),,-pthread)

# multi-threaded frame compression : same test, built with and without LZ4F_MULTITHREAD
# (c_program : objects are not shared, since frameMT employs different flags)
FRAMEMT_OBJS := lz4frame.o lz4.o lz4hc.o xxhash.o datagen.o timefn.o frameMT.o
# LZ4F_NBWORKERS_MAX=128 (as on 32-bit systems) : keeps the capped test case (1000 workers) affordable
frameMT: CPPFLAGS += -DLZ4F_MULTITHREAD=1 -DLZ4F_NBWORKERS_MAX=128u
frameMT: LDFLAGS += $(PTHREAD_LDFLAGS)
$(eval $(call c_program,frameMT,$(FRAMEMT_OBJS)))

frameMT-nomt: CPPFLAGS += -DLZ4F_NBWORKERS_MAX=128u
$(eval $(call c_program,frameMT-nomt,$(FRAMEMT_OBJS)))

# unit tests, with multi-threading support
unittest-mt: CPPFLAGS += -DLZ4F_MULTITHREAD=1
unittest-mt: LDFLAGS += $(PTHREAD_LDFLAGS)
$(eval $(call c_program,unittest-mt,$(UNITTEST_OBJS)))

.PHONY: clean
clean: clean_cache
	@$(MAKE) -C $(LIBDIR) $@ > $(VOID)
	@$(MAKE) -C $(PRGDIR) $@ > $(VOID)
	@$(RM) $(CLEAN) core *.o *.test
	@$(RM) -r $(VERSIONSTESTDIR) tmp*
	@echo Cleaning completed

.PHONY: versionsTest
versionsTest:
	$(PYTHON) compat/test-lz4-versions.py

.PHONY: listTest
listTest: lz4
	QEMU_SYS=$(QEMU_SYS) $(PYTHON) cli/test-lz4-list.py

# Note: requires liblz4 installed
CLEAN += abiTest
abiTest: LDLIBS += -llz4

.PHONY: abiTests
abiTests:
	$(PYTHON) compat/test-lz4-abi.py

checkTag:
$(eval $(call c_program_shared_o,checkTag,checkTag.o))


#-----------------------------------------------------------------------------
# validated only for Linux, OSX, BSD, Hurd and Solaris targets
#-----------------------------------------------------------------------------
ifeq ($(POSIX_ENV),Yes)

MD5 ?= $(if $(filter Darwin,$(shell $(UNAME))),md5 -r,md5sum)
GREP?= grep
CAT ?= cat
DATAGEN?=./datagen
PATH:=../programs:$(shell pwd):$(PATH)

.PHONY: list
list:
	$(GREP) '^[^#[:space:]].*:' Makefile | sed 's/:.*//' | sort | uniq

ifneq ($(TARGETSEARCH),NO)
ALL_TARGETS := $(shell make list TARGETSEARCH=NO)
endif
TEST_TARGETS := $(filter test%,$(ALL_TARGETS))
.PHONY: $(TEST_TARGETS) # all targets starting by `test` are now .PHONY

test_targets:
	@echo TEST_TARGETS = $(TEST_TARGETS)

.PHONY: check
check: test-lz4-essentials

test: test-lz4 test-lz4c test-frametest test-frameMT-quick test-unittest test-fullbench test-fuzzer test-amalgamation listTest

test32: CFLAGS+=-m32
test32: test

test-amalgamation: lz4_all.o

lz4_all.o: CFLAGS += -Werror -std=c90

CLEAN += lz4_all.c
lz4_all.c: $(LIBDIR)/lz4.c $(LIBDIR)/lz4hc.c $(LIBDIR)/lz4frame.c
	$(CAT) $^ > $@

test-install: lz4 lib liblz4.pc
	lz4_root=.. ./compat/test_install.sh

test-compile-with-lz4-memory-usage:
	$(MAKE) clean; CFLAGS=-O0 CPPFLAGS=-D'LZ4_MEMORY_USAGE=LZ4_MEMORY_USAGE_MIN' $(MAKE) all
	$(MAKE) clean; CFLAGS=-O0 CPPFLAGS=-D'LZ4_MEMORY_USAGE=LZ4_MEMORY_USAGE_MAX' $(MAKE) all

# Rules regarding Temporary test files :
# Each test must use its own unique set of names during execution.
# Each temporary test file must begin by an FPREFIX.
# Each FPREFIX must be unique for each test.
# All FPREFIX must start with `tmp`, for `make clean`
# All tests must clean their temporary test files on successful completion,
# and only their test files : do not employ sweeping statements such `rm tmp*` or `rm *.lz4`
test-lz4-sparse: lz4 datagen
	@echo "\n ---- test sparse file support ----"
	./cli/test-lz4-sparse.sh

test-lz4-contentSize: lz4 datagen
	@echo "\n ---- test original size support ----"
	./cli/test-lz4-contentSize.sh

test-lz4-frame-concatenation: lz4 datagen
	@echo "\n ---- test frame concatenation ----"
	./cli/test-lz4-frame-concatenation.sh

test-lz4-multiple: lz4 datagen
	@echo "\n ---- test multiple files ----"
	./cli/test-lz4-multiple.sh

test-lz4-multiple-legacy: lz4 datagen
	@echo "\n ---- test multiple files (Legacy format) ----"
	./cli/test-lz4-multiple-legacy.sh

test-lz4-skippable: lz4
	@echo "\n ---- test lz4 with skippable frames ----"
	./cli/test-lz4-skippable.sh

test-lz4-mozlz4: lz4 datagen lz4cat
	@echo "\n ---- test lz4 decoding Firefox mozlz4 files ----"
	./cli/test-lz4-mozlz4.sh

test-datagen-invalid-input: datagen
	@echo "\n ---- test datagen fails correctly on invalid input ----"
	$(DATAGEN) -g1234l && exit 1 || true

test-lz4-basic: lz4 datagen unlz4 lz4cat
	@echo "\n ---- test lz4 basic compression/decompression ----"
	./cli/test-lz4-basic.sh

test-lz4-dict: lz4 datagen
	@echo "\n ---- test lz4 compression/decompression with dictionary ----"
	./cli/test-lz4-dict.sh

test-lz4-blocksize: lz4 datagen checkFrame
	@echo "\n ---- test lz4 with custom block sizes ----"
	./cli/test-lz4-blocksize.sh

test-lz4-multithread: lz4 datagen
	@echo "\n ---- test lz4 multithreaded compression, memory budget, sources modified meanwhile ----"
	./cli/test-lz4-multithread.sh

test-lz4hc-hugefile: lz4 datagen
	@echo "\n ---- test HC compression/decompression of huge files ----"
	./cli/test-lz4hc-hugefile.sh

test-lz4-fast-hugefile: lz4 datagen
	@echo "\n ---- test huge files compression/decompression ----"
	./cli/test-lz4-fast-hugefile.sh

test-lz4-hugefile: test-lz4-fast-hugefile test-lz4hc-hugefile

test-lz4-testmode: lz4 datagen
	@echo "\n ---- bench mode ----"
	./cli/test-lz4-testmode.sh

test-lz4-opt-parser: lz4 datagen
	@echo "\n ---- test opt-parser ----"
	./cli/test-lz4-opt-parser.sh

test-lz4-essentials : lz4 datagen test-datagen-invalid-input test-lz4-basic test-lz4-multiple \
                      test-lz4-multiple-legacy test-lz4-frame-concatenation \
                      test-lz4-testmode test-lz4-contentSize test-lz4-dict \
                      test-lz4-blocksize test-lz4-mozlz4 test-lz4-multithread

test-lz4: lz4 datagen test-lz4-essentials test-lz4-opt-parser \
          test-lz4-sparse test-lz4-hugefile test-lz4-dict \
          test-lz4-skippable

test-lz4c: LZ4C := $(basename $(LZ4))c$(suffix $(LZ4))
test-lz4c: lz4c datagen
	@echo "\n ---- test lz4c variant ----"
	$(DATAGEN) -g256MB | $(LZ4C) -l -v | $(LZ4C) -t

test-lz4c32: CFLAGS+=-m32
test-lz4c32: test-lz4

test-interop-32-64: lz4 lz4c32 datagen
	@echo "\n ---- test interoperability 32-bits -vs- 64 bits ----"
	$(DATAGEN) -g16KB  | $(LZ4)c32 -9     | $(LZ4)    -t
	$(DATAGEN) -P10    | $(LZ4)    -9B4   | $(LZ4)c32 -t
	$(DATAGEN)         | $(LZ4)c32        | $(LZ4)    -t
	$(DATAGEN) -g1M    | $(LZ4)    -3B5   | $(LZ4)c32 -t
	$(DATAGEN) -g256MB | $(LZ4)c32 -vqB4D | $(LZ4)    -qt
	$(DATAGEN) -g1G -P90 | $(LZ4)         | $(LZ4)c32 -t
	$(DATAGEN) -g6GB   | $(LZ4)c32 -vq9BD | $(LZ4)    -qt

test-lz4c32-basic: lz4c32 datagen
	@echo "\n ---- test lz4c32 32-bits version ----"
	$(DATAGEN) -g16KB  | $(LZ4)c32 -9     | $(LZ4)c32 -t
	$(DATAGEN)         | $(LZ4)c32        | $(LZ4)c32 -t
	$(DATAGEN) -g256MB | $(LZ4)c32 -vqB4D | $(LZ4)c32 -qt
	$(DATAGEN) -g6GB   | $(LZ4)c32 -vqB5D | $(LZ4)c32 -qt

test-platform:
	@echo "\n ---- test lz4 $(QEMU_SYS) platform ----"
	$(QEMU_SYS) $(DATAGEN) -g16KB  | $(QEMU_SYS) $(LZ4) -9     | $(QEMU_SYS) $(LZ4) -t
	$(QEMU_SYS) $(DATAGEN)         | $(QEMU_SYS) $(LZ4)        | $(QEMU_SYS) $(LZ4) -t
	$(QEMU_SYS) $(DATAGEN) -g256MB | $(QEMU_SYS) $(LZ4) -vqB4D | $(QEMU_SYS) $(LZ4) -qt
ifneq ($(QEMU_SYS),qemu-arm-static)
	$(QEMU_SYS) $(DATAGEN) -g3GB   | $(QEMU_SYS) $(LZ4) -vqB5D | $(QEMU_SYS) $(LZ4) -qt
endif

test-fullbench: fullbench
	./fullbench --no-prompt $(NB_LOOPS) $(TEST_FILES)

test-fullbench32: CFLAGS += -m32
test-fullbench32: test-fullbench

test-fuzzer: fuzzer
	./fuzzer $(FUZZER_TIME)

test-fuzzer32: CFLAGS += -m32
test-fuzzer32: test-fuzzer

test-frametest: frametest
	./frametest -v $(FUZZER_TIME)

# same frames with and without multi-threading support
.PHONY: test-frameMT test-frameMT-quick
test-frameMT: frameMT frameMT-nomt
	./frameMT > tmp-frameMT.digest
	./frameMT-nomt > tmp-frameMT-nomt.digest
	cmp tmp-frameMT.digest tmp-frameMT-nomt.digest

test-frameMT-quick: frameMT frameMT-nomt
	./frameMT -q > tmp-frameMT.digest
	./frameMT-nomt -q > tmp-frameMT-nomt.digest
	cmp tmp-frameMT.digest tmp-frameMT-nomt.digest

test-frametest32: CFLAGS += -m32
test-frametest32: test-frametest

# random seed : explores more data ; a failure reports its seed
test-unittest: unittest unittest-mt
	./unittest -r
	./unittest-mt -r

VALGRIND = valgrind --leak-check=yes --error-exitcode=1
test-mem: FPREFIX = tmp-tvm
test-mem: lz4 datagen fuzzer frametest unittest fullbench
	@echo "\n ---- valgrind tests : memory analyzer ----"
	$(VALGRIND) $(DATAGEN) -g50M > $(VOID)
	$(DATAGEN) -g16KB > $(FPREFIX)dg16K
	$(VALGRIND) $(LZ4) -9 -BD -f $(FPREFIX)dg16K $(VOID)
	$(DATAGEN) -g16KB -s2 > $(FPREFIX)dg16K2
	$(DATAGEN) -g16KB -s3 > $(FPREFIX)dg16K3
	$(VALGRIND) $(LZ4) --force --multiple $(FPREFIX)dg16K $(FPREFIX)dg16K2 $(FPREFIX)dg16K3
	$(DATAGEN) -g7MB > $(FPREFIX)dg7M
	$(VALGRIND) $(LZ4) -9 -B5D -f $(FPREFIX)dg7M $(FPREFIX)dg16K2
	$(VALGRIND) $(LZ4) -t $(FPREFIX)dg16K2
	$(VALGRIND) $(LZ4) -bi1 $(FPREFIX)dg7M
	$(VALGRIND) ./fullbench -i1 $(FPREFIX)dg7M $(FPREFIX)dg16K2
	$(VALGRIND) $(LZ4) -B4D -f -vq $(FPREFIX)dg7M $(VOID)
	$(VALGRIND) $(LZ4) --list -m $(FPREFIX)*.lz4
	$(VALGRIND) $(LZ4) --list -m -v $(FPREFIX)*.lz4
	# truncated frame headers : not recognized, without reading uninitialized bytes (valgrind error : 99)
	$(LZ4) -f --content-size $(FPREFIX)dg16K $(FPREFIX)cs
	head -c 5 $(FPREFIX)dg16K.lz4 > $(FPREFIX)hdr5
	head -c 10 $(FPREFIX)cs > $(FPREFIX)hdr10
	$(VALGRIND) --error-exitcode=99 $(LZ4) --list $(FPREFIX)hdr5; test $$? -eq 1
	$(VALGRIND) --error-exitcode=99 $(LZ4) --list $(FPREFIX)hdr10; test $$? -eq 1
	$(RM) $(FPREFIX)*
	$(VALGRIND) ./fuzzer -i64 -t1
	$(VALGRIND) ./frametest -i256
	$(VALGRIND) ./unittest

test-mem32: lz4c32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary...

# public headers must not be included within extern "C" (#1600)
# requires clang : -fmodules turns such inclusions into errors
.PHONY: test-cxx-modules
test-cxx-modules:
	@echo "\n ---- test public headers with C++ modules ----"
	echo 'int main(void) { return 0; }' | $(CXX) -x c++ -fsyntax-only -fmodules -fcxx-modules \
		-fmodules-cache-path=tmp-cxx-modules -I$(LIBDIR) -DLZ4_HC_STATIC_LINKING_ONLY \
		-include lz4hc.h -include lz4file.h -
	$(RM) -r tmp-cxx-modules


#-----------------------------------------------------------------------------
# freestanding test only for Linux x86_64
# Query the compiler's target rather than the host (uname),
# so that the decision matches the guard in freestanding.c,
# even with CC="gcc -m32" or a cross-compiler.
#-----------------------------------------------------------------------------
NUM_SYMBOL := \#
FREESTANDING_TARGET := $(strip $(shell printf '$(NUM_SYMBOL)if defined(__x86_64__) && defined(__linux__)\nyes\n$(NUM_SYMBOL)endif\n' | $(CC) -E -P -x c - 2> $(VOID)))

FREESTANDING_CFLAGS :=
ifeq ($(FREESTANDING_TARGET), yes)
  FREESTANDING_CFLAGS := -ffreestanding -nostdlib
endif

CLEAN += freestanding
freestanding: compat/freestanding.c
	$(CC) $(FREESTANDING_CFLAGS) $^ -o $@$(EXT)

test-freestanding: freestanding
	@echo "\n ---- test freestanding ----"
ifeq ($(FREESTANDING_CFLAGS),)
	@echo "\n (skip)"
else
	./freestanding$(EXT)
	-strace ./freestanding$(EXT)
	-ltrace ./freestanding$(EXT)
endif


endif
