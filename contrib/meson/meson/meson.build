# #############################################################################
# Copyright (c) 2018-present        lzutao <taolzu(at)gmail.com>
# Copyright (c) 2022-present        Tristan Partin <tristan(at)partin.io>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################

cc = meson.get_compiler('c')

pkgconfig = import('pkgconfig')

lz4_source_root = '../../..'

lz4_version = meson.project_version()

lz4_h_file = lz4_source_root / 'lib/lz4.h'
GetLz4LibraryVersion_py = find_program('GetLz4LibraryVersion.py')
lz4_version = run_command(GetLz4LibraryVersion_py, lz4_h_file, check: true).stdout().strip()
message('Project version is now: @0@'.format(lz4_version))

add_project_arguments('-DXXH_NAMESPACE=LZ4_', language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments([
        '-Wcast-qual',
        '-Wcast-align',
        '-Wshadow',
        '-Wswitch-enum',
        '-Wdeclaration-after-statement',
        '-Wstrict-prototypes',
        '-Wundef',
        '-Wpointer-arith',
        '-Wstrict-aliasing=1',
        '-DLZ4_DEBUG=@0@'.format(get_option('debug-level')),
      ]
    ),
    language: 'c',
  )
endif

if get_option('memory-usage') > 0
  add_project_arguments(
    '-DLZ4_MEMORY_USAGE=@0@'.format(get_option('memory-usage')),
    language: 'c'
  )
endif

subdir('lib')

if get_option('programs')
  subdir('programs')
endif

if get_option('tests')
  subdir('tests')
endif

if get_option('contrib')
  subdir('contrib')
endif

if get_option('examples')
  subdir('examples')
endif
