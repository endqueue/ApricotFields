// What Rabbittus' fuzz targets share.
//
// Each target in this folder defines LLVMFuzzerTestOneInput over one of
// Rabbittus' readers of bytes the caller can't vouch for, and checks
// what the reader must keep whatever the input, beyond not crashing: a
// failed check prints and aborts, which libFuzzer reports as a crash and
// the replay test (rbtfuzz_replay.cpp) as a failure.
//
// Built two ways (CMakeLists.txt): with -fsanitize=fuzzer under
// RABBITTUS_BUILD_FUZZERS (the rabbittus-*-fuzz presets), and with
// rbtfuzz_replay.cpp's main as a ctest under RABBITTUS_BUILD_TESTS,
// which feeds it every file of its seed corpus (corpus/<target>) and its
// regression folder (regressions/<target>), with the normal toolchain.

#ifndef RBTFUZZ_HPP
#define RBTFUZZ_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RBTFUZZ_CHECK(cond)                                                                                     \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            fprintf(stderr, "%s:%d: RBTFUZZ_CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            abort();                                                                                             \
        }                                                                                                        \
    } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif // RBTFUZZ_HPP
