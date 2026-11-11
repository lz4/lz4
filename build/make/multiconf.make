# ##########################################################################
# multiconf.make
# Copyright (C) Yann Collet
#
# GPL-2.0-or-later License
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
# ##########################################################################


# Provides c_program(_shared_o) and cxx_program(_shared_o) target generation macros
# Provides static_library, c_dynamic_library and cxx_dynamic_library target generation macros
# Support recompilation of only impacted units when an associated *.h is updated.
# Provides V=1 / VERBOSE=1 support. V=2 is used for debugging purposes.
# Complement target clean: delete objects and binaries created by this script

# Source files are discovered in the current directory and below.
# Requires:
# - directory `.cache/multiconf/` available to cache object files.
#   alternatively: set MCM_CACHE_ROOT to some different value.
# Optional:
# - MCM_EXCLUDE_DIRS: directories to skip, besides hidden ones and MCM_CACHE_ROOT,
#   as paths from the current directory or `find -path` patterns (e.g. docs */experimental)
# - C_SRCDIRS, CXX_SRCDIRS, ASM_SRCDIRS: directories to add, e.g. excluded ones
# - C_SRCS, CPP_SRCS, CC_SRCS and ASM_SRCS (or C_OBJS, CPP_OBJS, CC_OBJS and ASM_OBJS)
#   replace the discovered files
# - MCM_EXTRA_OBJS: object files whose rule is written in the user Makefile
# - HASH can be set to a different custom hash program.
# - EXE: executable suffix, .exe by default on Windows, to set before the macro calls
# - MCM_CPPFLAGS, MCM_CFLAGS, MCM_CXXFLAGS, MCM_LDFLAGS: flags the Makefile needs (include paths,
#   -fPIC, soname...), globally or for a target (libfoo.so: MCM_CFLAGS += -fPIC). They stay when
#   the user sets CPPFLAGS, CFLAGS, CXXFLAGS or LDFLAGS, e.g. on the command line.

# *_program*: generates a recipe for a target that will be built in a cache directory.
# The cache directory is automatically derived from MCM_CACHE_ROOT and the compilers and flags
# in effect for the target, including target-level modifications (like: target: CFLAGS += someFlag).
# Targets with identical flags share their object files.
# *_shared_o* variants are kept for compatibility: they are identical to the standard variants.
#
# Note: enables .SECONDEXPANSION, so a `$$` in a prerequisite list is expanded twice.
# Note: binaries are phony targets, so a rule depending on one runs at every build.
# To link a program with the code of a library, list the library's objects instead.

# All *_program* macro functions take up to 4 argument:
# - The name of the target
# - The list of object files to build in the cache directory, named after their source file
#   from the current directory (src/util.o for src/util.c)
# - An optional list of extra dependencies, built by their own rules outside the cache directory
# - An optional complementary recipe code, that will run after compilation and link.
#   The built file is $$@, as usual in $(eval $(call ...)).


# Read once: included again (e.g. by two included files), this file would redefine its rules.
# origin, not ifndef: MCM_VERSION can come from the environment, e.g. exported by a parent make.
ifneq ($(origin MCM_VERSION),file)

# Version of this file, which each project keeps a copy of.
# 2: objects named from the current directory. Version 1 had no number.
MCM_VERSION := 2.0.0

# The rules of this file must not become the default goal: restored at the end of this file
MCM_SAVED_DEFAULT_GOAL := $(.DEFAULT_GOAL)

# Silent mode is default; use V=1 or VERBOSE=1 to see compilation lines (V=0 stays silent)
VERBOSE ?= $(V)
ifeq ($(filter-out 0,$(VERBOSE)),)
.SILENT:
endif

# Directory where object files will be built
MCM_CACHE_ROOT ?= .cache/multiconf
# Written as make writes file names, which it compares with $< and $@: without ./ at the start
# (make removes it), nor / at the end. override: it can be set on the command line.
override MCM_CACHE_ROOT := $(patsubst ./%,%,$(patsubst %/,%,$(MCM_CACHE_ROOT)))
# CACHE_ROOT, the former name, is ignored: an environment variable of that name would make `clean` delete its directory
ifneq (,$(filter file command line override,$(origin CACHE_ROOT)))
  $(warning CACHE_ROOT is ignored, set MCM_CACHE_ROOT instead)
endif

# Executable suffix, .exe on Windows (where Windows sets OS, also in MSYS2 and Cygwin)
ifeq ($(OS),Windows_NT)
  EXE ?= .exe
endif

# --------------------------------------------------------------------------------------------

# Dependency management
DEPFLAGS = -MT $@ -MMD -MP -MF

# --------------------------------------------------------------------------------------------

# Flags the Makefile needs: MCM_CPPFLAGS, MCM_CFLAGS, MCM_CXXFLAGS and MCM_LDFLAGS. CPPFLAGS, CFLAGS,
# CXXFLAGS and LDFLAGS are the user's: set on the command line, they replace the values of the
# Makefile, those of targets too. Each command places the MCM_ variables right before them, so that
# the user's flags come last (as in the GNU Coding Standards, and automake's AM_CFLAGS).
# Make's built-in rules (prog: prog.c), and the recipes written with their variables ($(LINK.c)),
# get them too: they are inserted in make's own definitions, not in those of the Makefile.
mcm_add_flags = $(subst $$(CPPFLAGS),$$(MCM_CPPFLAGS) $$(CPPFLAGS),$(subst $$(CFLAGS),$$(MCM_CFLAGS) $$(CFLAGS),$(subst $$(CXXFLAGS),$$(MCM_CXXFLAGS) $$(CXXFLAGS),$(subst $$(LDFLAGS),$$(MCM_LDFLAGS) $$(LDFLAGS),$(value $(1))))))
$(foreach v,$(filter COMPILE.% LINK.% LINT.% PREPROCESS.%,$(.VARIABLES)),$(if $(filter default,$(origin $(v))),$(eval $(v) = $(call mcm_add_flags,$(v)))))

# --------------------------------------------------------------------------------------------

# Automatic determination of build artifacts cache directory, keyed on build
# flags, so that we can do incremental, parallel builds of different binaries
# with different build flags without collisions.

# Layout: $(MCM_CACHE_ROOT)/<global flags>/<target flags>/ holds object files, and each binary
# is linked in a subdirectory keyed on its object list and link flags, so that changing
# them relinks without recompiling.

# mcm_key - Values of variables $(1), one shell word each, hashed one per line:
# CFLAGS=-O3 and CXXFLAGS=-O3 give different keys. Single quotes keep the shell from interpreting them.
mcm_key = $(foreach v,$(1),$(call mcm_quote,$(strip $($(v)))))
mcm_quote = '$(subst ','\'',$(1))'

# Flags that affect object files. Expanded in the context of each target,
# so that target-level modifications are included.
MCM_COMPILE_VARS := CC CXX MCM_CPPFLAGS CPPFLAGS MCM_CFLAGS CFLAGS MCM_CXXFLAGS CXXFLAGS
MCM_COMPILE_KEY = $(call mcm_key,$(MCM_COMPILE_VARS))

# A cache outside the current directory can be shared with the Makefiles of other directories
# (make MCM_CACHE_ROOT=/tmp/cache, passed to sub-makes), whose objects can have the same names:
# the current directory is then part of the keys
MCM_DIR_KEY := $(if $(filter $(CURDIR)/%,$(abspath $(MCM_CACHE_ROOT))),,$(call mcm_quote,$(CURDIR)))

# printf, not echo: the echo of some shells (dash) interprets backslashes.
mcm_global_key = printf '%s\n' $(MCM_COMPILE_KEY) $(MCM_DIR_KEY)

# The hash of the global flags also tells whether HASH works: empty if it does not.
# If HASH is not set, the same shell looks for it: every process counts, as it is slow to start on
# Windows. The name of the system would not tell what is installed (gmd5sum comes with coreutils).
ifdef HASH
  MCM_GLOBAL_DIR := $(firstword $(shell $(mcm_global_key) | $(HASH) 2>/dev/null))
else
  mcm_hash_found := $(shell for h in md5sum md5; do command -v $$h >/dev/null && { echo $$h; $(mcm_global_key) | $$h; break; }; done 2>/dev/null)
  HASH := $(or $(firstword $(mcm_hash_found)),md5sum)
  MCM_GLOBAL_DIR := $(word 2,$(mcm_hash_found))
endif
ifeq ($(MCM_GLOBAL_DIR),)
  $(info warning : could not find HASH ($(HASH)), required to differentiate builds using different flags)
  MCM_GLOBAL_DIR := generic
  mcm_subdirs = $(1)/bin
else
  # mcm_subdirs - <hash of $(2)>/<hash of $(3)>, in a single shell call.
  # md5sum follows each hash with `-`, or `*-` in binary mode (Windows).
  mcm_subdirs = $(call mcm_join_dirs,$(filter-out - *-,$(shell printf '%s\n' $(2) | $(HASH); printf '%s\n' $(3) | $(HASH))))
  mcm_join_dirs = $(word 1,$(1))/$(word 2,$(1))
endif

# The target-level flags of a target are only visible when make processes it,
# hence its cache path is computed during secondary expansion of prerequisites (see MCM_LINK_DIR).
.SECONDEXPANSION:

# mcm_cache_path - Cache path of binary $(1), from the flags in effect for $(1). Its link key
# also hashes the objects, the extra dependencies, and the text of the commands run after the
# link, as written (MCM_POST_*: $@ kept as is), so that changing them relinks.
mcm_cache_path = $(MCM_CACHE_ROOT)/$(MCM_GLOBAL_DIR)/$(call mcm_subdirs,$(1),$(MCM_COMPILE_KEY) $(call mcm_key,MCM_XHASH_$(1)),$(call mcm_key,MCM_OBJS_$(1) MCM_XDEPS_$(1) MCM_POST_$(1) $(MCM_LINK_VARS_$(1))))/$(1)

# mcm_cache_dep - Prerequisite of $(MCM_LINK_DIR)/$(1), computed by secondary expansion: the cache
# path of binary $(1). Kept for the run: make expands it twice for a binary not built yet, while
# it searches its rule.
mcm_cache_dep = $(or $(MCM_PATH_$(1)),$(eval MCM_PATH_$(1) := $(call mcm_cache_path,$(1)))$(MCM_PATH_$(1)))

# mcm_link_objs - Object files $(2) of a binary whose cache path stem is $(1):
# they are in the parent directory
mcm_link_objs = $(addprefix $(MCM_CACHE_ROOT)/$(dir $(1)),$(call mcm_cache_names,$(2)))

# mcm_cache_name - Path of object file $(1) in a cache directory. Sources may sit outside of the
# current directory (../lib/f.c), or have an absolute path: their objects must stay inside the
# cache directory of their flags, without colliding with each other. Each "../" becomes "__/",
# an absolute path gets a "__root__/" prefix, a leading "./" is dropped, ":" (Windows drives)
# becomes "_", and "\" becomes "/". From the make_ext branch of lz4.
mcm_cache_name = $(patsubst /%,__root__/%,$(subst ../,__/,$(patsubst ./%,%,$(subst :,_,$(subst \,/,$(1))))))
mcm_cache_names = $(foreach o,$(1),$(call mcm_cache_name,$(o)))

# Inputs of a link recipe: its prerequisites, except headers given as extra dependencies
MCM_LINK_INPUTS = $(filter-out %.h %.hh %.hpp %.hxx,$^)

# mcm_write_rsp - Writes $(1) to $@.rsp, the response file of $@, next to it in the cache.
# $(file) runs when make expands the recipe, even with make -n, which does not create the
# cache directory: not then.
mcm_write_rsp = $(if $(findstring n,$(firstword -$(MAKEFLAGS))),,$(file >$@.rsp,$(1)))

# mcm_link_list - Files $(1) of a link command: $(1), or @$@.rsp, after writing them there, with
# MCM_LD_RESPONSE_FILE=1, for small command line limits (Windows). Requires GNU make 4.0.
# Read when the recipe is expanded, like MCM_STRIP: it can be set for a single binary.
mcm_link_list = $(if $(filter 1,$(MCM_LD_RESPONSE_FILE)),$(call mcm_write_rsp,$(1))@$@.rsp,$(1))

# mcm_check_objs - Stops if binary $(1) lists object files $(2) without a rule, or with several
# sources (src/util.c and src/util.cpp both give src/util.o: only one would be compiled)
mcm_unknown_objs = $(filter-out $(C_OBJS) $(CPP_OBJS) $(CC_OBJS) $(ASM_OBJS) $(MCM_EXTRA_OBJS),$(1))
mcm_multi_src_objs = $(foreach o,$(1),$(if $(word 2,$(filter $(o),$(C_OBJS) $(CPP_OBJS) $(CC_OBJS) $(ASM_OBJS))),$(o)))
mcm_srcs_of = $(filter $(foreach x,c cpp cc S,$(1:.o=.$(x))),$(C_SRCS) $(CPP_SRCS) $(CC_SRCS) $(ASM_SRCS))
mcm_check_objs = $(if $(call mcm_unknown_objs,$(2)),$(error $(1): no source for $(call mcm_unknown_objs,$(2)): \
	objects are named after their source file from the current directory (src/util.o for src/util.c))) \
	$(foreach o,$(firstword $(call mcm_multi_src_objs,$(2))),$(error $(1): several sources for $(o): $(call mcm_srcs_of,$(o))))

# mcm_precious_extra - Line that marks the objects of $(1) built by rules of the user Makefile
# (MCM_EXTRA_OBJS) precious, like the others: reached through pattern rules, they are intermediate
# files, which make would delete after each build, then compile again at each link.
mcm_precious_extra = $(if $(filter $(MCM_EXTRA_OBJS),$(1)),.PRECIOUS: $(addprefix $(MCM_CACHE_ROOT)/%/,$(call mcm_cache_names,$(filter $(MCM_EXTRA_OBJS),$(1)))))

STRIP ?= strip
MKDIR ?= mkdir
LN ?= ln

# --------------------------------------------------------------------------------------------
# The following macros are used to create object files in the cache directory.
# The object files are named after the source file, but with a different path.

# Create build directories on-demand.
#
# For some reason, make treats the directory as an intermediate file and tries
# to delete it. So we work around that by marking it "precious". Solution found
# here:
# http://ismail.badawi.io/blog/2017/03/28/automatic-directory-creation-in-make/
.PRECIOUS: $(MCM_CACHE_ROOT)/%/.
$(MCM_CACHE_ROOT)/%/. :
	$(MKDIR) -p $@


define addTargetAsmObject  # objectName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2))))

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1))
$$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1)) : $(1:.o=.S) $(2) | $$(MCM_CACHE_ROOT)/%/$(dir $(call mcm_cache_name,$(1)))/. $$$$(MCM_ODEPS_$(1))
	@echo AS $$@
	$$(CC) $$(MCM_CPPFLAGS) $$(CPPFLAGS) $$(MCM_CFLAGS) $$(CFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(patsubst %.o,%.d,$(call mcm_cache_name,$(1))) -c $$< -o $$@

endef # addTargetAsmObject

define addTargetCObject  # objectName, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2)))) #debug print

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1))
$$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1)) : $(1:.o=.c) $(2) | $$(MCM_CACHE_ROOT)/%/$(dir $(call mcm_cache_name,$(1)))/. $$$$(MCM_ODEPS_$(1))
	@echo CC $$@
	$$(CC) $$(MCM_CPPFLAGS) $$(CPPFLAGS) $$(MCM_CFLAGS) $$(CFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(patsubst %.o,%.d,$(call mcm_cache_name,$(1))) -c $$< -o $$@

endef # addTargetCObject

define addTargetCxxObject  # objectName, suffix, addlDeps
$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3))))

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1))
$$(MCM_CACHE_ROOT)/%/$(call mcm_cache_name,$(1)) : $(1:.o=.$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/$(dir $(call mcm_cache_name,$(1)))/. $$$$(MCM_ODEPS_$(1))
	@echo CXX $$@
	$$(CXX) $$(MCM_CPPFLAGS) $$(CPPFLAGS) $$(MCM_CXXFLAGS) $$(CXXFLAGS) $$(DEPFLAGS) $$(MCM_CACHE_ROOT)/$$*/$(patsubst %.o,%.d,$(call mcm_cache_name,$(1))) -c $$< -o $$@

endef # addTargetCxxObject

# mcm_order_deps - Make objects $(1) wait for files $(2) before compiling, without
# recompiling when they change: e.g. headers of dependencies fetched on demand.
mcm_order_deps = $(foreach o,$(1),$(eval MCM_ODEPS_$(o) += $(2)))

# A Makefile that exports all its variables (export alone, .EXPORT_ALL_VARIABLES) must not export
# the functions of this file: make would run them for each command, and GNU make 4.4 and later
# for each $(shell) too, such as the ones below, where they would run each other recursively
unexport $(filter mcm_%,$(.VARIABLES))

# Discover source files: in the current directory and below, except in hidden directories,
# $(MCM_CACHE_ROOT) and MCM_EXCLUDE_DIRS. Files are listed rather than directories, so that
# large trees without sources (e.g. node_modules) only cost their traversal.
# find -path compares strings, and find prints ./build/cache: excluded directories are written that
# way (abspath turns ./build/cache, build//cache/ or $(CURDIR)/build/cache into build/cache).
MCM_SRCS := $(patsubst ./%,%,$(shell find . -name '.?*' -prune \
	$(foreach d,$(patsubst $(CURDIR)/%,%,$(abspath $(MCM_CACHE_ROOT) $(MCM_EXCLUDE_DIRS))),-o -path './$(d)' -prune) \
	-o ! -type d \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.S' \) -print))

# Directories added by C_SRCDIRS, CXX_SRCDIRS and ASM_SRCDIRS
vpath %.c $(C_SRCDIRS)
vpath %.cpp $(CXX_SRCDIRS)
vpath %.cc $(CXX_SRCDIRS)
vpath %.S $(ASM_SRCDIRS)

# If C_SRCS, CPP_SRCS, CC_SRCS and ASM_SRCS are not defined, use the discovered and added files
C_SRCS   ?= $(sort $(filter %.c,$(MCM_SRCS)) $(foreach dir,$(C_SRCDIRS),$(wildcard $(dir)/*.c)))
CPP_SRCS ?= $(sort $(filter %.cpp,$(MCM_SRCS)) $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cpp)))
CC_SRCS  ?= $(sort $(filter %.cc,$(MCM_SRCS)) $(foreach dir,$(CXX_SRCDIRS),$(wildcard $(dir)/*.cc)))
CXX_SRCS ?= $(CPP_SRCS) $(CC_SRCS)
ASM_SRCS ?= $(sort $(filter %.S,$(MCM_SRCS)) $(foreach dir,$(ASM_SRCDIRS),$(wildcard $(dir)/*.S)))

# If C_OBJS, CPP_OBJS, CC_OBJS and ASM_OBJS are not defined, derive them from the source lists.
# CXX_SRCS and CXX_OBJS list both kinds of C++ files, for the user Makefile.
C_OBJS  ?= $(patsubst %.c,%.o,$(C_SRCS))
CPP_OBJS ?= $(patsubst %.cpp,%.o,$(CPP_SRCS))
CC_OBJS  ?= $(patsubst %.cc,%.o,$(CC_SRCS))
CXX_OBJS ?= $(CPP_OBJS) $(CC_OBJS)
ASM_OBJS ?= $(patsubst %.S,%.o,$(ASM_SRCS))

# Create targets for individual object files
$(foreach OBJ,$(C_OBJS),$(eval $(call addTargetCObject,$(OBJ))))
$(foreach OBJ,$(CPP_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cpp)))
$(foreach OBJ,$(CC_OBJS),$(eval $(call addTargetCxxObject,$(OBJ),cc)))
$(foreach OBJ,$(ASM_OBJS),$(eval $(call addTargetAsmObject,$(OBJ))))

# Include the depfiles of objects built with the current global flags, so that header
# changes trigger recompilation. Cache directories of other global flags are not read.
MCM_DEPFILES := $(shell find $(MCM_CACHE_ROOT)/$(MCM_GLOBAL_DIR) -name '*.d' 2>/dev/null)
# Empty rule: stops make searching implicit rules to remake each depfile (~100 failed stat() each).
$(MCM_DEPFILES): ;
include $(MCM_DEPFILES)

# --------------------------------------------------------------------------------------------
# The following macros are used to create targets in the user Makefile.
# Binaries are built in the cache directory, and then symlinked to the current directory.
# The cache directory is automatically derived from MCM_CACHE_ROOT and list of flags and compilers.

# Each binary is a phony target, so that its link follows the current configuration, and depends
# on $(MCM_LINK_DIR)/<binary>, never created, whose rule computes its cache path and links it.
# A pattern rule: make expands its prerequisites when it builds the target, in the context of the
# binary and of its parent targets, so it sees the flags they set (parent: CFLAGS += -m32;
# parent: app), which then get their own objects. The prerequisites of an explicit rule are
# expanded by GNU make 4.3 and earlier when it starts, for all targets, and without these flags.
# Not under MCM_CACHE_ROOT: $(MCM_CACHE_ROOT)/%/app, the rule of the cache binary, would match.
# Reached through this pattern rule, cache binaries are intermediate files, which make deletes
# after the build: the macros mark them .PRECIOUS, like objects.
MCM_LINK_DIR := .mcm-link
$(MCM_LINK_DIR)/% : $$(call mcm_cache_dep,$$*)$$(MCM_EXE_$$*)
	$(LN) -sf $< $*$(MCM_EXE_$*)


# static_library - Create build rules for a static library with caching
# Parameters:
#   1. libName       - Library name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps     - Additional dependencies (no cache path prefix), only archived if object files
#   4. postBuildCmds - Extra commands to run after AR
#   5. extraHash     - Additional key to compute the unique cache path
# Archived with $(AR) $(ARFLAGS), as set by the Makefile, else make's default (rv).
# Example:
#   $(eval $(call static_library,libmath.a,src/vector.o src/matrix.o,$(CONFIG_H),cp $$@ dist/,$(VERSION)))
define static_library  # libName, objectDeps, extraDeps, postBuildCmds, extraHash

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5))))
$$(call mcm_check_objs,$(1),$(2))
$(call mcm_precious_extra,$(2))
MCM_ALL_LINKS += $(1)
MCM_OBJS_$(1) := $(2)
MCM_XDEPS_$(1) := $(3)
MCM_POST_$(1) := $(subst $$,$$$$,$(4))
MCM_XHASH_$(1) := $(5)
MCM_LINK_VARS_$(1) := AR ARFLAGS

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)
$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo AR $$@
	$$(AR) $$(ARFLAGS) $$@ $$(call mcm_link_list,$$(filter %.o,$$^))
	$(4)

.PHONY: $(1)
$(1) : $(MCM_LINK_DIR)/$(1)

endef # static_library


# c_dynamic_library, cxx_dynamic_library - Create build rules for a dynamic/shared library with
# caching, linked with $(CC) and the C flags, or with $(CXX) and the C++ flags (C++ objects)
# Parameters:
#   1. libName      - Library name (becomes output file and phony target)
#   2. objectDeps   - Object file dependencies (will be built in cache path)
# The following parameters are all optional:
#   3. extraDeps    - Additional dependencies (no cache path prefix), passed to the linker
#   4. postLinkCmds - Extra commands to run after linking
#   5. extraHash    - Additional key to compute the unique cache path
# Objects are compiled with the flags of the library: add -fPIC where the platform needs it,
# e.g. libmath.so: MCM_CFLAGS += -fPIC, and to MCM_CXXFLAGS for C++ objects
# Example:
#   $(eval $(call c_dynamic_library,libmath.so,src/vector.o src/matrix.o,$(LIBZ_SO),strip $$@,$(VERSION)))
define mcm_dynamic_library  # libName, objectDeps, extraDeps, postLinkCmds, extraHash, compiler, compilerFlags

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5),$(6),$(7))))
$$(call mcm_check_objs,$(1),$(2))
$(call mcm_precious_extra,$(2))
MCM_ALL_LINKS += $(1)
MCM_OBJS_$(1) := $(2)
MCM_XDEPS_$(1) := $(3)
MCM_POST_$(1) := $(subst $$,$$$$,$(4))
MCM_XHASH_$(1) := $(5)
MCM_LINK_VARS_$(1) := MCM_LDFLAGS LDFLAGS LDLIBS MCM_STRIP

.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)
$$(MCM_CACHE_ROOT)/%/$(1) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo LD $$@
	$$($(6)) $$(MCM_CPPFLAGS) $$(CPPFLAGS) $$(MCM_$(7)) $$($(7)) $$(MCM_LDFLAGS) $$(LDFLAGS) -shared -o $$@ $$(call mcm_link_list,$$(MCM_LINK_INPUTS)) $$(LDLIBS)
	$$(if $$(filter 1,$$(MCM_STRIP)),-$$(STRIP) -S $$@)
	$(4)

.PHONY: $(1)
$(1) : $(MCM_LINK_DIR)/$(1)

endef # mcm_dynamic_library

# Plain aliases, without an eval of their own, so that postLinkCmds is expanded as in mcm_dynamic_library
c_dynamic_library = $(call mcm_dynamic_library,$(1),$(2),$(3),$(4),$(5),CC,CFLAGS)
cxx_dynamic_library = $(call mcm_dynamic_library,$(1),$(2),$(3),$(4),$(5),CXX,CXXFLAGS)


# program_base - Create build rules for an executable program with caching
# Parameters:
#   1. progName      - Executable name (becomes output file and phony target)
#   2. objectDeps    - Object file dependencies (will be prefixed with cache path)
# Parameters 3 to 5 are optional:
#   3. extraDeps     - Additional dependencies (without cache path prefix), passed to the linker
#   4. postLinkCmds  - Extra commands to run after linking
#   5. extraHash     - Additional data to include in cache path hash
# Parameters 6 & 7 are compulsory:
#   6. compiler      - Variable name of compiler to use (CC or CXX)
#   7. compilerFlags - Variable name of compiler flags to use (CFLAGS or CXXFLAGS), after MCM_<compilerFlags>
# Example:
#   $(eval $(call program_base,myapp,src/main.o src/utils.o,$(LIBZ_A),strip $$@,$(VERSION),CC,CFLAGS))
#   $(eval $(call program_base,mycppapp,src/main.o src/utils.o,$(LIBZ_A),strip $$@,$(VERSION),CXX,CXXFLAGS))
define program_base  # progName, objectDeps, extraDeps, postLinkCmds, extraHash, compiler, compilerFlags

$$(if $$(filter 2,$$(V)),$$(info $$(call $(0),$(1),$(2),$(3),$(4),$(5),$(6),$(7))))
$$(call mcm_check_objs,$(1),$(2))
$(call mcm_precious_extra,$(2))
MCM_ALL_LINKS += $(1)$(EXE)
MCM_OBJS_$(1) := $(2)
MCM_XDEPS_$(1) := $(3)
MCM_POST_$(1) := $(subst $$,$$$$,$(4))
MCM_XHASH_$(1) := $(5)
MCM_LINK_VARS_$(1) := MCM_LDFLAGS LDFLAGS LDLIBS MCM_STRIP
MCM_EXE_$(1) := $(EXE)

# The program is built as $(1)$(EXE), the name a Windows linker gives it anyway
.PRECIOUS: $$(MCM_CACHE_ROOT)/%/$(1)$(EXE)
$$(MCM_CACHE_ROOT)/%/$(1)$(EXE) : $$$$(call mcm_link_objs,$$$$*,$(2)) $(3) | $$(MCM_CACHE_ROOT)/%/.
	@echo LD $$@
	$$($(6)) $$(MCM_CPPFLAGS) $$(CPPFLAGS) $$(MCM_$(7)) $$($(7)) $$(call mcm_link_list,$$(MCM_LINK_INPUTS)) -o $$@ $$(MCM_LDFLAGS) $$(LDFLAGS) $$(LDLIBS)
	$$(if $$(filter 1,$$(MCM_STRIP)),-$$(STRIP) $$@)
	$(4)

.PHONY: $(1)
$(1) : $(MCM_LINK_DIR)/$(1)

endef # program_base
# Note: $(EXE), the executable suffix, must be set before the macro calls

# c_program, cxx_program - progName, objectDeps, extraDeps, postLinkCmds: see program_base.
# Plain aliases, without an eval of their own, so that postLinkCmds is expanded as in program_base.
c_program = $(call program_base,$(1),$(2),$(3),$(4),,CC,CFLAGS)
cxx_program = $(call program_base,$(1),$(2),$(3),$(4),,CXX,CXXFLAGS)

# Kept for compatibility: identical to c_program and cxx_program
c_program_shared_o = $(call c_program,$(1),$(2),$(3),$(4))
cxx_program_shared_o = $(call cxx_program,$(1),$(2),$(3),$(4))

# --------------------------------------------------------------------------------------------

# Cleaning: delete all objects and binaries created by this script, the parent directory
# of MCM_CACHE_ROOT if left empty, and cachedObjs/, the default cache directory of earlier versions
.PHONY: clean_cache
clean_cache:
	$(RM) -rf $(MCM_CACHE_ROOT) cachedObjs
	rmdir $(dir $(MCM_CACHE_ROOT)) 2>/dev/null || true
	$(RM) $(MCM_ALL_LINKS)

# automatically attach to standard clean target
.PHONY: clean
clean: clean_cache

# Deletes the cache of the other global configurations (other compilers or global flags), and
# keeps the current one. Not for a cache outside the current directory: the Makefiles of other
# directories may share it, and their configurations are other ones.
.PHONY: clean_other_configs
clean_other_configs:
	$(if $(MCM_DIR_KEY),@echo 'clean_other_configs: $(MCM_CACHE_ROOT) is outside of the current directory: use clean_cache' >&2; exit 1)
	for d in $(MCM_CACHE_ROOT)/*; do [ "$$d" = $(MCM_CACHE_ROOT)/$(MCM_GLOBAL_DIR) ] || $(RM) -r "$$d"; done

# Restore the default goal: the first target defined before this file, or else after it
.DEFAULT_GOAL := $(MCM_SAVED_DEFAULT_GOAL)

# Nor its macros, which call these functions, nor its lists: sub-makes have their own
unexport addTargetAsmObject addTargetCObject addTargetCxxObject static_library c_dynamic_library \
	cxx_dynamic_library mcm_dynamic_library \
	program_base c_program cxx_program c_program_shared_o cxx_program_shared_o MCM_ALL_LINKS \
	C_SRCS CPP_SRCS CC_SRCS CXX_SRCS ASM_SRCS C_OBJS CPP_OBJS CC_OBJS CXX_OBJS ASM_OBJS

endif # read once
