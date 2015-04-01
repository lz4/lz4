# ##########################################################################
# LZ4 programs - Makefile
# Copyright (C) Yann Collet 2011-2015
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
#  - LZ4 source repository : https://github.com/Cyan4973/lz4
#  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
# ##########################################################################
# lz4 : Command Line Utility, supporting gzip-like arguments
# lz4c  : CLU, supporting also legacy lz4demo arguments
# lz4c32: Same as lz4c, but forced to compile in 32-bits mode
# fuzzer  : Test tool, to check lz4 integrity on target platform
# fuzzer32: Same as fuzzer, but forced to compile in 32-bits mode
# frametest  : Test tool, to check lz4frame integrity on target platform
# frametest32: Same as frametest, but forced to compile in 32-bits mode
# fullbench  : Precisely measure speed for each LZ4 function variant
# fullbench32: Same as fullbench, but forced to compile in 32-bits mode
# datagen : generates synthetic data samples for tests & benchmarks
# ##########################################################################

RELEASE?= r129

DESTDIR?=
PREFIX ?= /usr/local
CFLAGS ?= -O3
CFLAGS += -std=c99 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -pedantic -DLZ4_VERSION=\"$(RELEASE)\"
FLAGS   = -I../lib $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1
LZ4DIR=../lib

TEST_FILES = COPYING
TEST_TARGETS=test-native


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
VOID = nul
else
EXT =
VOID = /dev/null
endif


# Select test target for Travis CI's Build Matrix
TRAVIS_TARGET=$(LZ4_TRAVIS_CI_ENV)


default: lz4

m32: lz4c32 fullbench32 fuzzer32 frametest32

bins: lz4 lz4c fullbench fuzzer frametest datagen

all: bins m32

lz4: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

lz4c  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC)      $(FLAGS) -DENABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

lz4c32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC) -m32 $(FLAGS) -DENABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

fullbench  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c fullbench.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

fullbench32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c fullbench.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

fuzzer  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c fuzzer.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

fuzzer32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c fuzzer.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

frametest: $(LZ4DIR)/lz4frame.c $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c frametest.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

frametest32: $(LZ4DIR)/lz4frame.c $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c frametest.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

datagen : datagen.c datagencli.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

clean:
	@rm -f core *.o *.test \
        lz4$(EXT) lz4c$(EXT) lz4c32$(EXT) \
        fullbench$(EXT) fullbench32$(EXT) \
        fuzzer$(EXT) fuzzer32$(EXT) \
        frametest$(EXT) frametest32$(EXT) \
        datagen$(EXT)
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD and Hurd targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU))

install: lz4 lz4c
	@echo Installing binaries
	@install -d -m 755 $(DESTDIR)$(BINDIR)/ $(DESTDIR)$(MANDIR)/
	@install -m 755 lz4 $(DESTDIR)$(BINDIR)/lz4
	@ln -sf lz4 $(DESTDIR)$(BINDIR)/lz4cat
	@ln -sf lz4 $(DESTDIR)$(BINDIR)/unlz4
	@install -m 755 lz4c $(DESTDIR)$(BINDIR)/lz4c
	@echo Installing man pages
	@install -m 644 lz4.1 $(DESTDIR)$(MANDIR)/lz4.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/lz4c.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/lz4cat.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/unlz4.1
	@echo lz4 installation completed

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lz4cat
	rm -f $(DESTDIR)$(BINDIR)/unlz4
	[ -x $(DESTDIR)$(BINDIR)/lz4 ] && rm -f $(DESTDIR)$(BINDIR)/lz4
	[ -x $(DESTDIR)$(BINDIR)/lz4c ] && rm -f $(DESTDIR)$(BINDIR)/lz4c
	[ -f $(DESTDIR)$(MANDIR)/lz4.1 ] && rm -f $(DESTDIR)$(MANDIR)/lz4.1
	rm -f $(DESTDIR)$(MANDIR)/lz4c.1
	rm -f $(DESTDIR)$(MANDIR)/lz4cat.1
	rm -f $(DESTDIR)$(MANDIR)/unlz4.1
	@echo lz4 programs successfully uninstalled

test: test-lz4 test-lz4c test-frametest test-fullbench test-fuzzer test-mem

test32: test-lz4c32 test-frametest32 test-fullbench32 test-fuzzer32 test-mem32

test-all: test test32

test-travis: $(TRAVIS_TARGET)

test-lz4-sparse: lz4 datagen
	@echo ---- test sparse file support ----
	./datagen -g50M -P100 | ./lz4 -B4D | ./lz4 -dv --sparse > tmpB4
	./datagen -g50M -P100 | ./lz4 -B5D | ./lz4 -dv --sparse > tmpB5
	./datagen -g50M -P100 | ./lz4 -B6D | ./lz4 -dv --sparse > tmpB6
	./datagen -g50M -P100 | ./lz4 -B7D | ./lz4 -dv --sparse > tmpB7
	ls -ls tmp*
	./datagen -g50M -P100 | diff -s - tmpB4
	./datagen -g50M -P100 | diff -s - tmpB5
	./datagen -g50M -P100 | diff -s - tmpB6
	./datagen -g50M -P100 | diff -s - tmpB7
	./datagen -s1 -g1200007 -P100 | ./lz4 | ./lz4 -dv --sparse > tmpOdd   # Odd size file (to not finish on an exact nb of blocks)
	./datagen -s1 -g1200007 -P100 | diff -s - tmpOdd
	ls -ls tmpOdd
	@rm tmp*

test-lz4-contentSize: lz4 datagen
	@echo ---- test original size support ----
	./datagen -g15M > tmp
	./lz4 -v tmp | ./lz4 -t
	./lz4 -v --content-size tmp | ./lz4 -d > tmp2
	diff -s tmp tmp2
	@rm tmp*

test-lz4-frame-concatenation: lz4 datagen
	@echo ---- test frame concatenation ----
	@echo -n > empty.test
	@echo hi > nonempty.test
	cat nonempty.test empty.test nonempty.test > orig.test
	@./lz4 -zq empty.test > empty.lz4.test
	@./lz4 -zq nonempty.test > nonempty.lz4.test
	cat nonempty.lz4.test empty.lz4.test nonempty.lz4.test > concat.lz4.test
	./lz4 -d concat.lz4.test > result.test
	sdiff orig.test result.test
	@rm *.test
	@echo frame concatenation test completed

test-lz4: lz4 datagen test-lz4-sparse test-lz4-contentSize test-lz4-frame-concatenation
	@echo ---- test lz4 basic compression/decompression ----
	./datagen -g0     | ./lz4 -v     | ./lz4 -t
	./datagen -g16KB  | ./lz4 -9     | ./lz4 -t
	./datagen         | ./lz4        | ./lz4 -t
	./datagen -g6M -P99 | ./lz4 -9BD | ./lz4 -t
	./datagen -g17M   | ./lz4 -9v    | ./lz4 -tq
	./datagen -g33M   | ./lz4 --no-frame-crc | ./lz4 -t
	./datagen -g256MB | ./lz4 -vqB4D | ./lz4 -t
	./datagen -g6GB   | ./lz4 -vqB5D | ./lz4 -t
	./datagen -g6GB   | ./lz4 -vq9BD | ./lz4 -t
	@echo ---- test multiple input files ----
	@./datagen -s1 > file1
	@./datagen -s2 > file2
	@./datagen -s3 > file3
	./lz4 -f -m file1 file2 file3
	ls -l file*
	@rm file1 file2 file3 file1.lz4 file2.lz4 file3.lz4
	@echo ---- test pass-through ----
	./datagen | ./lz4 -tf

test-lz4c: lz4c datagen
	./datagen -g256MB | ./lz4c -l -v    | ./lz4c   -t

test-lz4c32: lz4 lz4c32 datagen
	./datagen -g16KB  | ./lz4c32 -9     | ./lz4c32 -t
	./datagen -g16KB  | ./lz4c32 -9     | ./lz4    -t
	./datagen         | ./lz4c32        | ./lz4c32 -t
	./datagen         | ./lz4c32        | ./lz4    -t
	./datagen -g256MB | ./lz4c32 -vqB4D | ./lz4c32 -t
	./datagen -g256MB | ./lz4c32 -vqB4D | ./lz4    -t
	./datagen -g6GB   | ./lz4c32 -vqB5D | ./lz4c32 -t
	./datagen -g6GB   | ./lz4c32 -vq9BD | ./lz4    -t

test-fullbench: fullbench
	./fullbench --no-prompt $(TEST_FILES)

test-fullbench32: fullbench32
	./fullbench32 --no-prompt $(TEST_FILES)

test-fuzzer: fuzzer
	./fuzzer

test-fuzzer32: fuzzer32
	./fuzzer32

test-frametest: frametest
	./frametest

test-frametest32: frametest32
	./frametest32

test-mem: lz4 datagen fuzzer frametest
	valgrind --leak-check=yes ./datagen -g50M > $(VOID)
	./datagen -g16KB > tmp
	valgrind --leak-check=yes ./lz4 -9 -BD -f tmp $(VOID)
	./datagen -g16MB > tmp
	valgrind --leak-check=yes ./lz4 -9 -B5D -f tmp tmp2
	valgrind --leak-check=yes ./lz4 -t tmp2
	./datagen -g256MB > tmp
	valgrind --leak-check=yes ./lz4 -B4D -f -vq tmp $(VOID)
	rm tmp*
	valgrind --leak-check=yes ./fuzzer -i64 -t1
	valgrind --leak-check=yes ./frametest -i256

test-mem32: lz4c32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary. If someone knows how to do a valgrind-test on a 32-bits exe with a 64-bits system...

endif
