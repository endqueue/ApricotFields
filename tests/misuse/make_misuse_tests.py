#!/usr/bin/env python3
"""Writes tests/apri_misuse_tests.c from abi.txt: a call to every public
function of ApricotFields with every argument zeroed - NULL handles and
pointers, zero sizes and counts, zeroed structs (ROADMAP.md, section 3.1,
"a misuse harness from the headers"). Each must not crash, and one that
returns APRESULT must not return APRESULT_OK, unless the list below says
why zeroes are a valid call.

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


def main():
    functions = []
    for line in open(os.path.join(ROOT, 'abi.txt')):
        m = re.match(r'fn (\w+) (.+?) \((.*)\)$', line.strip())
        if m:
            functions.append((m.group(1), m.group(2), m.group(3)))
    out = []
    out.append('// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.\n')
    out.append('// Every public function of ApricotFields called with every argument zeroed\n')
    out.append('// (ROADMAP.md, section 3.1). None may crash; one returning APRESULT returns\n')
    out.append('// an error unless the generator lists zeroes as a valid call for it.\n\n')
    out.append('#include "aprend.h"\n#include "archive/aparchive.h"\n#include "audio/apaudio.h"\n')
    out.append('#include "audio/apaudiocues.h"\n#include "jobs/apjobs.h"\n#include "math/apmathbvh.h"\n')
    out.append('#include "math/apmathpoly.h"\n#include "sync/apsyncnet.h"\n#include "text/aptext.h"\n')
    out.append('#include "aprimath.h"\n#include <stdio.h>\n\nstatic int failures = 0;\n\n')
    out.append('static void expect_error(const char *name, APRESULT result)\n{\n')
    out.append('    if (result == APRESULT_OK)\n    {\n')
    out.append('        printf("FAIL %s: returned APRESULT_OK for zeroes\\n", name);\n        failures++;\n    }\n}\n\n')
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
    out.append('    return failures ? 1 : 0;\n}\n')
    open(os.path.join(ROOT, 'tests', 'apri_misuse_tests.c'), 'w').write(''.join(out))


main()
