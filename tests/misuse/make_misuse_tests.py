#!/usr/bin/env python3
"""Writes tests/apri_misuse_tests.c from abi.txt, in two halves.

Zeroes: a call to every public function of ApricotFields with every
argument zeroed - NULL handles and pointers, zero sizes and counts, zeroed
structs (ROADMAP.md, section 3.1, "a misuse harness from the headers").
Each must not crash, and one that returns APRESULT must not return
APRESULT_OK, unless the list below says why zeroes are a valid call.

Destroyed handles (the workspace's DECISIONS.md, card X16): for every
public function whose first parameter is a handle type with a factory in
misuse_factories.h (written by hand, beside this), a call with a handle
that factory made and destroyed, every other argument zeroed. It must not
crash, and must refuse: one returning APRESULT returns anything but
APRESULT_OK, a getter returns what it returns for a NULL handle, and a
void call just returns. Only a Debug build checks a destroyed handle, and
a sanitizer's heap reports the read of freed memory itself, so this half
runs in a Debug build not under a sanitizer and says why it skipped
otherwise. A handle type with no factory is listed in the output, with the
reason.

Run after ./check.sh --update-abi changes ApricotFields/abi.txt:
    python3 tests/misuse/make_misuse_tests.py
The output is checked in, so a build needs no Python."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Functions zeroes are a valid call for, each with the reason.
ZEROES_SUCCEED = {
}

# Functions not called: zeroes are outside their contract in a way a
# NULL check can't catch, each with the reason.
NOT_CALLED = {
}

# Handle types misuse_factories.h makes a destroyed one of: the type ->
# its factory, a function there that returns the handle destroyed, or NULL
# when it couldn't make it.
FACTORIES = {
    'aparchive_codecs': 'dead_aparchive_codecs',
    'aparchive_writer': 'dead_aparchive_writer',
    'aparchive_reader': 'dead_aparchive_reader',
    'aparchive_entry_reader': 'dead_aparchive_entry_reader',
    'apaudio_mixer': 'dead_apaudio_mixer',
    'apaudio_cues': 'dead_apaudio_cues',
    'apjobs_pool': 'dead_apjobs_pool',
    'apmath_mesh_bvh': 'dead_apmath_mesh_bvh',
    'apmath_instance_bvh': 'dead_apmath_instance_bvh',
    'apmath_poly': 'dead_apmath_poly',
    'apsync_session': 'dead_apsync_session',
    'aptext_face': 'dead_aptext_face',
    'aptext_chain': 'dead_aptext_chain',
    'aptext_layout': 'dead_aptext_layout',
    'aptext_atlas': 'dead_aptext_atlas',
}

# Handle types with no factory here, each with the reason. A handle type
# in neither table fails the generator.
NO_FACTORY = {
    'aprend_': 'an Aprend handle needs a GPU device, which these tests never make; '
               'apricottests/tests/aprend_gpu_tests.c, test_destroyed_handles, calls these',
}


def no_factory_reason(handle):
    for prefix, reason in NO_FACTORY.items():
        if handle.startswith(prefix):
            return reason
    return None


def main():
    functions = []
    handles = []
    for line in open(os.path.join(ROOT, 'abi.txt')):
        m = re.match(r'fn (\w+) (.+?) \((.*)\)$', line.strip())
        if m:
            functions.append((m.group(1), m.group(2), m.group(3)))
        m = re.match(r'typedef (\w+) = struct \1_t \*$', line.strip())
        if m:
            handles.append(m.group(1))
    for handle in handles:
        if handle not in FACTORIES and not no_factory_reason(handle):
            raise SystemExit('%s: a handle type with no factory and no reason for none' % handle)
    out = []
    out.append('// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.\n')
    out.append('// Every public function of ApricotFields called with every argument zeroed\n')
    out.append('// (ROADMAP.md, section 3.1). None may crash; one returning APRESULT returns\n')
    out.append('// an error unless the generator lists zeroes as a valid call for it.\n')
    out.append('// Then, in a Debug build not under a sanitizer, every one whose first\n')
    out.append('// parameter is a handle type misuse/misuse_factories.h can make, called\n')
    out.append('// with a destroyed handle (DECISIONS.md, card X16): each refuses.\n')
    out.append('//\n')
    out.append('// Handle types with no factory, not called with a destroyed handle:\n')
    for prefix, reason in NO_FACTORY.items():
        uncovered = [h for h in sorted(handles) if h not in FACTORIES and h.startswith(prefix)]
        if uncovered:
            out.append('// - %s%s.\n//   Not covered: %s.\n' % (reason[0].upper(), reason[1:], ', '.join(uncovered)))
    out.append('\n')
    out.append('#include "aprend.h"\n#include "archive/aparchive.h"\n#include "audio/apaudio.h"\n')
    out.append('#include "audio/apaudiocues.h"\n#include "jobs/apjobs.h"\n#include "math/apmathbvh.h"\n')
    out.append('#include "math/apmathpoly.h"\n#include "sync/apsyncnet.h"\n#include "text/aptext.h"\n')
    out.append('#include "aprimath.h"\n#include "misuse/misuse_factories.h"\n#include <stdio.h>\n#include <string.h>\n\n')
    out.append('#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)\n#define MISUSE_SANITIZED 1\n')
    out.append('#elif defined(__has_feature)\n')
    out.append('#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)\n')
    out.append('#define MISUSE_SANITIZED 1\n#endif\n#endif\n#ifndef MISUSE_SANITIZED\n#define MISUSE_SANITIZED 0\n#endif\n\n')
    out.append('static int failures = 0;\n\n')
    out.append('static void expect_error(const char *name, APRESULT result)\n{\n')
    out.append('    if (result == APRESULT_OK)\n    {\n')
    out.append('        printf("FAIL %s: returned APRESULT_OK for zeroes\\n", name);\n        failures++;\n    }\n}\n\n')
    out.append('#if _DEBUG && !MISUSE_SANITIZED\n')
    out.append('static int destroyed_calls = 0;\n\n')
    out.append('static void factory_missing(const char *name)\n{\n')
    out.append('    printf("FAIL %s: its factory made no handle to destroy\\n", name);\n    failures++;\n}\n\n')
    out.append('static void expect_refused(const char *name, APRESULT result)\n{\n')
    out.append('    destroyed_calls++;\n    if (result == APRESULT_OK)\n    {\n')
    out.append('        printf("FAIL %s: returned APRESULT_OK for a destroyed handle\\n", name);\n        failures++;\n    }\n}\n\n')
    out.append('// A getter given a destroyed handle returns what it returns for NULL.\n')
    out.append('static void expect_null_fallback(const char *name, const void *got, const void *for_null, size_t size)\n{\n')
    out.append('    destroyed_calls++;\n    if (memcmp(got, for_null, size) != 0)\n    {\n')
    out.append('        printf("FAIL %s: a destroyed handle returned other than NULL\'s fallback\\n", name);\n        failures++;\n    }\n}\n\n')
    out.append('static void calls_with_destroyed_handles(void)\n{\n')
    destroyed = []
    for name, ret, args in functions:
        if name in NOT_CALLED:
            continue
        params = [a.strip() for a in args.split(',')] if args.strip() not in ('', 'void') else []
        if not params or params[0] not in FACTORIES:
            continue
        handle = params[0]
        rest = ''.join(', (%s){0}' % p for p in params[1:])
        dead_call = '%s(dead%s)' % (name, rest)
        null_call = '%s((%s){0}%s)' % (name, handle, rest)
        out.append('    {\n')
        if ret not in ('APRESULT', 'void'):
            out.append('        %s for_null = %s;\n' % (ret, null_call))
        out.append('        %s dead = %s();\n' % (handle, FACTORIES[handle]))
        out.append('        if (!dead)\n            factory_missing("%s");\n' % name)
        if ret == 'APRESULT':
            out.append('        else\n            expect_refused("%s", %s);\n' % (name, dead_call))
        elif ret == 'void':
            out.append('        else\n        {\n            %s;\n            destroyed_calls++;\n        }\n' % dead_call)
        else:
            out.append('        else\n        {\n            %s got = %s;\n' % (ret, dead_call))
            out.append('            expect_null_fallback("%s", &got, &for_null, sizeof(got));\n        }\n' % name)
        out.append('    }\n')
        destroyed.append(name)
    out.append('}\n#endif\n\n')
    out.append('int main(void)\n{\n    int calls = 0;\n')
    for name, ret, args in functions:
        if name in NOT_CALLED:
            out.append('    // %s: not called: %s\n' % (name, NOT_CALLED[name]))
            continue
        params = [a.strip() for a in args.split(',')] if args.strip() not in ('', 'void') else []
        call = '%s(%s)' % (name, ', '.join('(%s){0}' % p for p in params))
        if ret == 'APRESULT':
            if name in ZEROES_SUCCEED:
                out.append('    (void)%s; // zeroes are valid: %s\n' % (call, ZEROES_SUCCEED[name]))
            else:
                out.append('    expect_error("%s", %s);\n' % (name, call))
        else:
            out.append('    (void)%s;\n' % call if ret != 'void' else '    %s;\n' % call)
        out.append('    calls++;\n')
    out.append('    printf("apri_misuse_tests: %d calls with zeroes, %d failure(s)\\n", calls, failures);\n')
    out.append('#if _DEBUG && !MISUSE_SANITIZED\n')
    out.append('    // Each refusal below prints a line on stderr: they are expected.\n')
    out.append('    const int zero_failures = failures;\n')
    out.append('    calls_with_destroyed_handles();\n')
    out.append('    printf("apri_misuse_tests: %%d calls with a destroyed handle (of %d generated), %%d failure(s)\\n", '
               'destroyed_calls, failures - zero_failures);\n' % len(destroyed))
    out.append('#elif _DEBUG\n')
    out.append('    printf("apri_misuse_tests: calls with a destroyed handle skipped: under a sanitizer, whose heap '
               'reports the read of freed memory itself\\n");\n')
    out.append('#else\n')
    out.append('    printf("apri_misuse_tests: calls with a destroyed handle skipped: a Release build doesn\'t check '
               'for one\\n");\n')
    out.append('#endif\n')
    out.append('    return failures ? 1 : 0;\n}\n')
    open(os.path.join(ROOT, 'tests', 'apri_misuse_tests.c'), 'w').write(''.join(out))


main()
