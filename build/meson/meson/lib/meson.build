# #############################################################################
# Copyright (c) 2018-present        lzutao <taolzu(at)gmail.com>
# Copyright (c) 2022-present        Tristan Partin <tristan(at)partin.io>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

lz4_source_root = '../../../..'

sources = files(
  lz4_source_root / 'lib/lz4.c',
  lz4_source_root / 'lib/lz4frame.c',
  lz4_source_root / 'lib/lz4hc.c',
  lz4_source_root / 'lib/xxhash.c'
)

if get_option('unstable')
  sources += files(lz4_source_root / 'lib/lz4file.c')
endif

c_args = []

if host_machine.system() == 'windows' and get_option('default_library') != 'static'
  c_args += '-DLZ4_DLL_EXPORT=1'
endif

# Multi-threaded compression (see LZ4F_preferences_t.nbWorkers) : requires threads, except on Windows.
# By default, only the shared library supports it,
# so that programs linking the static library don't need to link threads.
default_library = get_option('default_library')
shared_mt = get_option('shared-multithread') and default_library != 'static'
static_mt = get_option('static-multithread') and default_library != 'shared'
internal_lib = get_option('tests') or get_option('programs') or get_option('examples') or get_option('ossfuzz')
mt_args = []
mt_deps = []
if shared_mt or static_mt or (internal_lib and get_option('enable_multithread'))
  if host_machine.system() == 'windows'
    mt_args = ['-DLZ4F_MULTITHREAD=1']
  else
    thread_dep = dependency('threads', required: false)
    if thread_dep.found()
      mt_args = ['-DLZ4F_MULTITHREAD=1']
      mt_deps = [thread_dep]
    else
      warning('threads not found : liblz4 is built without multithreading support')
      shared_mt = false
      static_mt = false
    endif
  endif
endif

if default_library == 'both' and shared_mt != static_mt
  # different compilation flags : shared and static libraries can't share objects
  # (c_shared_args and c_static_args would require meson >= 1.3)
  liblz4 = shared_library(
    'lz4',
    sources,
    c_args: c_args + (shared_mt ? mt_args : []),
    dependencies: shared_mt ? mt_deps : [],
    install: true,
    version: meson.project_version(),
    gnu_symbol_visibility: 'hidden'
  )
  liblz4_static = static_library(
    'lz4',
    sources,
    c_args: c_args + (static_mt ? mt_args : []),
    dependencies: static_mt ? mt_deps : [],
    install: true,
    gnu_symbol_visibility: 'hidden'
  )
else
  lib_mt = default_library == 'static' ? static_mt : shared_mt
  liblz4 = library(
    'lz4',
    sources,
    c_args: c_args + (lib_mt ? mt_args : []),
    dependencies: lib_mt ? mt_deps : [],
    install: true,
    version: meson.project_version(),
    gnu_symbol_visibility: 'hidden'
  )
  if default_library == 'both'
    liblz4_static = liblz4.get_static_lib()
  elif default_library == 'static'
    liblz4_static = liblz4
  endif
endif

liblz4_dep = declare_dependency(
  link_with: liblz4,
  dependencies: (default_library == 'static' and static_mt) ? mt_deps : [],
  compile_args: compile_args,
  include_directories: include_directories(lz4_source_root / 'lib')
)

meson.override_dependency('liblz4', liblz4_dep)

if internal_lib
  if default_library == 'shared'
    liblz4_internal = static_library(
      'lz4-internal',
      objects: liblz4.extract_all_objects(recursive: true),
      gnu_symbol_visibility: 'hidden'
    )
    internal_mt = shared_mt
  else
    liblz4_internal = liblz4_static
    internal_mt = static_mt
  endif

  if get_option('enable_multithread') and mt_args.length() > 0 and not internal_mt
    # programs compress frames with liblz4's worker threads
    liblz4_internal = static_library(
      'lz4-internal-mt',
      sources,
      c_args: c_args + mt_args,
      dependencies: mt_deps,
      gnu_symbol_visibility: 'hidden'
    )
    internal_mt = true
  endif

  liblz4_internal_dep = declare_dependency(
    link_with: liblz4_internal,
    dependencies: internal_mt ? mt_deps : [],
    compile_args: compile_args,
    include_directories: include_directories(lz4_source_root / 'lib')
  )
endif

# liblz4 is the shared library when both are built :
# the threads dependency of a multi-threaded static library must then be listed explicitly (pkg-config --static).
# A static-only build already lists it (Libs: -pthread).
pkgconfig.generate(
  liblz4,
  name: 'lz4',
  filebase: 'liblz4',
  description: 'extremely fast lossless compression algorithm library',
  version: meson.project_version(),
  url: 'http://www.lz4.org/',
  libraries_private: (default_library == 'both' and static_mt) ? mt_deps : []
)

install_headers(
  lz4_source_root / 'lib/lz4.h',
  lz4_source_root / 'lib/lz4hc.h',
  lz4_source_root / 'lib/lz4frame.h'
)

if get_option('default_library') != 'shared'
  install_headers(lz4_source_root / 'lib/lz4frame_static.h')
  if get_option('unstable')
    install_headers(lz4_source_root / 'lib/lz4file.h')
  endif
endif
