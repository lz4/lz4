# ##########################################################################
# LZ4 programs - Makefile
# Copyright (C) Yann Collet 2011-2016
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

DESTDIR ?=
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1
LIBDIR  := ../lib
PRGDIR  := ../programs
VOID    := /dev/null
TESTDIR := versionsTest
PYTHON  ?= python3

CFLAGS  ?= -O3 # can select custom optimization flags. For example : CFLAGS=-O2 make
CFLAGS  += -g -Wall -Wextra -Wundef -Wcast-qual -Wcast-align -Wshadow -Wswitch-enum \
           -Wdeclaration-after-statement -Wstrict-prototypes \
           -Wpointer-arith -Wstrict-aliasing=1
CFLAGS  += $(MOREFLAGS)
CPPFLAGS:= -I$(LIBDIR) -I$(PRGDIR) -DXXH_NAMESPACE=LZ4_
FLAGS    = $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif
LZ4     := $(PRGDIR)/lz4$(EXT)


# Default test parameters
TEST_FILES   := COPYING
FUZZER_TIME  := -T3mn
NB_LOOPS     ?= -i1


default: all

all: fullbench fuzzer frametest datagen fasttest

all32: CFLAGS+=-m32
all32: all

lz4:
	$(MAKE) -C $(PRGDIR) clean $@ CFLAGS="$(CFLAGS)"

lz4c:
	$(MAKE) -C $(PRGDIR) clean $@ CFLAGS="$(CFLAGS)"

lz4c32:   # create a 32-bits version for 32/64 interop tests
	$(MAKE) -C $(PRGDIR) clean $@ CFLAGS="-m32 $(CFLAGS)"
	cp $(LZ4) $(LZ4)c32

fullbench  : $(LIBDIR)/lz4.o $(LIBDIR)/lz4hc.o $(LIBDIR)/lz4frame.o $(LIBDIR)/xxhash.o fullbench.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

fullbench-lib: fullbench.c $(LIBDIR)/xxhash.c
	$(MAKE) -C $(LIBDIR) liblz4.a
	$(CC) $(FLAGS) $^ -o $@$(EXT) $(LIBDIR)/liblz4.a

fullbench-dll: fullbench.c $(LIBDIR)/xxhash.c
	$(MAKE) -C $(LIBDIR) liblz4
	$(CC) $(FLAGS) $^ -o $@$(EXT) -DLZ4_DLL_IMPORT=1 $(LIBDIR)/dll/liblz4.dll

fuzzer  : $(LIBDIR)/lz4.o $(LIBDIR)/lz4hc.o $(LIBDIR)/xxhash.o fuzzer.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

frametest: $(LIBDIR)/lz4frame.o $(LIBDIR)/lz4.o $(LIBDIR)/lz4hc.o $(LIBDIR)/xxhash.o frametest.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

fasttest: $(LIBDIR)/lz4.o fasttest.c
	$(CC) $(FLAGS) $^ -o $@$(EXT)

datagen : $(PRGDIR)/datagen.c datagencli.c
	$(CC) $(FLAGS) -I$(PRGDIR) $^ -o $@$(EXT)

clean:
	@$(MAKE) -C $(LIBDIR) $@ > $(VOID)
	@$(MAKE) -C $(PRGDIR) $@ > $(VOID)
	@$(RM) core *.o *.test tmp* \
        fullbench-dll$(EXT) fullbench-lib$(EXT) \
        fullbench$(EXT) fullbench32$(EXT) \
        fuzzer$(EXT) fuzzer32$(EXT) \
        frametest$(EXT) frametest32$(EXT) \
        fasttest$(EXT) datagen$(EXT)
	@rm -fR $(TESTDIR)
	@echo Cleaning completed

.PHONY: versionsTest
versionsTest:
	$(PYTHON) test-lz4-versions.py


#------------------------------------------------------------------------
#make test is validated only for Linux, OSX, kFreeBSD, FreeBSD, Hurd and
#Solaris targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU OpenBSD FreeBSD NetBSD DragonFly SunOS))

MD5:=md5sum
ifneq (,$(filter $(shell uname), Darwin ))
MD5:=md5 -r
endif

DIFF:=diff
ifneq (,$(filter $(shell uname),SunOS))
DIFF:=gdiff
endif


test: test-lz4 test-lz4c test-fasttest test-frametest test-fullbench test-fuzzer

test32: CFLAGS+=-m32
test32: test

test-lz4-sparse: lz4 datagen
	@echo "\n ---- test sparse file support ----"
	./datagen -g5M  -P100 > tmpSrc
	$(LZ4) -B4D tmpSrc | $(LZ4) -dv --sparse > tmpB4
	$(DIFF) -s tmpSrc tmpB4
	$(LZ4) -B5D tmpSrc | $(LZ4) -dv --sparse > tmpB5
	$(DIFF) -s tmpSrc tmpB5
	$(LZ4) -B6D tmpSrc | $(LZ4) -dv --sparse > tmpB6
	$(DIFF) -s tmpSrc tmpB6
	$(LZ4) -B7D tmpSrc | $(LZ4) -dv --sparse > tmpB7
	$(DIFF) -s tmpSrc tmpB7
	$(LZ4) tmpSrc | $(LZ4) -dv --no-sparse > tmpNoSparse
	$(DIFF) -s tmpSrc tmpNoSparse
	ls -ls tmp*
	./datagen -s1 -g1200007 -P100 | $(LZ4) | $(LZ4) -dv --sparse > tmpOdd   # Odd size file (to generate non-full last block)
	./datagen -s1 -g1200007 -P100 | $(DIFF) -s - tmpOdd
	ls -ls tmpOdd
	@$(RM) tmp*
	@echo "\n Compatibility with Console :"
	echo "Hello World 1 !" | $(LZ4) | $(LZ4) -d -c
	echo "Hello World 2 !" | $(LZ4) | $(LZ4) -d | cat
	echo "Hello World 3 !" | $(LZ4) --no-frame-crc | $(LZ4) -d -c
	@echo "\n Compatibility with Append :"
	./datagen -P100 -g1M > tmp1M
	cat tmp1M tmp1M > tmp2M
	$(LZ4) -B5 -v tmp1M tmpC
	$(LZ4) -d -v tmpC tmpR
	$(LZ4) -d -v tmpC >> tmpR
	ls -ls tmp*
	$(DIFF) tmp2M tmpR
	@$(RM) tmp*

test-lz4-contentSize: lz4 datagen
	@echo "\n ---- test original size support ----"
	./datagen -g15M > tmp
	$(LZ4) -v tmp | $(LZ4) -t
	$(LZ4) -v --content-size tmp | $(LZ4) -d > tmp2
	$(DIFF) -s tmp tmp2
	# test large size [2-4] GB
	@./datagen -g3G -P100 | $(LZ4) -vv | $(LZ4) --decompress --force --sparse - tmp
	@ls -ls tmp
	@./datagen -g3G -P100 | $(LZ4) --quiet --content-size | $(LZ4) --verbose --decompress --force --sparse - tmp2
	@ls -ls tmp2
	$(DIFF) -s tmp tmp2
	@$(RM) tmp*

test-lz4-frame-concatenation: lz4 datagen
	@echo "\n ---- test frame concatenation ----"
	@echo -n > empty.test
	@echo hi > nonempty.test
	cat nonempty.test empty.test nonempty.test > orig.test
	@$(LZ4) -zq empty.test > empty.lz4.test
	@$(LZ4) -zq nonempty.test > nonempty.lz4.test
	cat nonempty.lz4.test empty.lz4.test nonempty.lz4.test > concat.lz4.test
	$(LZ4) -d concat.lz4.test > result.test
	sdiff orig.test result.test
	@$(RM) *.test
	@echo frame concatenation test completed

test-lz4-multiple: lz4 datagen
	@echo "\n ---- test multiple files ----"
	@./datagen -s1        > tmp1 2> $(VOID)
	@./datagen -s2 -g100K > tmp2 2> $(VOID)
	@./datagen -s3 -g1M   > tmp3 2> $(VOID)
	$(LZ4) -f -m tmp*
	ls -ls tmp*
	@$(RM) tmp1 tmp2 tmp3
	$(LZ4) -df -m *.lz4
	ls -ls tmp*
	$(LZ4) -f -m tmp1 notHere tmp2; echo $$?
	@$(RM) tmp*

unlz4:
	@$(MAKE) -C $(PRGDIR) $@ CFLAGS="$(CFLAGS)"

lz4cat:
	@$(MAKE) -C $(PRGDIR) $@ CFLAGS="$(CFLAGS)"

test-lz4-basic: lz4 datagen unlz4 lz4cat
	@echo "\n ---- test lz4 basic compression/decompression ----"
	./datagen -g0     | $(LZ4) -v     | $(LZ4) -t
	./datagen -g16KB  | $(LZ4) -9     | $(LZ4) -t
	./datagen -g20KB > tmpSrc
	$(LZ4) < tmpSrc   | $(LZ4) -d > tmpRes
	$(DIFF) -q tmpSrc tmpRes
	$(LZ4) --no-frame-crc < tmpSrc | $(LZ4) -d > tmpRes
	$(DIFF) -q tmpSrc tmpRes
	./datagen         | $(LZ4)        | $(LZ4) -t
	./datagen -g6M -P99 | $(LZ4) -9BD | $(LZ4) -t
	./datagen -g17M   | $(LZ4) -9v    | $(LZ4) -qt
	./datagen -g33M   | $(LZ4) --no-frame-crc | $(LZ4) -t
	./datagen -g256MB | $(LZ4) -vqB4D | $(LZ4) -t
	@echo "hello world" > tmp
	$(LZ4) --rm -f tmp
	ls -ls tmp         && false || true   # must fail (--rm)
	ls -ls tmp.lz4
	$(PRGDIR)/lz4cat tmp.lz4              # must display hello world
	ls -ls tmp.lz4
	$(PRGDIR)/unlz4 --rm tmp.lz4
	ls -ls tmp
	ls -ls tmp.lz4     && false || true   # must fail (--rm)
	ls -ls tmp.lz4.lz4 && false || true   # must fail (unlz4)
	$(PRGDIR)/lz4cat tmp                  # pass-through mode
	ls -ls tmp
	ls -ls tmp.lz4     && false || true   # must fail (lz4cat)
	$(LZ4) tmp                         # creates tmp.lz4
	$(PRGDIR)/lz4cat < tmp.lz4 > tmp3  # checks lz4cat works with stdin (#285)
	$(DIFF) -q tmp tmp3
	$(PRGDIR)/lz4cat < tmp > tmp2      # checks lz4cat works with stdin (#285)
	$(DIFF) -q tmp tmp2
	@$(RM) tmp*

test-lz4-hugefile: lz4 datagen
	@echo "\n ---- test huge files compression/decompression ----"
	./datagen -g6GB   | $(LZ4) -vB5D  | $(LZ4) -qt
	./datagen -g6GB   | $(LZ4) -v5BD  | $(LZ4) -qt
	@$(RM) tmp*

test-lz4-testmode: lz4 datagen
	@echo "\n ---- bench mode ----"
	$(LZ4) -bi1
	@echo "\n ---- test mode ----"
	./datagen | $(LZ4) -t             && false || true
	./datagen | $(LZ4) -tf            && false || true
	@echo "\n ---- pass-through mode ----"
	./datagen | $(LZ4) -d  > $(VOID)  && false || true
	./datagen | $(LZ4) -df > $(VOID)
	@echo "Hello World !" > tmp1
	$(LZ4) -dcf tmp1
	@echo "from underground..." > tmp2
	$(LZ4) -dcfm tmp1 tmp2
	@echo "\n ---- test cli ----"
	$(LZ4)     file-does-not-exist    && false || true
	$(LZ4) -f  file-does-not-exist    && false || true
	$(LZ4) -fm file1-dne file2-dne    && false || true
	$(LZ4) -fm file1-dne file2-dne    && false || true

test-lz4-opt-parser: lz4 datagen
	@echo "\n ---- test opt-parser ----"
	./datagen -g16KB      | $(LZ4) -12      | $(LZ4) -t
	./datagen -P10        | $(LZ4) -12B4    | $(LZ4) -t
	./datagen -g256K      | $(LZ4) -12B4D   | $(LZ4) -t
	./datagen -g512K -P25 | $(LZ4) -12BD    | $(LZ4) -t
	./datagen -g1M        | $(LZ4) -12B5    | $(LZ4) -t
	./datagen -g2M -P99   | $(LZ4) -11B4D   | $(LZ4) -t
	./datagen -g4M        | $(LZ4) -11vq    | $(LZ4) -qt
	./datagen -g8M        | $(LZ4) -11B4    | $(LZ4) -t
	./datagen -g16M -P90  | $(LZ4) -11B5    | $(LZ4) -t
	./datagen -g32M -P10  | $(LZ4) -11B5D   | $(LZ4) -t

test-lz4: lz4 datagen test-lz4-opt-parser test-lz4-basic test-lz4-multiple test-lz4-sparse \
          test-lz4-frame-concatenation test-lz4-testmode test-lz4-contentSize \
          test-lz4-hugefile

test-lz4c: lz4c datagen
	@echo "\n ---- test lz4c version ----"
	./datagen -g256MB | $(LZ4)c -l -v    | $(LZ4)c   -t

test-lz4c32: CFLAGS+=-m32
test-lz4c32: test-lz4

test-interop-32-64: lz4 lz4c32 datagen
	@echo "\n ---- test interoperability 32-bits -vs- 64 bits ----"
	./datagen -g16KB  | $(LZ4)c32 -9     | $(LZ4)    -t
	./datagen -P10    | $(LZ4)    -9B4   | $(LZ4)c32 -t
	./datagen         | $(LZ4)c32        | $(LZ4)    -t
	./datagen -g1M    | $(LZ4)    -3B5   | $(LZ4)c32 -t
	./datagen -g256MB | $(LZ4)c32 -vqB4D | $(LZ4)    -qt
	./datagen -g1G -P90 | $(LZ4)         | $(LZ4)c32 -t
	./datagen -g6GB   | $(LZ4)c32 -vq9BD | $(LZ4)    -qt

test-lz4c32-basic: lz4c32 datagen
	@echo "\n ---- test lz4c32 32-bits version ----"
	./datagen -g16KB  | $(LZ4)c32 -9     | $(LZ4)c32 -t
	./datagen         | $(LZ4)c32        | $(LZ4)c32 -t
	./datagen -g256MB | $(LZ4)c32 -vqB4D | $(LZ4)c32 -qt
	./datagen -g6GB   | $(LZ4)c32 -vqB5D | $(LZ4)c32 -qt

test-platform:
	@echo "\n ---- test lz4 $(QEMU_SYS) platform ----"
	$(QEMU_SYS) ./datagen -g16KB  | $(QEMU_SYS) $(LZ4) -9     | $(QEMU_SYS) $(LZ4) -t
	$(QEMU_SYS) ./datagen         | $(QEMU_SYS) $(LZ4)        | $(QEMU_SYS) $(LZ4) -t
	$(QEMU_SYS) ./datagen -g256MB | $(QEMU_SYS) $(LZ4) -vqB4D | $(QEMU_SYS) $(LZ4) -qt
ifneq ($(QEMU_SYS),qemu-arm-static)
	$(QEMU_SYS) ./datagen -g3GB   | $(QEMU_SYS) $(LZ4) -vqB5D | $(QEMU_SYS) $(LZ4) -qt
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
	./frametest $(FUZZER_TIME)

test-frametest32: CFLAGS += -m32
test-frametest32: test-frametest

test-fasttest: fasttest
	./fasttest

test-mem: lz4 datagen fuzzer frametest fullbench
	@echo "\n ---- valgrind tests : memory analyzer ----"
	valgrind --leak-check=yes --error-exitcode=1 ./datagen -g50M > $(VOID)
	./datagen -g16KB > tmp
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) -9 -BD -f tmp $(VOID)
	./datagen -g16KB -s2 > tmp2
	./datagen -g16KB -s3 > tmp3
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) --force --multiple tmp tmp2 tmp3
	./datagen -g16MB > tmp
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) -9 -B5D -f tmp tmp2
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) -t tmp2
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) -bi1 tmp
	valgrind --leak-check=yes --error-exitcode=1 ./fullbench -i1 tmp tmp2
	./datagen -g256MB > tmp
	valgrind --leak-check=yes --error-exitcode=1 $(LZ4) -B4D -f -vq tmp $(VOID)
	$(RM) tmp*
	valgrind --leak-check=yes --error-exitcode=1 ./fuzzer -i64 -t1
	valgrind --leak-check=yes --error-exitcode=1 ./frametest -i256

test-mem32: lz4c32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary...

endif
