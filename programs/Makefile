# ##########################################################################
# LZ4 programs - Makefile
# Copyright (C) Yann Collet 2011-2014
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
#  - LZ4 source repository : http://code.google.com/p/lz4/
#  - LZ4 forum froup : https://groups.google.com/forum/#!forum/lz4c
# ##########################################################################
# lz4 : Command Line Utility, supporting gzip-like arguments
# lz4c  : CLU, supporting also legacy lz4demo arguments
# lz4c32: Same as lz4c, but forced to compile in 32-bits mode
# fuzzer  : Test tool, to check lz4 integrity on target platform
# fuzzer32: Same as fuzzer, but forced to compile in 32-bits mode
# fullbench  : Precisely measure speed for each LZ4 function variant
# fullbench32: Same as fullbench, but forced to compile in 32-bits mode
# ##########################################################################

RELEASE=r124

DESTDIR?=
PREFIX ?= /usr
CC     := $(CC)
CFLAGS ?= -O3
CFLAGS += -std=c99 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -DLZ4_VERSION=\"$(RELEASE)\"
FLAGS   = -I.. $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1
LZ4DIR=..

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
ifeq ($(LZ4_TRAVIS_CI_ENV),-lz4)
TRAVIS_TARGET=test-lz4
else ifeq ($(LZ4_TRAVIS_CI_ENV),-lz4c)
TRAVIS_TARGET=test-lz4c
else ifeq ($(LZ4_TRAVIS_CI_ENV),-lz4c32)
TRAVIS_TARGET=test-lz4c32
else ifeq ($(LZ4_TRAVIS_CI_ENV),-fullbench)
TRAVIS_TARGET=test-fullbench
else ifeq ($(LZ4_TRAVIS_CI_ENV),-fullbench32)
TRAVIS_TARGET=test-fullbench32
else ifeq ($(LZ4_TRAVIS_CI_ENV),-fuzzer)
TRAVIS_TARGET=test-fuzzer
else ifeq ($(LZ4_TRAVIS_CI_ENV),-fuzzer32)
TRAVIS_TARGET=test-fuzzer32
else ifeq ($(LZ4_TRAVIS_CI_ENV),-frametest)
TRAVIS_TARGET=test-frametest
else ifeq ($(LZ4_TRAVIS_CI_ENV),-frametest32)
TRAVIS_TARGET=test-frametest32
else ifeq ($(LZ4_TRAVIS_CI_ENV),-mem)
TRAVIS_TARGET=test-mem
else
TRAVIS_TARGET=test
endif


default: lz4 lz4c

all: lz4 lz4c lz4c32 fullbench fullbench32 fuzzer fuzzer32 frametest frametest32 datagen

lz4: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC)      $(FLAGS) -DDISABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

lz4c  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

lz4c32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c
	$(CC) -m32 $(FLAGS) $^ -o $@$(EXT)

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

datagen : datagen.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)


clean:
	@rm -f core *.o \
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
	@install -m 755 lz4   $(DESTDIR)$(BINDIR)/lz4
	@ln -sf lz4 $(DESTDIR)$(BINDIR)/lz4cat
	@install -m 755 lz4c  $(DESTDIR)$(BINDIR)/lz4c
	@echo Installing man pages
	@install -m 644 lz4.1 $(DESTDIR)$(MANDIR)/lz4.1
	@install -m 644 lz4c.1 $(DESTDIR)$(MANDIR)/lz4c.1
	@install -m 644 lz4cat.1 $(DESTDIR)$(MANDIR)/lz4cat.1
	@echo lz4 installation completed

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lz4cat
	[ -x $(DESTDIR)$(BINDIR)/lz4 ] && rm -f $(DESTDIR)$(BINDIR)/lz4
	[ -x $(DESTDIR)$(BINDIR)/lz4c ] && rm -f $(DESTDIR)$(BINDIR)/lz4c
	[ -f $(DESTDIR)$(MANDIR)/lz4.1 ] && rm -f $(DESTDIR)$(MANDIR)/lz4.1
	[ -f $(DESTDIR)$(MANDIR)/lz4c.1 ] && rm -f $(DESTDIR)$(MANDIR)/lz4c.1
	[ -f $(DESTDIR)$(MANDIR)/lz4cat.1 ] && rm -f $(DESTDIR)$(MANDIR)/lz4cat.1
	@echo lz4 successfully uninstalled

test: test-lz4 test-lz4c test-frametest test-fullbench test-fuzzer test-mem

test32: test-lz4c32 test-frametest32 test-fullbench32 test-fuzzer32 test-mem32

test-all: test test32

test-travis: $(TRAVIS_TARGET)

test-lz4: lz4 datagen
	./datagen -g16KB  | ./lz4 -9     | ./lz4 -vdq > $(VOID)
	./datagen         | ./lz4        | ./lz4 -vdq > $(VOID)
	./datagen -g256MB | ./lz4 -vqB4D | ./lz4 -vdq > $(VOID)
	./datagen -g6GB   | ./lz4 -vqB5D | ./lz4 -vdq > $(VOID)

test-lz4c: lz4c datagen

test-lz4c32: lz4 lz4c32 lz4 datagen
	./datagen -g16KB  | ./lz4c32 -9     | ./lz4c32 -vdq > $(VOID)
	./datagen -g16KB  | ./lz4c32 -9     | ./lz4    -vdq > $(VOID)
	./datagen         | ./lz4c32        | ./lz4c32 -vdq > $(VOID)
	./datagen         | ./lz4c32        | ./lz4    -vdq > $(VOID)
	./datagen -g256MB | ./lz4c32 -vqB4D | ./lz4c32 -vdq > $(VOID)
	./datagen -g256MB | ./lz4c32 -vqB4D | ./lz4    -vdq > $(VOID)
	./datagen -g6GB   | ./lz4c32 -vqB5D | ./lz4c32 -vdq > $(VOID)

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
	./datagen -g16KB > tmp
	valgrind --leak-check=yes ./lz4 -9 -BD -f tmp /dev/null
	./datagen -g16MB > tmp
	valgrind --leak-check=yes ./lz4 -9 -B5D -f tmp /dev/null
	./datagen -g256MB > tmp
	valgrind --leak-check=yes ./lz4 -B4D -f tmp /dev/null
	rm tmp
	valgrind --leak-check=yes ./fuzzer -i50 -t0
	valgrind --leak-check=yes ./frametest -i100

test-mem32: lz4c32 datagen
# unfortunately, valgrind doesn't seem to work with non-native binary. If someone knows how to do a valgrind-test on a 32-bits exe with a 64-bits system...

endif
