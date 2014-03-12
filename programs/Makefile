# ################################################################
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
# ################################################################
# lz4 : Command Line Utility, supporting gzip-like arguments
# lz4c  : CLU, supporting also legacy lz4demo arguments
# lz4c32: Same as lz4c, but forced to compile in 32-bits mode
# fuzzer  : Test tool, to check lz4 integrity on target platform
# fuzzer32: Same as fuzzer, but forced to compile in 32-bits mode
# fullbench  : Precisely measure speed for each LZ4 function variant
# fullbench32: Same as fullbench, but forced to compile in 32-bits mode
# ################################################################

RELEASE=r114
DESTDIR=
PREFIX=/usr
CC:=$(CC)
CFLAGS+= -I.. -std=c99 -O3 -Wall -W -Wundef -DLZ4_VERSION=\"$(RELEASE)\"

BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1
LZ4DIR=..


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif


default: lz4 lz4c

all: lz4 lz4c lz4c32 fuzzer fuzzer32 fullbench fullbench32

lz4: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c bench.c xxhash.c lz4io.c lz4cli.c
	$(CC)      $(CFLAGS) -DDISABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

lz4c  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c bench.c xxhash.c lz4io.c lz4cli.c
	$(CC)      $(CFLAGS) $^ -o $@$(EXT)

lz4c32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c bench.c xxhash.c lz4io.c lz4cli.c
	$(CC) -m32 $(CFLAGS) $^ -o $@$(EXT)

fuzzer  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c fuzzer.c
	$(CC)      $(CFLAGS) $^ -o $@$(EXT)

fuzzer32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c fuzzer.c
	$(CC) -m32 $(CFLAGS) $^ -o $@$(EXT)

fullbench  : $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c xxhash.c fullbench.c
	$(CC)      $(CFLAGS) $^ -o $@$(EXT)

fullbench32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c xxhash.c fullbench.c
	$(CC) -m32 $(CFLAGS) $^ -o $@$(EXT)

clean:
	@rm -f core *.o \
        lz4$(EXT) lz4c$(EXT) lz4c32$(EXT) \
        fuzzer$(EXT) fuzzer32$(EXT) fullbench$(EXT) fullbench32$(EXT) 
	@echo Cleaning completed


ifneq (,$(filter $(shell uname),Linux Darwin))

install: lz4 lz4c
	@install -d -m 755 $(DESTDIR)$(BINDIR)/ $(DESTDIR)$(MANDIR)/
	@install -m 755 lz4   $(DESTDIR)$(BINDIR)/lz4
	@install -m 755 lz4c  $(DESTDIR)$(BINDIR)/lz4c
	@install -m 644 lz4.1 $(DESTDIR)$(MANDIR)/lz4.1
	@echo lz4 installation completed

uninstall:
	[ -x $(DESTDIR)$(BINDIR)/lz4 ] && rm -f $(DESTDIR)$(BINDIR)/lz4
	[ -x $(DESTDIR)$(BINDIR)/lz4c ] && rm -f $(DESTDIR)$(BINDIR)/lz4c
	[ -f $(DESTDIR)$(MANDIR)/lz4.1 ] && rm -f $(DESTDIR)$(MANDIR)/lz4.1
	@echo lz4 successfully uninstalled

endif
