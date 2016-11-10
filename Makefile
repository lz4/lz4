# ################################################################
# LZ4 - Makefile
# Copyright (C) Yann Collet 2011-2015
# All rights reserved.
#
# BSD license
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice, this
#   list of conditions and the following disclaimer in the documentation and/or
#   other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
# ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# You can contact the author at :
#  - LZ4 source repository : https://github.com/Cyan4973/lz4
#  - LZ4 forum froup : https://groups.google.com/forum/#!forum/lz4c
# ################################################################

DESTDIR ?=
PREFIX  ?= /usr/local
VOID    := /dev/null

LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
PRGDIR  = programs
LZ4DIR  = lib
TESTDIR = tests


# Define nul output
ifneq (,$(filter Windows%,$(OS)))
EXT = .exe
else
EXT =
endif


.PHONY: default all lib lz4 clean test versionsTest examples

default: lz4

all: lib lz4

lib:
	@$(MAKE) -C $(LZ4DIR) all

lz4:
	@$(MAKE) -C $(PRGDIR)
	@cp $(PRGDIR)/lz4$(EXT) .

clean:
	@$(MAKE) -C $(PRGDIR) $@ > $(VOID)
	@$(MAKE) -C $(TESTDIR) $@ > $(VOID)
	@$(MAKE) -C $(LZ4DIR) $@ > $(VOID)
	@$(MAKE) -C examples $@ > $(VOID)
	@$(RM) lz4$(EXT)
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD, Hurd and
#FreeBSD targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU FreeBSD))

install:
	@$(MAKE) -C $(LZ4DIR) $@
	@$(MAKE) -C $(PRGDIR) $@

uninstall:
	@$(MAKE) -C $(LZ4DIR) $@
	@$(MAKE) -C $(PRGDIR) $@

travis-install:
	$(MAKE) install PREFIX=~/install_test_dir

test:
	$(MAKE) -C $(TESTDIR) test

cmake:
	@cd contrib/cmake_unofficial; cmake CMakeLists.txt; $(MAKE)

gpptest: clean
	$(MAKE) all CC=g++ CFLAGS="-O3 -I../lib -Wall -Wextra -Wundef -Wshadow -Wcast-align -Werror"

c_standards: clean
	$(MAKE) all MOREFLAGS="-std=gnu90 -Werror" 
	$(MAKE) clean
	$(MAKE) all MOREFLAGS="-std=c99 -Werror" 
	$(MAKE) clean
	$(MAKE) all MOREFLAGS="-std=gnu99 -Werror" 
	$(MAKE) clean
	$(MAKE) all MOREFLAGS="-std=c11 -Werror" 
	$(MAKE) clean

clangtest: clean
	clang -v
	CFLAGS="-O3 -Werror -Wconversion -Wno-sign-conversion" $(MAKE) all CC=clang

sanitize: clean
	CFLAGS="-O3 -g -fsanitize=undefined" $(MAKE) test CC=clang FUZZER_TIME="-T1mn" NB_LOOPS=-i1

staticAnalyze: clean
	CFLAGS=-g scan-build --status-bugs -v $(MAKE) all

platformTest: clean
	@echo "\n ---- test lz4 with $(CC) compiler ----"
	@$(CC) -v
	CFLAGS="-O3 -Werror"         $(MAKE) -C $(LZ4DIR) all
	CFLAGS="-O3 -Werror -static" $(MAKE) -C $(PRGDIR) bins
	CFLAGS="-O3 -Werror -static" $(MAKE) -C $(TESTDIR) bins
	$(MAKE) -C $(TESTDIR) test-platform

versionsTest: clean
	$(MAKE) -C $(TESTDIR) $@

examples:
	$(MAKE) -C $(LZ4DIR)
	$(MAKE) -C $(PRGDIR) lz4
	$(MAKE) -C examples test

endif
