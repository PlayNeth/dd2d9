#!/usr/bin/env python3
"""Prepares Wine's ddraw tests for running one sub-test per process.

usage: patch_tests.py <wine tests dir> <out dir>

Copies each test file to <out dir>/src, guarding every sub-test call in its START_TEST
body with wt_want("<name>") (the WT_ONLY environment variable picks one; unset runs
all), and writes <out dir>/subtests.json with the sub-test names of each file, in order.
"""
import json
import os
import re
import sys

FILES = ['ddraw1', 'ddraw2', 'ddraw4', 'ddraw7', 'ddrawmodes', 'dsurface', 'refcount', 'visual', 'd3d']

HELPER = '''
/* dd2d9 harness: run one sub-test per process (WT_ONLY=<name>), all if unset. */
#include <string.h>
static int wt_want(const char *name)
{
    const char *only = getenv("WT_ONLY");
    return !only || !*only || !strcmp(only, name);
}

'''

# A sub-test call on a line of its own: test_x(...); or run_for_each_device_type(test_x);
CALL = re.compile(r'^(\s+)((?:run_for_each_device_type\((test_\w+)\))|((test_\w+)\(.*\)));\s*$')


def patch(text):
    start = re.search(r'^START_TEST\(\w+\)\s*$', text, re.M)
    if not start:
        raise SystemExit('no START_TEST')
    head, body = text[:start.start()], text[start.start():]
    names = []
    out = []
    for line in body.split('\n'):
        m = CALL.match(line)
        if m:
            name = m.group(3) or m.group(5)
            if name not in names:
                names.append(name)
            line = f'{m.group(1)}if (wt_want("{name}")) {m.group(2)};'
        out.append(line)
    return head + HELPER + '\n'.join(out), names


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(f'{dst}/src', exist_ok=True)
    subtests = {}
    for f in FILES:
        text = open(f'{src}/{f}.c', encoding='utf-8', errors='surrogateescape').read()
        patched, names = patch(text)
        with open(f'{dst}/src/{f}.c', 'w', encoding='utf-8', errors='surrogateescape') as o:
            o.write(patched)
        subtests[f] = names
    with open(f'{dst}/subtests.json', 'w') as o:
        json.dump(subtests, o, indent=0)
    print(' '.join(f'{f}:{len(n)}' for f, n in subtests.items()))


if __name__ == '__main__':
    main()
