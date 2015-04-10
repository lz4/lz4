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
export VERSION=129
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


default: lz4programs

all: 
	@cd $(LZ4DIR); $(MAKE) -e all
	@cd $(PRGDIR); $(MAKE) -e all

lz4programs:
	@cd $(PRGDIR); $(MAKE) -e

clean:
	@cd $(PRGDIR); $(MAKE) clean > $(VOID)
	@cd $(LZ4DIR); $(MAKE) clean > $(VOID)
	@cd examples;  $(MAKE) clean > $(VOID)
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD and Hurd targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU))

install:
	@cd $(LZ4DIR); $(MAKE) -e install
	@cd $(PRGDIR); $(MAKE) -e install

uninstall:
	@cd $(LZ4DIR); $(MAKE) uninstall
	@cd $(PRGDIR); $(MAKE) uninstall

travis-install:
	sudo $(MAKE) install

test:
	@cd $(PRGDIR); $(MAKE) -e test

test-travis: $(TRAVIS_TARGET)

cmake:
	@cd cmake_unofficial; cmake CMakeLists.txt; $(MAKE)

gpptest: clean
	$(MAKE) all CC=g++ CFLAGS="-O3 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Werror"

clangtest: clean
	$(MAKE) all CC=clang CFLAGS="-O3 -Werror"

sanitize: clean
	$(MAKE) test CC=clang CFLAGS="-O3 -g -fsanitize=undefined" FUZZER_TIME="-T5mn"

staticAnalyze: clean
	scan-build -v $(MAKE) all CFLAGS=-g

armtest: clean
	cd lib; $(MAKE) -e all CC=arm-linux-gnueabi-gcc CFLAGS="-O3 -Werror"
	cd programs; $(MAKE) -e bins CC=arm-linux-gnueabi-gcc CFLAGS="-O3 -Werror"

streaming-examples:
	cd examples; $(MAKE) -e test

prg-travis:
	@cd $(PRGDIR); $(MAKE) -e test-travis

endif
