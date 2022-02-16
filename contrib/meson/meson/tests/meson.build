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

exes = {
  'fullbench': {
    'sources': files(lz4_source_root / 'tests/fullbench.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'fuzzer': {
    'sources': files(lz4_source_root / 'tests/fuzzer.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'frametest': {
    'sources': files(lz4_source_root / 'tests/frametest.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'roundTripTest': {
    'sources': files(lz4_source_root / 'tests/roundTripTest.c'),
  },
  'datagen': {
    'sources': files(lz4_source_root / 'tests/datagencli.c'),
    'objects': lz4.extract_objects(lz4_source_root / 'programs/datagen.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'checkFrame': {
    'sources': files(lz4_source_root / 'tests/checkFrame.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'checkTag': {
    'sources': files(lz4_source_root / 'tests/checkTag.c'),
  },
}

foreach e, attrs : exes
  executable(
    e,
    attrs.get('sources'),
    objects: attrs.get('objects', []),
    dependencies: [liblz4_internal_dep],
    include_directories: attrs.get('include_directories', []),
    install: false
  )
endforeach
