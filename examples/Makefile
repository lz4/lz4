# ##########################################################################
# LZ4 examples - Makefile
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

CC     := $(CC)
CFLAGS ?= -O3
CFLAGS += -std=c99 -Wall -Wextra -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -Wno-missing-braces   # Wno-missing-braces required due to GCC <4.8.3 bug
FLAGS   = -I../lib $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

TESTFILE= Makefile
LZ4DIR  = ../lib


# Minimize test target for Travis CI's Build Matrix
ifeq ($(LZ4_TRAVIS_CI_ENV),-m32)
CFLAGS += -m32
else ifeq ($(LZ4_TRAVIS_CI_ENV),-m64)
endif


# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
VOID = nul
else
EXT =
VOID = /dev/null
endif


default: all

all: printVersion doubleBuffer ringBuffer ringBufferHC lineCompress

printVersion: $(LZ4DIR)/lz4.c printVersion.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

doubleBuffer: $(LZ4DIR)/lz4.c blockStreaming_doubleBuffer.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

ringBuffer  : $(LZ4DIR)/lz4.c blockStreaming_ringBuffer.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

ringBufferHC: $(LZ4DIR)/lz4.c $(LZ4DIR)/lz4hc.c HCStreaming_ringBuffer.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

lineCompress: $(LZ4DIR)/lz4.c blockStreaming_lineByLine.c
	$(CC)      $(FLAGS) $^ -o $@$(EXT)

test : all
	./printVersion$(EXT)
	./doubleBuffer$(EXT) $(TESTFILE)
	./ringBuffer$(EXT)   $(TESTFILE)
	./ringBufferHC$(EXT) $(TESTFILE)
	./lineCompress$(EXT) $(TESTFILE)

clean:
	@rm -f core *.o *.dec *-0 *-9 *-8192 *.lz4s \
        printVersion$(EXT) doubleBuffer$(EXT) ringBuffer$(EXT) ringBufferHC$(EXT) lineCompress$(EXT)
	@echo Cleaning completed

