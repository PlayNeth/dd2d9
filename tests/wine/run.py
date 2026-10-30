#!/usr/bin/env python3
"""Runs every Wine ddraw sub-test in its own process against one ddraw.dll.

usage: run.py <ddraw.dll | builtin> <label> [options]
  -> results/<label>.jsonl, one line per sub-test: failures, crash, summary

options:
  --display N    X display to use (default 78; started with display.sh)
  --prefix DIR   Wine prefix (default work/prefix, created on first use)
  --lenient      Wine's own expectations (todo_wine, broken) instead of Windows' (the default)
  --only FILE    only the sub-tests of one test file (ddraw1, ddraw7, ...)

Wine comes from $WINE (default: wine on the PATH); wineserver from beside it.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, 'work')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('label')
    ap.add_argument('--display', default='78')
    ap.add_argument('--prefix', default=os.path.join(WORK, 'prefix'))
    ap.add_argument('--lenient', action='store_true')
    ap.add_argument('--only')
    a = ap.parse_args()

    wine = os.environ.get('WINE', 'wine')
    wineserver = os.path.join(os.path.dirname(shutil.which(wine) or wine), 'wineserver')
    if not os.path.exists(wineserver):
        wineserver = 'wineserver'

    run_dir = os.path.join(WORK, 'run-' + a.label)
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir)
    for f in os.listdir(WORK):
        if f.endswith('.exe'):
            shutil.copy(os.path.join(WORK, f), run_dir)
    builtin = a.dll == 'builtin'
    if not builtin:
        shutil.copy(a.dll, os.path.join(run_dir, 'ddraw.dll'))

    env = dict(os.environ, WINEPREFIX=a.prefix, WINEDEBUG='-all', DISPLAY=':' + a.display,
               WINETEST_PLATFORM='wine' if a.lenient else 'windows',
               WINEDLLOVERRIDES=('ddraw=b' if builtin else 'ddraw=n') + ';mscoree=;mshtml=;winedbg.exe=d')
    subprocess.run([os.path.join(HERE, 'display.sh'), a.display])
    subprocess.run([wineserver, '-k'], env=env)

    names = json.load(open(os.path.join(WORK, 'subtests.json')))
    jobs = [(f, n) for f, ns in names.items() for n in (ns or [''])]
    if a.only:
        jobs = [j for j in jobs if j[0] == a.only]
    os.makedirs(os.path.join(HERE, 'results'), exist_ok=True)
    out_path = os.path.join(HERE, 'results', a.label + '.jsonl')
    with open(out_path, 'w') as out:
        for i, (f, n) in enumerate(jobs, 1):
            e = dict(env, WT_ONLY=n)
            t0 = time.time()
            try:
                p = subprocess.run([wine, f + '.exe'], cwd=run_dir, env=e, capture_output=True, timeout=90)
                rc, text = p.returncode, (p.stdout + p.stderr).decode('utf-8', 'replace')
            except subprocess.TimeoutExpired as x:
                rc, text = 'timeout', ((x.stdout or b'') + (x.stderr or b'')).decode('utf-8', 'replace')
            fails = [l for l in text.splitlines() if 'Test failed' in l]
            summary = re.findall(r'\d+ tests executed.*', text)
            # A test returns its failure count; one that ends without its summary line crashed.
            crashed = rc != 0 and not summary
            if rc == 'timeout' or crashed:
                subprocess.run([wineserver, '-k'], env=env)
            out.write(json.dumps({'file': f, 'test': n, 'rc': rc, 'secs': round(time.time() - t0, 1),
                                  'nfail': len(fails), 'fails': fails[:40],
                                  'summary': summary[-1].strip() if summary else None,
                                  'crash': crashed}) + '\n')
            out.flush()
            print(f'[{i}/{len(jobs)}] {f} {n or "(all)"}: {len(fails)} failed{" CRASH" if crashed else ""}',
                  flush=True)
    print('results in', out_path)


if __name__ == '__main__':
    main()
