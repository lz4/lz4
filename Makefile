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

# Version number
export VERSION=132
export RELEASE=r$(VERSION)

DESTDIR?=
PREFIX ?= /usr/local

LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
PRGDIR  = programs
LZ4DIR  = lib


# Select test target for Travis CI's Build Matrix
ifneq (,$(filter test-%,$(LZ4_TRAVIS_CI_ENV)))
TRAVIS_TARGET=prg-travis
else
TRAVIS_TARGET=$(LZ4_TRAVIS_CI_ENV)
endif

# Define nul output
ifneq (,$(filter Windows%,$(OS)))
VOID = nul
else
VOID = /dev/null
endif


.PHONY: default all lib lz4programs clean test versionsTest examples

default: lz4programs

all: lib
	@cd $(PRGDIR); $(MAKE) all

lib:
	@cd $(LZ4DIR); $(MAKE) all

lz4programs:
	@cd $(PRGDIR); $(MAKE)

clean:
	@cd $(PRGDIR); $(MAKE) clean > $(VOID)
	@cd $(LZ4DIR); $(MAKE) clean > $(VOID)
	@cd examples;  $(MAKE) clean > $(VOID)
	@cd versionsTest; $(MAKE) clean > $(VOID)
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD and Hurd targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU))

install:
	@cd $(LZ4DIR); $(MAKE) install
	@cd $(PRGDIR); $(MAKE) install

uninstall:
	@cd $(LZ4DIR); $(MAKE) uninstall
	@cd $(PRGDIR); $(MAKE) uninstall

travis-install:
	sudo $(MAKE) install

test:
	@cd $(PRGDIR); $(MAKE) test

test-travis: $(TRAVIS_TARGET)

cmake:
	@cd cmake_unofficial; cmake CMakeLists.txt; $(MAKE)

gpptest: clean
	$(MAKE) all CC=g++ CFLAGS="-O3 -I../lib -Wall -Wextra -Wundef -Wshadow -Wcast-align -Werror"

clangtest: clean
	CFLAGS="-O3 -Werror -Wconversion -Wno-sign-conversion" $(MAKE) all CC=clang

sanitize: clean
	CFLAGS="-O3 -g -fsanitize=undefined" $(MAKE) test CC=clang FUZZER_TIME="-T1mn" NB_LOOPS=-i1

staticAnalyze: clean
	CFLAGS=-g scan-build --status-bugs -v $(MAKE) all

armtest: clean
	cd lib; CFLAGS="-O3 -Werror" $(MAKE) all CC=arm-linux-gnueabi-gcc
	cd programs; CFLAGS="-O3 -Werror" $(MAKE) bins CC=arm-linux-gnueabi-gcc

versionsTest: clean
	@cd versionsTest; $(MAKE)

examples:
	cd lib; $(MAKE)
	cd programs; $(MAKE) lz4
	cd examples; $(MAKE) test

prg-travis:
	@cd $(PRGDIR); $(MAKE) test-travis

endif
