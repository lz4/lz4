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

examples = {
  'printVersion': 'printVersion.c',
  'doubleBuffer': 'blockStreaming_doubleBuffer.c',
  'dictionaryRandomAccess': 'dictionaryRandomAccess.c',
  'ringBuffer': 'blockStreaming_ringBuffer.c',
  'ringBufferHC': 'HCStreaming_ringBuffer.c',
  'lineCompress': 'blockStreaming_lineByLine.c',
  'frameCompress': 'frameCompress.c',
  'compressFunctions': 'compress_functions.c',
  'simpleBuffer': 'simple_buffer.c',
}

foreach e, src : examples
  executable(
    e,
    lz4_source_root / 'examples' / src,
    dependencies: [liblz4_internal_dep],
    install: false
  )
endforeach
