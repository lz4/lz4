# ##########################################################################
# LZ4 oss fuzzer - Makefile
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
#  - LZ4 source repository : https://github.com/lz4/lz4
# ##########################################################################
# compress_fuzzer : OSS Fuzz test tool
# decompress_fuzzer : OSS Fuzz test tool
# ##########################################################################

LZ4DIR  := ../lib
LIB_FUZZING_ENGINE ?= standaloneengine.o

DEBUGLEVEL?= 1
DEBUGFLAGS = -g -DLZ4_DEBUG=$(DEBUGLEVEL)

CFLAGS   += -I$(LZ4DIR) $(DEBUGFLAGS) $(MOREFLAGS)
CXXFLAGS += -I$(LZ4DIR) $(DEBUGFLAGS) $(MOREFLAGS)
CPPFLAGS += -DXXH_NAMESPACE=LZ4_

include ../Makefile.inc

# Include a rule to build the static library if calling this target
# directly.
$(LZ4DIR)/liblz4.a:
	$(MAKE) -C $(LZ4DIR) CFLAGS="$(CFLAGS)" liblz4.a

%.o: %.cc
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) $< -o $@

# Generic rule for generating fuzzers
%_fuzzer: %_fuzzer.o $(LZ4DIR)/liblz4.a
	# Compile the standalone code just in case. The OSS-Fuzz code might
	# override the LIB_FUZZING_ENGINE value to "-fsanitize=fuzzer"
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) standaloneengine.cc -o standaloneengine.o

	# Now compile the actual fuzzer.
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LIB_FUZZING_ENGINE) $^ -o $@$(EXT)
