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

fuzzer_time = 90
test_exes = {
  'abiTest': {
    'sources': files(lz4_source_root / 'tests/compat/abiTest.c'),
    'test': false,
  },
  'checkFrame': {
    'sources': files(lz4_source_root / 'tests/tools/checkFrame.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'checkTag': {
    'sources': files(lz4_source_root / 'tests/compat/checkTag.c'),
    'test': false,
  },
  'datagen': {
    'sources': files(
        lz4_source_root / 'programs/lorem.c',
        lz4_source_root / 'tests/tools/datagencli.c',
        lz4_source_root / 'tests/tools/datagen.c',
        lz4_source_root / 'tests/tools/loremOut.c',
        ),
    'include_directories': include_directories(lz4_source_root / 'programs'),
  },
  'frametest': {
    'sources': files(
      lz4_source_root / 'tests/fuzz/frametest.c',
    ),
    'include_directories': include_directories(lz4_source_root / 'programs'),
    'args': ['-v', '-T@0@s'.format(fuzzer_time)],
    'test': false,
  },
  'freestanding': {
    'sources': files(lz4_source_root / 'tests/compat/freestanding.c'),
    'c_args': ['-ffreestanding', '-fno-stack-protector', '-Wno-unused-parameter', '-Wno-declaration-after-statement', '-DLZ4_DEBUG=0'],
    'link_args': ['-nostdlib'],
    'build': cc.get_id() in ['gcc', 'clang'] and
      host_machine.system() == 'linux' and host_machine.cpu_family() == 'x86_64',
    'override_options': ['optimization=1'],
  },
  'fullbench': {
    'sources': files(lz4_source_root / 'tests/bench/fullbench.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
    'args': ['--no-prompt', '-i1', files(lz4_source_root / 'tests/data/COPYING')],
    'test': false,
  },
  'fuzzer': {
    'sources': files(lz4_source_root / 'tests/fuzz/fuzzer.c'),
    'include_directories': include_directories(lz4_source_root / 'programs'),
    'args': ['-T@0@s'.format(fuzzer_time)],
    'test': false,
  },
  'unittest': {
    'sources': files(
      lz4_source_root / 'tests/unit/unittest.c',
      lz4_source_root / 'tests/unit/lz4frame_helpers.c',
      lz4_source_root / 'tests/unit/unittest_lz4.c',
      lz4_source_root / 'tests/unit/unittest_lz4hc.c',
      lz4_source_root / 'tests/unit/unittest_lz4frame.c',
      lz4_source_root / 'tests/unit/unittest_lz4frameMT.c',
      lz4_source_root / 'tests/tools/datagen.c',
      lz4_source_root / 'programs/timefn.c',
      lz4_source_root / 'lib/lz4file.c',
    ),
    # tells unittest_lz4frameMT.c if liblz4 supports multi-threading (see lib/meson.build)
    'c_args': internal_mt ? ['-DLZ4F_MULTITHREAD=1'] : [],
    'include_directories': include_directories(lz4_source_root / 'programs', lz4_source_root / 'tests/tools'),
    'timeout': 360,   # -O0 (buildtype=debug) : about 1 minute on a CI runner, sometimes more than twice as long
  },
}

targets = {}

foreach e, attrs : test_exes
  if not attrs.get('build', true)
    targets += {e: disabler()}
    continue
  endif

  t = executable(
    e,
    attrs.get('sources'),
    c_args: attrs.get('c_args', []),
    link_args: attrs.get('link_args', []),
    objects: attrs.get('objects', []),
    dependencies: [liblz4_internal_dep],
    include_directories: attrs.get('include_directories', []),
    install: false,
    override_options: attrs.get('override_options', [])
  )

  targets += {e: t}

  if not attrs.get('test', true)
    continue
  endif

  test(
    e,
    t,
    args: attrs.get('params', []),
    timeout: attrs.get('timeout', 120)
  )
endforeach

fs = import('fs')

run_env.prepend('PATH', meson.current_build_dir())

test_scripts = {
  'lz4-basic': {
    'depends': [lz4, lz4cat, unlz4, targets['datagen']],
  },
  'lz4-dict': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-contentSize': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-fast-hugefile': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-frame-concatenation': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-multiple': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-multiple-legacy': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-opt-parser': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-skippable': {
    'depends': [lz4],
  },
  'lz4-sparse': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4-testmode': {
    'depends': [lz4, targets['datagen']],
  },
  'lz4hc-hugefile': {
    'depends': [lz4, targets['datagen']],
  },
}

foreach s, attrs : test_scripts
  script = find_program(lz4_source_root / 'tests/cli/test-@0@.sh'.format(s))

  test(
    '@0@'.format(s),
    script,
    depends: attrs.get('depends', []),
    workdir: fs.parent(fs.parent(script.full_path())),   # tests/, as with tests/Makefile
    env: run_env,
    timeout: 360
  )
endforeach
