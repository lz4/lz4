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
export VERSION=128
export RELEASE=r$(VERSION)

DESTDIR?=
PREFIX ?= /usr/local

LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
PRGDIR  = programs
LZ4DIR  = lib
DISTRIBNAME=lz4-$(RELEASE).tar.gz

TEXT =  $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4.h $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4hc.h \
	$(LZ4DIR)/lz4frame.c $(LZ4DIR)/lz4frame.h $(LZ4DIR)/lz4frame_static.h \
	$(LZ4DIR)/xxhash.c $(LZ4DIR)/xxhash.h \
	$(LZ4DIR)/liblz4.pc.in $(LZ4DIR)/Makefile $(LZ4DIR)/LICENSE \
	Makefile lz4_block_format.txt LZ4_Frame_Format.html NEWS README.md \
	cmake_unofficial/CMakeLists.txt \
	$(PRGDIR)/fullbench.c $(PRGDIR)/lz4cli.c \
	$(PRGDIR)/datagen.c $(PRGDIR)/datagen.h $(PRGDIR)/datagencli.c $(PRGDIR)/fuzzer.c \
	$(PRGDIR)/lz4io.c $(PRGDIR)/lz4io.h \
	$(PRGDIR)/bench.c $(PRGDIR)/bench.h \
	$(PRGDIR)/lz4.1 $(PRGDIR)/lz4c.1 $(PRGDIR)/lz4cat.1 \
	$(PRGDIR)/Makefile $(PRGDIR)/COPYING	
NONTEXT = images/image00.png images/image01.png images/image02.png \
	images/image03.png images/image04.png images/image05.png \
	images/image06.png
SOURCES = $(TEXT) $(NONTEXT)


# Select test target for Travis CI's Build Matrix
ifneq (,$(filter test-%,$(LZ4_TRAVIS_CI_ENV)))
TRAVIS_TARGET=prg-travis
else
TRAVIS_TARGET=$(LZ4_TRAVIS_CI_ENV)
endif


default: lz4programs

all: 
	@cd $(LZ4DIR); $(MAKE) -e all
	@cd $(PRGDIR); $(MAKE) -e all

lz4programs:
	@cd $(PRGDIR); $(MAKE) -e

clean:
	@rm -f $(DISTRIBNAME) *.sha1
	@cd $(PRGDIR); $(MAKE) clean
	@cd $(LZ4DIR); $(MAKE) clean
	@cd examples;  $(MAKE) clean
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

dist: clean
	@install -dD -m 700 lz4-$(RELEASE)/lib/
	@install -dD -m 700 lz4-$(RELEASE)/programs/
	@install -dD -m 700 lz4-$(RELEASE)/cmake_unofficial/
	@install -dD -m 700 lz4-$(RELEASE)/images/
	@for f in $(TEXT); do \
		tr -d '\r' < $$f > .tmp; \
		install -m 600 .tmp lz4-$(RELEASE)/$$f; \
	done
	@rm .tmp
	@for f in $(NONTEXT); do \
		install -m 600 $$f lz4-$(RELEASE)/$$f; \
	done
	@tar -czf $(DISTRIBNAME) lz4-$(RELEASE)/
	@rm -rf lz4-$(RELEASE)
	@sha1sum $(DISTRIBNAME) > $(DISTRIBNAME).sha1
	@echo Distribution $(DISTRIBNAME) built

test:
	@cd $(PRGDIR); $(MAKE) -e test

test-travis: $(TRAVIS_TARGET)

cmake:
	@cd cmake_unofficial; cmake CMakeLists.txt; $(MAKE)

gpptest: clean
	export CC=g++; export CFLAGS="-O3 -Wall -Wextra -Wundef -Wshadow -Wcast-align"; $(MAKE) -e all

clangtest: clean
	export CC=clang; $(MAKE) all

staticAnalyze: clean
	export CFLAGS=-g; scan-build -v $(MAKE) all

streaming-examples:
	cd examples; $(MAKE) -e test

prg-travis:
	@cd $(PRGDIR); $(MAKE) -e test-travis

endif
