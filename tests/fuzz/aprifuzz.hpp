// What ApricotFields' fuzz targets share.
//
// Each target in this folder defines LLVMFuzzerTestOneInput over one of
// ApricotFields' readers of bytes the caller can't vouch for, and checks
// what the reader must keep whatever the input, beyond not crashing: a
// failed check prints and aborts, which libFuzzer reports as a crash and
// the replay test (aprifuzz_replay.cpp) as a failure.
//
// Built two ways (CMakeLists.txt): with -fsanitize=fuzzer under
// APRICOTFIELDS_BUILD_FUZZERS (the apricotfields-*-fuzz presets), and with
// aprifuzz_replay.cpp's main as a ctest under APRICOTFIELDS_BUILD_TESTS,
// which feeds it every file of its seed corpus (corpus/<target>) and its
// regression folder (regressions/<target>), with the normal toolchain.

#ifndef APRIFUZZ_HPP
#define APRIFUZZ_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define APRIFUZZ_CHECK(cond)                                                                                     \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            fprintf(stderr, "%s:%d: APRIFUZZ_CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            abort();                                                                                             \
        }                                                                                                        \
    } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif // APRIFUZZ_HPP
