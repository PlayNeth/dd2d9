#!/usr/bin/env python3
"""Compares a run of the Wine tests with a baseline.

usage: compare.py <baseline.json | baseline .jsonl> <results .jsonl> [--write-baseline out.json]

Prints the totals of both, every sub-test that got worse (more failures, or a crash)
with its first new failures, and the ones that got better. Exits 1 if anything got
worse, so it can gate a build.

A baseline .json holds, per sub-test, only the failure count and whether it crashed
(--write-baseline makes one from a results file); a .jsonl is another run's results.
"""
import json
import sys


def load(path):
    """{(file, test): {'nfail', 'crash', 'fails'}}"""
    if path.endswith('.jsonl'):
        d = {}
        for line in open(path):
            j = json.loads(line)
            d[(j['file'], j['test'])] = j
        return d
    raw = json.load(open(path))
    return {tuple(k.split(':', 1)): {'nfail': v['nfail'], 'crash': v['crash'], 'fails': []}
            for k, v in raw['tests'].items()}


def totals(d):
    return (sum(j['nfail'] for j in d.values()), sum(bool(j['crash']) for j in d.values()),
            sum(1 for j in d.values() if not j['nfail'] and not j['crash']), len(d))


def main():
    args = sys.argv[1:]
    write = None
    if '--write-baseline' in args:
        i = args.index('--write-baseline')
        write = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    base, new = load(args[0]), load(args[1])

    if write:
        json.dump({'note': 'per sub-test failure counts of a Wine ddraw test run (see compare.py)',
                   'tests': {f'{f}:{t}': {'nfail': j['nfail'], 'crash': bool(j['crash'])}
                             for (f, t), j in sorted(new.items())}},
                  open(write, 'w'), indent=1)
        print('baseline written to', write)

    for name, d in (('baseline', base), ('this run', new)):
        f, c, clean, n = totals(d)
        print(f'{name:9}: {f} failed checks, {c} crashed, {clean}/{n} sub-tests clean')

    worse, better = [], []
    for key, j in sorted(new.items()):
        b = base.get(key, {'nfail': 0, 'crash': False, 'fails': []})
        if j['nfail'] > b['nfail'] or (j['crash'] and not b['crash']):
            worse.append((key, b, j))
        elif j['nfail'] < b['nfail'] or (b['crash'] and not j['crash']):
            better.append((key, b, j))
    missing = [k for k in base if k not in new]

    for key, b, j in worse:
        print(f'WORSE  {key[0]} {key[1] or "(all)"}: {b["nfail"]} -> {j["nfail"]}'
              f'{" (now crashes)" if j["crash"] and not b["crash"] else ""}')
        old = set(b.get('fails', []))
        for line in [l for l in j.get('fails', []) if l not in old][:5]:
            print('         ' + line.split('Test failed: ', 1)[-1])
    for key, b, j in better:
        print(f'better {key[0]} {key[1] or "(all)"}: {b["nfail"]} -> {j["nfail"]}'
              f'{" (no longer crashes)" if b["crash"] and not j["crash"] else ""}')
    if missing:
        print(f'{len(missing)} sub-tests of the baseline did not run (--only, or a newer test tree?)')
    print('NO REGRESSIONS' if not worse else f'{len(worse)} SUB-TESTS GOT WORSE')
    sys.exit(1 if worse else 0)


if __name__ == '__main__':
    main()
