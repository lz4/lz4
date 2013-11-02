# ################################################################
# LZ4 Makefile
# Copyright (C) Yann Collet 2011-2013
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
# ################################################################
# lz4 : Command Line Utility, supporting gzip-like arguments
# lz4c  : CLU, supporting also legacy lz4demo arguments
# lz4c32: Same as lz4c, but forced to compile in 32-bits mode
# fuzzer  : Test tool, to check lz4 integrity on target platform
# fuzzer32: Same as fuzzer, but forced to compile in 32-bits mode
# fullbench  : Precisely measure speed for each LZ4 function variant
# fullbench32: Same as fullbench, but forced to compile in 32-bits mode
# ################################################################

RELEASE=r108
DESTDIR=
PREFIX=${DESTDIR}/usr
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1
DISTRIBNAME=lz4-$(RELEASE).tar.gz
CC=gcc
CFLAGS=-I. -std=c99 -Wall -W -Wundef

# Define *.exe as extension for Windows systems
# ifeq ($(OS),Windows_NT)
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif

TEXT = bench.c bench.h fullbench.c fuzzer.c lz4.1 lz4.c lz4cli.c \
	lz4_format_description.txt lz4.h lz4hc.c lz4hc.h \
	Makefile xxhash.c xxhash.h \
	NEWS COPYING \
	cmake/CMakeLists.txt cmake/pack/release_COPYING.txt \
	cmake/pack/CMakeLists.txt
NONTEXT = LZ4_Streaming_Format.odt
SOURCES = $(TEXT) $(NONTEXT)


default: lz4 lz4c

all: lz4 lz4c lz4c32 fuzzer fuzzer32 fullbench fullbench32

lz4: lz4.c lz4hc.c bench.c xxhash.c lz4cli.c
	$(CC)      -O3 $(CFLAGS) -DDISABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

lz4c  : lz4.c lz4hc.c bench.c xxhash.c lz4cli.c
	$(CC)      -O3 $(CFLAGS) $^ -o $@$(EXT)

lz4c32: lz4.c lz4hc.c bench.c xxhash.c lz4cli.c
	$(CC) -m32 -O3 $(CFLAGS) $^ -o $@$(EXT)

fuzzer  : lz4.c lz4hc.c fuzzer.c
	@echo fuzzer is a test tool to check lz4 integrity on target platform
	$(CC)      -O3 $(CFLAGS) $^ -o $@$(EXT)

fuzzer32: lz4.c lz4hc.c fuzzer.c
	$(CC) -m32 -O3 $(CFLAGS) $^ -o $@$(EXT)

fullbench  : lz4.c lz4hc.c xxhash.c fullbench.c
	$(CC)      -O3 $(CFLAGS) $^ -o $@$(EXT)

fullbench32: lz4.c lz4hc.c xxhash.c fullbench.c
	$(CC) -m32 -O3 $(CFLAGS) $^ -o $@$(EXT)

clean:
	@rm -f core *.o lz4$(EXT) lz4c$(EXT) lz4c32$(EXT) \
        fuzzer$(EXT) fuzzer32$(EXT) fullbench$(EXT) fullbench32$(EXT)
	@echo Cleaning completed


ifeq ($(shell uname),Linux)

install: lz4 lz4c
	@install -d -m 755 $(BINDIR)/ $(MANDIR)/
	@install -m 755 lz4 $(BINDIR)/lz4
	@install -m 755 lz4c $(BINDIR)/lz4c
	@install -m 644 lz4.1 $(MANDIR)/lz4.1

uninstall:
	[ -x $(BINDIR)/lz4 ] && rm -f $(BINDIR)/lz4
	[ -x $(BINDIR)/lz4c ] && rm -f $(BINDIR)/lz4c
	[ -f $(MANDIR)/lz4.1 ] && rm -f $(MANDIR)/lz4.1

dist: clean
	@install -dD -m 700 lz4-$(RELEASE)/cmake/pack/
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
	@echo Distribution $(DISTRIBNAME) built

endif
