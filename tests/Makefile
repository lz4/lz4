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

LZ4DIR  := ../lib
PRGDIR  := ../programs
TESTDIR := versionsTest
PYTHON  ?= python3

DEBUGLEVEL?= 1
DEBUGFLAGS = -g -DLZ4_DEBUG=$(DEBUGLEVEL)
CFLAGS  ?= -O3 # can select custom optimization flags. Example : CFLAGS=-O2 make
CFLAGS  += -Wall -Wextra -Wundef -Wcast-qual -Wcast-align -Wshadow \
           -Wswitch-enum -Wdeclaration-after-statement -Wstrict-prototypes \
           -Wpointer-arith -Wstrict-aliasing=1
CFLAGS  += $(DEBUGFLAGS) $(MOREFLAGS)
CPPFLAGS+= -I$(LZ4DIR) -I$(PRGDIR) -DXXH_NAMESPACE=LZ4_
FLAGS    = $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)

include ../Makefile.inc

LZ4 := $(PRGDIR)/lz4$(EXT)


# Default test parameters
TEST_FILES   := COPYING
FUZZER_TIME  := -T90s
NB_LOOPS     ?= -i1

.PHONY: default
default: all

all: fullbench fuzzer frametest roundTripTest datagen checkFrame decompress-partial

all32: CFLAGS+=-m32
all32: all

lz4:
	$(MAKE) -C $(PRGDIR) $@ CFLAGS="$(CFLAGS)"

lib liblz4.pc:
	$(MAKE) -C $(LZ4DIR) $@ CFLAGS="$(CFLAGS)"

lz4c unlz4 lz4cat: lz4
	$(LN_SF) $(LZ4) $(PRGDIR)/$@

lz4c32:   # create a 32-bits version for 32/64 interop tests
	$(MAKE) -C $(PRGDIR) $@ CFLAGS="-m32 $(CFLAGS)"

%.o : $(LZ4DIR)/%.c $(LZ4DIR)/%.h
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $< -o $@

fullbench : DEBUGLEVEL=0
fullbench : lz4.o lz4hc.o lz4frame.o xxhash.o fullbench.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

$(LZ4DIR)/liblz4.a:
	$(MAKE) -C $(LZ4DIR) liblz4.a

fullbench-lib: fullbench.c $(LZ4DIR)/liblz4.a
	$(CC) $(FLAGS) $^ -o $@$(EXT)

fullbench-dll: fullbench.c $(LZ4DIR)/xxhash.c
	$(MAKE) -C $(LZ4DIR) liblz4
	$(CC) $(FLAGS) $^ -o $@$(EXT) -DLZ4_DLL_IMPORT=1 $(LZ4DIR)/dll/$(LIBLZ4).dll

# test LZ4_USER_MEMORY_FUNCTIONS
fullbench-wmalloc: CPPFLAGS += -DLZ4_USER_MEMORY_FUNCTIONS
fullbench-wmalloc: fullbench

fuzzer  : lz4.o lz4hc.o xxhash.o fuzzer.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

frametest: lz4frame.o lz4.o lz4hc.o xxhash.o frametest.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

roundTripTest : lz4.o lz4hc.o xxhash.o roundTripTest.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

datagen : $(PRGDIR)/datagen.c datagencli.c
	$(CC) $(FLAGS) -I$(PRGDIR) $^ -o $@$(EXT)

checkFrame : lz4frame.o lz4.o lz4hc.o xxhash.o checkFrame.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

decompress-partial: lz4.o decompress-partial.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

decompress-partial-usingDict: lz4.o decompress-partial-usingDict.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

freestanding: freestanding.c
	$(CC) -ffreestanding -nostdlib $^ -o $@$(EXT)

.PHONY: clean
clean:
	@$(MAKE) -C $(LZ4DIR) $@ > $(VOID)
	@$(MAKE) -C $(PRGDIR) $@ > $(VOID)
	@$(RM) -rf core *.o *.test tmp* \
        fullbench-dll$(EXT) fullbench-lib$(EXT) \
        fullbench$(EXT) fullbench32$(EXT) \
        fuzzer$(EXT) fuzzer32$(EXT) \
        frametest$(EXT) frametest32$(EXT) \
        fasttest$(EXT) roundTripTest$(EXT) \
        datagen$(EXT) checkTag$(EXT) \
        frameTest$(EXT) decompress-partial$(EXT) \
        abiTest$(EXT) freestanding$(EXT) \
        lz4_all.c
	@$(RM) -rf $(TESTDIR)
	@echo Cleaning completed

.PHONY: versionsTest
versionsTest:
	$(PYTHON) test-lz4-versions.py

.PHONY: listTest
listTest: lz4
	QEMU_SYS=$(QEMU_SYS) $(PYTHON) test-lz4-list.py

abiTest: LDLIBS += -llz4

.PHONY: abiTests
abiTests:
	$(PYTHON) test-lz4-abi.py

checkTag: checkTag.c $(LZ4DIR)/lz4.h
	$(CC) $(FLAGS) $< -o $@$(EXT)

#-----------------------------------------------------------------------------
# validated only for Linux, OSX, BSD, Hurd and Solaris targets
#-----------------------------------------------------------------------------
ifeq ($(POSIX_ENV),Yes)

MD5:=md5sum
ifneq (,$(filter $(shell $(UNAME)), Darwin ))
MD5:=md5 -r
endif

# note : we should probably settle on a single compare utility
CMP:=cmp
GREP:=grep
DIFF:=diff
ifneq (,$(filter $(shell $(UNAME)),SunOS))
DIFF:=gdiff
endif

CAT:=cat
DD:=dd
DATAGEN:=./datagen

.PHONY: list
list:
	@$(MAKE) -pRrq -f $(lastword $(MAKEFILE_LIST)) : 2>/dev/null | awk -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' | sort | egrep -v -e '^[^[:alnum:]]' -e '^$@$$' | xargs

.PHONY: check
check: test-lz4-essentials

.PHONY: test
test: test-lz4 test-lz4c test-frametest test-fullbench test-fuzzer test-amalgamation listTest test-decompress-partial

.PHONY: test32
test32: CFLAGS+=-m32
test32: test

.PHONY: test-amalgamation
test-amalgamation: lz4_all.o

lz4_all.c: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c
	$(CAT) $^ > $@

.PHONY: test-install
test-install: lz4 lib liblz4.pc
	lz4_root=.. ./test_install.sh

.PHONY: test-compile-with-lz4-memory-usage
test-compile-with-lz4-memory-usage:
	$(MAKE) clean; CFLAGS=-O0 CPPFLAGS=-D'LZ4_MEMORY_USAGE=LZ4_MEMORY_USAGE_MIN' $(MAKE) all
	$(MAKE) clean; CFLAGS=-O0 CPPFLAGS=-D'LZ4_MEMORY_USAGE=LZ4_MEMORY_USAGE_MAX' $(MAKE) all

.PHONY: test-lz4-sparse
# Rules regarding Temporary test files :
# Each test must use its own unique set of names during execution.
# Each temporary test file must begin by an FPREFIX.
# Each FPREFIX must be unique for each test.
# All FPREFIX must start with `tmp`, for `make clean`
# All tests must clean their temporary test files on successful completion,
# and only their test files : do not employ sweeping statements such `rm tmp*` or `rm *.lz4`
test-lz4-sparse: FPREFIX = tmp-tls
test-lz4-sparse: lz4 datagen
	@echo "\n ---- test sparse file support ----"
	$(DATAGEN) -g5M  -P100 > $(FPREFIX)dg5M
	$(LZ4) -B4D $(FPREFIX)dg5M -c | $(LZ4) -dv --sparse > $(FPREFIX)cB4
	$(DIFF) -s $(FPREFIX)dg5M $(FPREFIX)cB4
	$(LZ4) -B5D $(FPREFIX)dg5M -c | $(LZ4) -dv --sparse > $(FPREFIX)cB5
	$(DIFF) -s $(FPREFIX)dg5M $(FPREFIX)cB5
	$(LZ4) -B6D $(FPREFIX)dg5M -c | $(LZ4) -dv --sparse > $(FPREFIX)cB6
	$(DIFF) -s $(FPREFIX)dg5M $(FPREFIX)cB6
	$(LZ4) -B7D $(FPREFIX)dg5M -c | $(LZ4) -dv --sparse > $(FPREFIX)cB7
	$(DIFF) -s $(FPREFIX)dg5M $(FPREFIX)cB7
	$(LZ4) $(FPREFIX)dg5M -c | $(LZ4) -dv --no-sparse > $(FPREFIX)nosparse
	$(DIFF) -s $(FPREFIX)dg5M $(FPREFIX)nosparse
	ls -ls $(FPREFIX)*
	$(DATAGEN) -s1 -g1200007 -P100 | $(LZ4) | $(LZ4) -dv --sparse > $(FPREFIX)odd   # Odd size file (to generate non-full last block)
	$(DATAGEN) -s1 -g1200007 -P100 | $(DIFF) -s - $(FPREFIX)odd
	ls -ls $(FPREFIX)odd
	@$(RM) $(FPREFIX)*
	@echo "\n Compatibility with Console :"
	echo "Hello World 1 !" | $(LZ4) | $(LZ4) -d -c
	echo "Hello World 2 !" | $(LZ4) | $(LZ4) -d | $(CAT)
	echo "Hello World 3 !" | $(LZ4) --no-frame-crc | $(LZ4) -d -c
	@echo "\n Compatibility with Append :"
	$(DATAGEN) -P100 -g1M > $(FPREFIX)dg1M
	$(CAT) $(FPREFIX)dg1M $(FPREFIX)dg1M > $(FPREFIX)2M
	$(LZ4) -B5 -v $(FPREFIX)dg1M $(FPREFIX)c
	$(LZ4) -d -v $(FPREFIX)c $(FPREFIX)r
	$(LZ4) -d -v $(FPREFIX)c -c >> $(FPREFIX)r
	ls -ls $(FPREFIX)*
	$(DIFF) $(FPREFIX)2M $(FPREFIX)r
	@$(RM) $(FPREFIX)*

test-lz4-contentSize: FPREFIX = tmp-lzc
test-lz4-contentSize: lz4 datagen
	@echo "\n ---- test original size support ----"
	$(DATAGEN) -g15M > $(FPREFIX)
	$(LZ4) -v $(FPREFIX) -c | $(LZ4) -t
	$(LZ4) -v --content-size $(FPREFIX) -c | $(LZ4) -d > $(FPREFIX)-dup
	$(DIFF) $(FPREFIX) $(FPREFIX)-dup
	$(LZ4) -f $(FPREFIX) -c > $(FPREFIX).lz4 # compressed with content size
	$(LZ4) --content-size $(FPREFIX) -c > $(FPREFIX)-wcz.lz4
	! $(DIFF) $(FPREFIX).lz4 $(FPREFIX)-wcz.lz4  # must differ, due to content size
	$(LZ4) --content-size < $(FPREFIX) > $(FPREFIX)-wcz2.lz4 # can determine content size because stdin is just a file
	$(DIFF) $(FPREFIX)-wcz.lz4 $(FPREFIX)-wcz2.lz4  # both must contain content size
	$(CAT) $(FPREFIX) | $(LZ4) > $(FPREFIX)-ncz.lz4
	$(DIFF) $(FPREFIX).lz4 $(FPREFIX)-ncz.lz4  # both don't have content size
	$(CAT) $(FPREFIX) | $(LZ4) --content-size > $(FPREFIX)-ncz2.lz4 # can't determine content size
	$(DIFF) $(FPREFIX).lz4 $(FPREFIX)-ncz2.lz4  # both don't have content size
	@$(RM) $(FPREFIX)*

test-lz4-frame-concatenation: FPREFIX = tmp-lfc
test-lz4-frame-concatenation: lz4 datagen
	@echo "\n ---- test frame concatenation ----"
	@echo -n > $(FPREFIX)-empty
	@echo hi > $(FPREFIX)-nonempty
	$(CAT) $(FPREFIX)-nonempty $(FPREFIX)-empty $(FPREFIX)-nonempty > $(FPREFIX)-src
	$(LZ4) -zq $(FPREFIX)-empty -c > $(FPREFIX)-empty.lz4
	$(LZ4) -zq $(FPREFIX)-nonempty -c > $(FPREFIX)-nonempty.lz4
	$(CAT) $(FPREFIX)-nonempty.lz4 $(FPREFIX)-empty.lz4 $(FPREFIX)-nonempty.lz4 > $(FPREFIX)-concat.lz4
	$(LZ4) -d $(FPREFIX)-concat.lz4 -c > $(FPREFIX)-result
	$(CMP) $(FPREFIX)-src $(FPREFIX)-result
	@$(RM) $(FPREFIX)*
	@echo frame concatenation test completed

test-lz4-multiple: FPREFIX = tmp-tml
test-lz4-multiple: lz4 datagen
	@echo "\n ---- test multiple files ----"
	@$(DATAGEN) -s1        > $(FPREFIX)1 2> $(VOID)
	@$(DATAGEN) -s2 -g100K > $(FPREFIX)2 2> $(VOID)
	@$(DATAGEN) -s3 -g200K > $(FPREFIX)3 2> $(VOID)
	# compress multiple files : one .lz4 per source file
	$(LZ4) -f -m $(FPREFIX)*
	test -f $(FPREFIX)1.lz4
	test -f $(FPREFIX)2.lz4
	test -f $(FPREFIX)3.lz4
	# decompress multiple files : one output file per .lz4
	mv $(FPREFIX)1 $(FPREFIX)1-orig
	mv $(FPREFIX)2 $(FPREFIX)2-orig
	mv $(FPREFIX)3 $(FPREFIX)3-orig
	$(LZ4) -d -f -m $(FPREFIX)*.lz4
	$(CMP) $(FPREFIX)1 $(FPREFIX)1-orig   # must be identical
	$(CMP) $(FPREFIX)2 $(FPREFIX)2-orig
	$(CMP) $(FPREFIX)3 $(FPREFIX)3-orig
	# compress multiple files into stdout
	$(CAT) $(FPREFIX)1.lz4 $(FPREFIX)2.lz4 $(FPREFIX)3.lz4 > $(FPREFIX)-concat1
	$(RM) $(FPREFIX)*.lz4
	$(LZ4) -m $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3 -c > $(FPREFIX)-concat2
	test ! -f $(FPREFIX)1.lz4  # must not create .lz4 artefact
	$(CMP) $(FPREFIX)-concat1 $(FPREFIX)-concat2  # must be equivalent
	# decompress multiple files into stdout
	$(RM) $(FPREFIX)-concat1 $(FPREFIX)-concat2
	$(LZ4) -f -m $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3   # generate .lz4 to decompress
	$(CAT) $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3 > $(FPREFIX)-concat1   # create concatenated reference
	$(RM) $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3
	$(LZ4) -d -m $(FPREFIX)1.lz4 $(FPREFIX)2.lz4 $(FPREFIX)3.lz4 -c > $(FPREFIX)-concat2
	test ! -f $(FPREFIX)1  # must not create file artefact
	$(CMP) $(FPREFIX)-concat1 $(FPREFIX)-concat2  # must be equivalent
	# compress multiple files, one of which is absent (must fail)
	! $(LZ4) -f -m $(FPREFIX)-concat1 notHere $(FPREFIX)-concat2  # must fail : notHere not present
	# test lz4-compressed file
	$(LZ4) -tm $(FPREFIX)-concat1.lz4
	$(LZ4) -tm $(FPREFIX)-concat1.lz4 $(FPREFIX)-concat2.lz4
	# test multiple lz4 files, one of which is absent (must fail)
	! $(LZ4) -tm $(FPREFIX)-concat1.lz4 notHere.lz4 $(FPREFIX)-concat2.lz4
	@$(RM) $(FPREFIX)*

test-lz4-multiple-legacy: FPREFIX = tmp-lml
test-lz4-multiple-legacy: lz4 datagen
	@echo "\n ---- test multiple files (Legacy format) ----"
	@$(DATAGEN) -s1        > $(FPREFIX)1 2> $(VOID)
	@$(DATAGEN) -s2 -g100K > $(FPREFIX)2 2> $(VOID)
	@$(DATAGEN) -s3 -g200K > $(FPREFIX)3 2> $(VOID)
	# compress multiple files using legacy format: one .lz4 per source file
	$(LZ4) -f -l -m $(FPREFIX)*
	test -f $(FPREFIX)1.lz4
	test -f $(FPREFIX)2.lz4
	test -f $(FPREFIX)3.lz4
	# decompress multiple files compressed using legacy format: one output file per .lz4
	mv $(FPREFIX)1 $(FPREFIX)1-orig
	mv $(FPREFIX)2 $(FPREFIX)2-orig
	mv $(FPREFIX)3 $(FPREFIX)3-orig
	$(LZ4) -d -f -m $(FPREFIX)*.lz4
	$(LZ4) -l -d -f -m $(FPREFIX)*.lz4 # -l mustn't impact -d option
	$(CMP) $(FPREFIX)1 $(FPREFIX)1-orig   # must be identical
	$(CMP) $(FPREFIX)2 $(FPREFIX)2-orig
	$(CMP) $(FPREFIX)3 $(FPREFIX)3-orig
	# compress multiple files into stdout using legacy format
	$(CAT) $(FPREFIX)1.lz4 $(FPREFIX)2.lz4 $(FPREFIX)3.lz4 > $(FPREFIX)-concat1
	$(RM) $(FPREFIX)*.lz4
	$(LZ4) -l -m $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3 -c > $(FPREFIX)-concat2
	test ! -f $(FPREFIX)1.lz4  # must not create .lz4 artefact
	$(CMP) $(FPREFIX)-concat1 $(FPREFIX)-concat2  # must be equivalent
	# # # decompress multiple files into stdout using legacy format
	$(RM) $(FPREFIX)-concat1 $(FPREFIX)-concat2
	$(LZ4) -l -f -m $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3   # generate .lz4 to decompress
	$(CAT) $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3 > $(FPREFIX)-concat1   # create concatenated reference
	$(RM) $(FPREFIX)1 $(FPREFIX)2 $(FPREFIX)3
	$(LZ4) -d -m $(FPREFIX)1.lz4 $(FPREFIX)2.lz4 $(FPREFIX)3.lz4 -c > $(FPREFIX)-concat2
	$(LZ4) -d -l -m $(FPREFIX)1.lz4 $(FPREFIX)2.lz4 $(FPREFIX)3.lz4 -c > $(FPREFIX)-concat2 # -l mustn't impact option -d
	test ! -f $(FPREFIX)1  # must not create file artefact
	$(CMP) $(FPREFIX)-concat1 $(FPREFIX)-concat2  # must be equivalent
	# # # compress multiple files, one of which is absent (must fail)
	! $(LZ4) -f -l -m $(FPREFIX)-concat1 notHere-legacy $(FPREFIX)-concat2  # must fail : notHere-legacy not present
	@$(RM) $(FPREFIX)*

SKIPFILE = goldenSamples/skip.bin
test-lz4-skippable: FPREFIX = tmp-lsk
test-lz4-skippable: lz4 datagen
	@echo "\n ---- test lz4 with skippable frames ----"
	$(LZ4) -dc $(SKIPFILE)
	$(LZ4) -dc < $(SKIPFILE)
	cat $(SKIPFILE) | $(LZ4) -dc
	echo "Hello from Valid Frame!\n" | $(LZ4) -c > $(FPREFIX).lz4
	cat $(SKIPFILE) $(FPREFIX).lz4 $(SKIPFILE) | $(LZ4) -dc
	$(RM) $(FPREFIX)*

test-lz4-basic: FPREFIX = tmp-tlb
test-lz4-basic: lz4 datagen unlz4 lz4cat
	@echo "\n ---- test lz4 basic compression/decompression ----"
	$(DATAGEN) -g0       | $(LZ4) -v     | $(LZ4) -t
	$(DATAGEN) -g16KB    | $(LZ4) -9     | $(LZ4) -t
	$(DATAGEN) -g20KB > $(FPREFIX)-dg20k
	$(LZ4) < $(FPREFIX)-dg20k | $(LZ4) -d > $(FPREFIX)-dec
	$(DIFF) -q $(FPREFIX)-dg20k $(FPREFIX)-dec
	$(LZ4) --no-frame-crc < $(FPREFIX)-dg20k | $(LZ4) -d > $(FPREFIX)-dec
	$(DIFF) -q $(FPREFIX)-dg20k $(FPREFIX)-dec
	$(DATAGEN)           | $(LZ4) -BI    | $(LZ4) -t
	$(DATAGEN)           | $(LZ4) --no-crc | $(LZ4) -t
	$(DATAGEN) -g6M -P99 | $(LZ4) -9BD   | $(LZ4) -t
	$(DATAGEN) -g17M     | $(LZ4) -9v    | $(LZ4) -qt
	$(DATAGEN) -g33M     | $(LZ4) --no-frame-crc | $(LZ4) -t
	$(DATAGEN) -g256MB   | $(LZ4) -vqB4D | $(LZ4) -t --no-crc
	@echo "hello world" > $(FPREFIX)-hw
	$(LZ4) --rm -f $(FPREFIX)-hw $(FPREFIX)-hw.lz4
	test ! -f $(FPREFIX)-hw                      # must fail (--rm)
	test   -f $(FPREFIX)-hw.lz4
	$(PRGDIR)/lz4cat $(FPREFIX)-hw.lz4 | $(GREP) "hello world"
	$(PRGDIR)/unlz4 --rm $(FPREFIX)-hw.lz4 $(FPREFIX)-hw
	test   -f $(FPREFIX)-hw
	test ! -f $(FPREFIX)-hw.lz4                  # must fail (--rm)
	test ! -f $(FPREFIX)-hw.lz4.lz4              # must fail (unlz4)
	$(PRGDIR)/lz4cat $(FPREFIX)-hw               # pass-through mode
	test   -f $(FPREFIX)-hw
	test ! -f $(FPREFIX)-hw.lz4                  # must fail (lz4cat)
	$(LZ4) $(FPREFIX)-hw $(FPREFIX)-hw.lz4          # creates $(FPREFIX)-hw.lz4
	$(PRGDIR)/lz4cat < $(FPREFIX)-hw.lz4 > $(FPREFIX)3  # checks lz4cat works with stdin (#285)
	$(DIFF) -q $(FPREFIX)-hw $(FPREFIX)3
	$(PRGDIR)/lz4cat < $(FPREFIX)-hw > $(FPREFIX)2      # checks lz4cat works in pass-through mode
	$(DIFF) -q $(FPREFIX)-hw $(FPREFIX)2
	cp $(FPREFIX)-hw ./-d
	$(LZ4) --rm -- -d -d.lz4               # compresses ./d into ./-d.lz4
	test   -f ./-d.lz4
	test ! -f ./-d
	mv ./-d.lz4 ./-z
	$(LZ4) -d --rm -- -z $(FPREFIX)4          # uncompresses ./-z into $(FPREFIX)4
	test ! -f ./-z
	$(DIFF) -q $(FPREFIX)-hw $(FPREFIX)4
	! $(LZ4) $(FPREFIX)2 $(FPREFIX)3 $(FPREFIX)4    # must fail: refuse to handle 3+ file names
	$(LZ4) -f $(FPREFIX)-hw                   # create $(FPREFIX)-hw.lz4, for next tests
	$(LZ4) --list $(FPREFIX)-hw.lz4           # test --list on valid single-frame file
	$(LZ4) --list < $(FPREFIX)-hw.lz4         # test --list from stdin (file only)
	$(CAT) $(FPREFIX)-hw >> $(FPREFIX)-hw.lz4
	! $(LZ4) -f $(FPREFIX)-hw.lz4             # uncompress valid frame followed by invalid data (must fail now)
	$(LZ4) -BX $(FPREFIX)-hw -c -q | $(LZ4) -tv  # test block checksum
	# $(DATAGEN) -g20KB generates the same file every single time
	# cannot save output of $(DATAGEN) -g20KB as input file to lz4 because the following shell commands are run before $(DATAGEN) -g20KB
	test "$(shell $(DATAGEN) -g20KB | $(LZ4) -c --fast | wc -c)" -lt "$(shell $(DATAGEN) -g20KB | $(LZ4) -c --fast=9 | wc -c)" # -1 vs -9
	test "$(shell $(DATAGEN) -g20KB | $(LZ4) -c -1 | wc -c)" -lt "$(shell $(DATAGEN) -g20KB| $(LZ4) -c --fast=1 | wc -c)" # 1 vs -1
	test "$(shell $(DATAGEN) -g20KB | $(LZ4) -c --fast=1 | wc -c)" -eq "$(shell $(DATAGEN) -g20KB| $(LZ4) -c --fast| wc -c)" # checks default fast compression is -1
	! $(LZ4) -c --fast=0 $(FPREFIX)-dg20K # lz4 should fail when fast=0
	! $(LZ4) -c --fast=-1 $(FPREFIX)-dg20K # lz4 should fail when fast=-1
	# High --fast values can result in out-of-bound dereferences #876
	$(DATAGEN) -g1M | $(LZ4) -c --fast=999999999 > /dev/null
	# Test for #596
	@echo "TEST" > $(FPREFIX)-test
	$(LZ4) -m $(FPREFIX)-test
	$(LZ4) $(FPREFIX)-test.lz4 $(FPREFIX)-test2
	$(DIFF) -q $(FPREFIX)-test $(FPREFIX)-test2
	@$(RM) $(FPREFIX)*


test-lz4-dict: FPREFIX = tmp-dict
test-lz4-dict: lz4 datagen
	@echo "\n ---- test lz4 compression/decompression with dictionary ----"
	$(DATAGEN) -g16KB > $(FPREFIX)
	$(DATAGEN) -g32KB > $(FPREFIX)-sample-32k
	< $(FPREFIX)-sample-32k $(LZ4) -D $(FPREFIX) | $(LZ4) -dD $(FPREFIX) | diff - $(FPREFIX)-sample-32k
	$(DATAGEN) -g128MB > $(FPREFIX)-sample-128m
	< $(FPREFIX)-sample-128m $(LZ4) -D $(FPREFIX) | $(LZ4) -dD $(FPREFIX) | diff - $(FPREFIX)-sample-128m
	touch $(FPREFIX)-sample-0
	< $(FPREFIX)-sample-0 $(LZ4) -D $(FPREFIX) | $(LZ4) -dD $(FPREFIX) | diff - $(FPREFIX)-sample-0

	< $(FPREFIX)-sample-32k $(LZ4) -D $(FPREFIX)-sample-0 | $(LZ4) -dD $(FPREFIX)-sample-0 | diff - $(FPREFIX)-sample-32k
	< $(FPREFIX)-sample-0 $(LZ4) -D $(FPREFIX)-sample-0 | $(LZ4) -dD $(FPREFIX)-sample-0 | diff - $(FPREFIX)-sample-0

	@echo "\n ---- test lz4 dictionary loading ----"
	$(DATAGEN) -g128KB > $(FPREFIX)-data-128KB
	set -e; \
	for l in 0 1 4 128 32767 32768 32769 65535 65536 65537 98303 98304 98305 131071 131072 131073; do \
		$(DATAGEN) -g$$l > $(FPREFIX)-$$l; \
		$(DD) if=$(FPREFIX)-$$l of=$(FPREFIX)-$$l-tail bs=1 count=65536 skip=$$((l > 65536 ? l - 65536 : 0)); \
		< $(FPREFIX)-$$l      $(LZ4) -D stdin $(FPREFIX)-data-128KB -c | $(LZ4) -dD $(FPREFIX)-$$l-tail | $(DIFF) - $(FPREFIX)-data-128KB; \
		< $(FPREFIX)-$$l-tail $(LZ4) -D stdin $(FPREFIX)-data-128KB -c | $(LZ4) -dD $(FPREFIX)-$$l      | $(DIFF) - $(FPREFIX)-data-128KB; \
	done
	@$(RM) $(FPREFIX)*

test-lz4hc-hugefile: lz4 datagen
	@echo "\n ---- test HC compression/decompression of huge files ----"
	$(DATAGEN) -g4200MB | $(LZ4) -v3BD | $(LZ4) -qt

test-lz4-fast-hugefile: FPREFIX = tmp-lfh
test-lz4-fast-hugefile: lz4 datagen
	@echo "\n ---- test huge files compression/decompression ----"
	$(DATAGEN) -g6GB    | $(LZ4) -vB5D | $(LZ4) -qt
	# test large file size [2-4] GB
	@$(DATAGEN) -g3G -P100 | $(LZ4) -vv | $(LZ4) --decompress --force --sparse - $(FPREFIX)1
	@ls -ls $(FPREFIX)1
	@$(DATAGEN) -g3G -P100 | $(LZ4) --quiet --content-size | $(LZ4) --verbose --decompress --force --sparse - $(FPREFIX)2
	@ls -ls $(FPREFIX)2
	$(DIFF) -s $(FPREFIX)1 $(FPREFIX)2
	@$(RM) $(FPREFIX)*

test-lz4-hugefile: test-lz4-fast-hugefile test-lz4hc-hugefile

test-lz4-testmode: FPREFIX = tmp-ltm
test-lz4-testmode: lz4 datagen
	@echo "\n ---- bench mode ----"
	$(LZ4) -bi0
	$(DATAGEN) > $(FPREFIX)
	$(LZ4) -f $(FPREFIX) -c > $(FPREFIX).lz4
	$(LZ4) -bdi0 $(FPREFIX).lz4 # test benchmark decode-only mode
	$(LZ4) -bdi0 --no-crc $(FPREFIX).lz4 # test benchmark decode-only mode
	@echo "\n ---- test mode ----"
	! $(DATAGEN) | $(LZ4) -t
	! $(DATAGEN) | $(LZ4) -tf
	@echo "\n ---- pass-through mode ----"
	@echo "Why hello there " > $(FPREFIX)2.lz4
	! $(LZ4) -f $(FPREFIX)2.lz4 > $(VOID)
	! $(DATAGEN) | $(LZ4) -dc  > $(VOID)
	! $(DATAGEN) | $(LZ4) -df > $(VOID)
	$(DATAGEN) | $(LZ4) -dcf > $(VOID)
	@echo "Hello World !" > $(FPREFIX)1
	$(LZ4) -dcf $(FPREFIX)1
	@echo "from underground..." > $(FPREFIX)2
	$(LZ4) -dcfm $(FPREFIX)1 $(FPREFIX)2
	@echo "\n ---- non-existing source (must fail cleanly) ----"
	! $(LZ4)     file-does-not-exist
	! $(LZ4) -f  file-does-not-exist
	! $(LZ4) -t  file-does-not-exist
	! $(LZ4) -fm file1-dne file2-dne
	@$(RM) $(FPREFIX)*

test-lz4-opt-parser: lz4 datagen
	@echo "\n ---- test opt-parser ----"
	$(DATAGEN) -g16KB      | $(LZ4) -12      | $(LZ4) -t
	$(DATAGEN) -P10        | $(LZ4) -12B4    | $(LZ4) -t
	$(DATAGEN) -g256K      | $(LZ4) -12B4D   | $(LZ4) -t
	$(DATAGEN) -g512K -P25 | $(LZ4) -12BD    | $(LZ4) -t
	$(DATAGEN) -g1M        | $(LZ4) -12B5    | $(LZ4) -t
	$(DATAGEN) -g1M -s2    | $(LZ4) -12B4D   | $(LZ4) -t
	$(DATAGEN) -g2M -P99   | $(LZ4) -11B4D   | $(LZ4) -t
	$(DATAGEN) -g4M        | $(LZ4) -11vq    | $(LZ4) -qt
	$(DATAGEN) -g8M        | $(LZ4) -11B4    | $(LZ4) -t
	$(DATAGEN) -g16M -P90  | $(LZ4) -11B5    | $(LZ4) -t
	$(DATAGEN) -g32M -P10  | $(LZ4) -11B5D   | $(LZ4) -t

test-lz4-essentials : lz4 datagen test-lz4-basic test-lz4-multiple test-lz4-multiple-legacy \
                      test-lz4-frame-concatenation test-lz4-testmode \
                      test-lz4-contentSize test-lz4-dict

test-lz4: lz4 datagen test-lz4-essentials test-lz4-opt-parser \
          test-lz4-sparse test-lz4-hugefile test-lz4-dict \
          test-lz4-skippable

test-lz4c: LZ4C = $(LZ4)c
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

test-frametest32: CFLAGS += -m32
test-frametest32: test-frametest

VALGRIND = valgrind --leak-check=yes --error-exitcode=1
test-mem: FPREFIX = tmp-tvm
test-mem: lz4 datagen fuzzer frametest fullbench
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
	$(RM) $(FPREFIX)*
	$(VALGRIND) ./fuzzer -i64 -t1
	$(VALGRIND) ./frametest -i256

test-mem32: lz4c32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary...

test-decompress-partial : decompress-partial decompress-partial-usingDict
	@echo "\n ---- test decompress-partial ----"
	./decompress-partial$(EXT)
	@echo "\n ---- test decompress-partial-usingDict ----"
	./decompress-partial-usingDict$(EXT)

test-freestanding: freestanding
	@echo "\n ---- test freestanding ----"
	./freestanding$(EXT)
	-strace ./freestanding$(EXT)
	-ltrace ./freestanding$(EXT)

endif
