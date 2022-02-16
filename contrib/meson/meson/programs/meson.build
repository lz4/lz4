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
  lz4_source_root / 'programs/bench.c',
  lz4_source_root / 'programs/datagen.c',
  lz4_source_root / 'programs/lz4cli.c',
  lz4_source_root / 'programs/lz4io.c',
)

lz4 = executable(
  'lz4',
  sources,
  include_directories: include_directories(lz4_source_root / 'programs'),
  dependencies: [liblz4_internal_dep],
  export_dynamic: get_option('debug') and host_machine.system() == 'windows',
  install: true
)

install_man(lz4_source_root / 'programs/lz4.1')

if meson.version().version_compare('>=0.61.0')
  foreach alias : ['lz4c', 'lz4cat', 'unlz4']
    install_symlink(
      alias,
      install_dir: get_option('bindir'),
      pointing_to: 'lz4'
    )
    install_symlink(
      '@0@.1'.format(alias),
      install_dir: get_option('mandir') / 'man1',
      pointing_to: 'lz4.1'
    )
  endforeach
endif
