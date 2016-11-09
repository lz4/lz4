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
#  - LZ4 source repository : https://github.com/Cyan4973/lz4
# ##########################################################################
# lz4 : Command Line Utility, supporting gzip-like arguments
# lz4c  : CLU, supporting also legacy lz4demo arguments
# lz4c32: Same as lz4c, but forced to compile in 32-bits mode
# ##########################################################################

DESTDIR ?=
PREFIX  ?= /usr/local
VOID    := /dev/null
BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1
LZ4DIR  := ../lib

CFLAGS  ?= -O3   # can select custom flags. For example : CFLAGS="-O2 -g" make
CFLAGS  += -Wall -Wextra -Wundef -Wcast-qual -Wcast-align -Wshadow -Wswitch-enum -Wdeclaration-after-statement -Wstrict-prototypes -Wpointer-arith
CFLAGS  += $(MOREFLAGS)
CPPFLAGS:= -I$(LZ4DIR) -DXXH_NAMESPACE=LZ4_
FLAGS   := $(CFLAGS) $(CPPFLAGS) $(LDFLAGS)


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif



default: lz4

m32: lz4c32

bins: lz4 lz4c

all: bins m32

lz4: $(LZ4DIR)/lz4.o $(LZ4DIR)/lz4hc.o $(LZ4DIR)/lz4frame.o $(LZ4DIR)/xxhash.o bench.o lz4io.o lz4cli.o datagen.o
	$(CC) $(FLAGS) $^ -o $@$(EXT)

lz4c  : $(LZ4DIR)/lz4.o $(LZ4DIR)/lz4hc.o $(LZ4DIR)/lz4frame.o $(LZ4DIR)/xxhash.o bench.o lz4io.o lz4cli.o datagen.o
	$(CC) $(FLAGS) -DENABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

lz4c32: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c $(LZ4DIR)/lz4frame.c $(LZ4DIR)/xxhash.c bench.c lz4io.c lz4cli.c datagen.c
	$(CC) -m32 $(FLAGS) -DENABLE_LZ4C_LEGACY_OPTIONS $^ -o $@$(EXT)

clean:
	@$(MAKE) -C $(LZ4DIR) $@ > $(VOID)
	@$(RM) core *.o *.test tmp* \
        lz4$(EXT) lz4c$(EXT) lz4c32$(EXT) \
	@echo Cleaning completed


#------------------------------------------------------------------------
#make install is validated only for Linux, OSX, kFreeBSD, Hurd and
#FreeBSD targets
ifneq (,$(filter $(shell uname),Linux Darwin GNU/kFreeBSD GNU FreeBSD))

install: lz4$(EXT) lz4c$(EXT)
	@echo Installing binaries
	@install -d -m 755 $(DESTDIR)$(BINDIR)/ $(DESTDIR)$(MANDIR)/
	@install -m 755 lz4$(EXT) $(DESTDIR)$(BINDIR)/lz4$(EXT)
	@ln -sf lz4$(EXT) $(DESTDIR)$(BINDIR)/lz4cat$(EXT)
	@ln -sf lz4$(EXT) $(DESTDIR)$(BINDIR)/unlz4$(EXT)
	@install -m 755 lz4c$(EXT) $(DESTDIR)$(BINDIR)/lz4c$(EXT)
	@echo Installing man pages
	@install -m 644 lz4.1 $(DESTDIR)$(MANDIR)/lz4.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/lz4c.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/lz4cat.1
	@ln -sf lz4.1 $(DESTDIR)$(MANDIR)/unlz4.1
	@echo lz4 installation completed

uninstall:
	@$(RM) $(DESTDIR)$(BINDIR)/lz4cat$(EXT)
	@$(RM) $(DESTDIR)$(BINDIR)/unlz4$(EXT)
	@$(RM) $(DESTDIR)$(BINDIR)/lz4$(EXT)
	@$(RM) $(DESTDIR)$(BINDIR)/lz4c$(EXT)
	@$(RM) $(DESTDIR)$(MANDIR)/lz4.1
	@$(RM) $(DESTDIR)$(MANDIR)/lz4c.1
	@$(RM) $(DESTDIR)$(MANDIR)/lz4cat.1
	@$(RM) $(DESTDIR)$(MANDIR)/unlz4.1
	@echo lz4 programs successfully uninstalled

endif
