# ################################################################
# LZ4 library - Makefile
# Copyright (C) Yann Collet 2011-2014
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
#  - LZ4 source repository : http://code.google.com/p/lz4/
#  - LZ4 forum froup : https://groups.google.com/forum/#!forum/lz4c
# ################################################################

RELEASE=r113
DESTDIR=
PREFIX=/usr
CC=gcc
CFLAGS+= -I. -std=c99 -Wall -W -Wundef -DLZ4_VERSION=\"$(RELEASE)\"

LIBDIR=$(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
PRGDIR=programs
DISTRIBNAME=lz4-$(RELEASE).tar.gz


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif

TEXT = lz4.c lz4.h lz4hc.c lz4hc.h \
	lz4_format_description.txt Makefile NEWS LICENSE \
	cmake_unofficial/CMakeLists.txt \
	$(PRGDIR)/fullbench.c $(PRGDIR)/fuzzer.c $(PRGDIR)/lz4cli.c \
	$(PRGDIR)/lz4io.c $(PRGDIR)/lz4io.h \
	$(PRGDIR)/bench.c $(PRGDIR)/bench.h \
	$(PRGDIR)/xxhash.c $(PRGDIR)/xxhash.h \
	$(PRGDIR)/lz4.1 $(PRGDIR)/Makefile $(PRGDIR)/COPYING
NONTEXT = LZ4_Streaming_Format.odt
SOURCES = $(TEXT) $(NONTEXT)


default: liblz4
	@cd $(PRGDIR); make

all: liblz4 lz4programs

liblz4: liblz4.a liblz4.so

lz4programs: lz4.c lz4hc.c
	@cd $(PRGDIR); make all

liblz4.a: lz4.c lz4hc.c
	$(CC) -O3 -c $(CFLAGS) $^
	ar rcs liblz4.a lz4.o lz4hc.o

liblz4.so: lz4.c lz4hc.c
	$(CC) -shared -fPIC -Wl,--soname=liblz4.so.1 $(CFLAGS) $^ -o $@

clean:
	@rm -f core *.o *.a *.so $(DISTRIBNAME)
	@cd $(PRGDIR); make clean
	@echo Cleaning completed


#ifeq ($(shell uname),Linux)
ifneq (,$(filter $(shell uname),Linux Darwin))

install: liblz4
	@install -d -m 755 $(DESTDIR)$(LIBDIR)/ $(DESTDIR)$(INCLUDEDIR)/
	@install -m 755 liblz4.a $(DESTDIR)$(LIBDIR)/liblz4.a
	@install -m 755 liblz4.so $(DESTDIR)$(LIBDIR)/liblz4.so
	@install -m 755 lz4.h $(DESTDIR)$(INCLUDEDIR)/lz4.h
	@install -m 755 lz4hc.h $(DESTDIR)$(INCLUDEDIR)/lz4hc.h
	@echo lz4 static and shared library installed
	@cd $(PRGDIR); make install

uninstall:
	[ -x $(DESTDIR)$(LIBDIR)/liblz4.a ] && rm -f $(DESTDIR)$(LIBDIR)/liblz4.a
	[ -x $(DESTDIR)$(LIBDIR)/liblz4.so ] && rm -f $(DESTDIR)$(LIBDIR)/liblz4.so
	[ -f $(DESTDIR)$(INCLUDEDIR)/lz4.h ] && rm -f $(DESTDIR)$(INCLUDEDIR)/lz4.h
	[ -f $(DESTDIR)$(INCLUDEDIR)/lz4hc.h ] && rm -f $(DESTDIR)$(INCLUDEDIR)/lz4hc.h
	@echo lz4 libraries successfully uninstalled
	@cd $(PRGDIR); make uninstall

dist: clean
	@install -dD -m 700 lz4-$(RELEASE)/programs/
	@install -dD -m 700 lz4-$(RELEASE)/cmake_unofficial/
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
