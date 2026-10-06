#!/usr/bin/env python3
"""Compile public headers with Clang C++ modules and verify C linkage.

Run from tests with: python3 test-header-modules.py
CXX may select a Clang compiler; missing or non-Clang compilers are skipped.
A Clang module compilation failure is a test failure.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class HeaderModulesTest(unittest.TestCase):
    def test_public_headers(self):
        lib = Path(__file__).resolve().parent.parent / "lib"
        cxx = shlex.split(os.environ.get("CXX", "clang++"))
        with tempfile.TemporaryDirectory(prefix="lz4-header-modules-",
                                         dir=os.environ.get("LZ4_TEST_TMPDIR")) as directory:
            work = Path(directory)
            flags = ["-std=c++11", "-fmodules", "-fcxx-modules",
                     "-Werror=module-import-in-extern-c",
                     "-fmodules-cache-path=" + str(work / "cache")]
            try:
                version = subprocess.run(cxx + ["--version"], capture_output=True,
                                         text=True, timeout=10)
            except FileNotFoundError:
                self.skipTest("Clang C++ compiler is unavailable")
            self.assertEqual(version.returncode, 0, version.stderr)
            if "clang" not in version.stdout.lower():
                self.skipTest("Clang C++ modules require a Clang compiler")
            probe = work / "probe.cpp"
            probe.write_text("#include <stdint.h>\nint main() { return 0; }\n")
            result = subprocess.run(cxx + flags + ["-c", str(probe), "-o", str(work / "probe.o")],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            cc = shlex.split(os.environ.get("CC", "cc"))
            objects = []
            for source in ("lz4.c", "lz4hc.c"):
                obj = work / (source + ".o")
                subprocess.run(cc + ["-I" + str(lib), "-c", str(lib / source), "-o", str(obj)],
                               check=True, timeout=60)
                objects.append(str(obj))
            for header, static_first, preinclude in (
                    (header, static_first, preinclude)
                    for header in ("lz4.h", "lz4hc.h")
                    for static_first in (False, True)
                    for preinclude in (False, True)):
                    with self.subTest(header=header, static_first=static_first, preinclude=preinclude):
                        source = work / "test.cpp"
                        source.write_text(
                            ("#include <stddef.h>\n#include <stdint.h>\n" if preinclude else "") +
                            ("#define LZ4_STATIC_LINKING_ONLY\n#define LZ4_HC_STATIC_LINKING_ONLY\n"
                             if static_first else "") +
                            '#include "' + header + '"\n' +
                            '#define LZ4_STATIC_LINKING_ONLY\n#define LZ4_HC_STATIC_LINKING_ONLY\n' +
                            '#include "lz4.h"\n#include "lz4hc.h"\n#include "lz4.h"\n' +
                            '#include <string.h>\nint main() {\n'
                            'const char input[] = "a normal public compression round trip";\n'
                            'char compressed[LZ4_COMPRESSBOUND(sizeof(input))];\n'
                            'char decoded[sizeof(input)];\n'
                            'LZ4_stream_t stream; LZ4_streamHC_t hc;\n'
                            'if (!LZ4_initStream(&stream, sizeof(stream))) return 1;\n'
                            'if (!LZ4_initStreamHC(&hc, sizeof(hc))) return 2;\n'
                            'int size = LZ4_compress_HC(input, compressed, sizeof(input), sizeof(compressed), 9);\n'
                            'if (size <= 0) return 3;\n'
                            'if (LZ4_decompress_safe(compressed, decoded, size, sizeof(decoded)) != sizeof(input)) return 4;\n'
                            'return memcmp(input, decoded, sizeof(input)) != 0;\n}\n')
                        binary = work / "test"
                        result = subprocess.run(cxx + flags + ["-I" + str(lib), str(source)] +
                                                objects + ["-o", str(binary)],
                                                capture_output=True, text=True, timeout=60)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
