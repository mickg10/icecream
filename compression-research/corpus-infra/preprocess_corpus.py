#!/usr/bin/env python3
"""Preprocess every C++ TU in a compile_commands.json into a corpus of .ii files.

For each C++ entry we re-run the SAME compiler with the SAME flags, but swap
`-c -o x.o` for `-E -o <mirrored>.ii` (preprocess only; no compile/link).
Outputs mirror the object path under --outdir so names never collide.
"""
import argparse, json, os, shlex, subprocess, sys, multiprocessing as mp

CXX_EXT = ('.cc', '.cpp', '.cxx', '.c++', '.C', '.CC')

def parse_entry(e):
    if 'arguments' in e:
        args = list(e['arguments'])
    else:
        args = shlex.split(e['command'])
    directory = e['directory']
    src = e['file']
    # locate and strip -o <obj> and -c
    obj = None
    out_args = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-o':
            obj = args[i + 1]
            i += 2
            continue
        if a.startswith('-o') and len(a) > 2:
            obj = a[2:]
            i += 1
            continue
        if a == '-c':
            i += 1
            continue
        out_args.append(a)
        i += 1
    if obj is None:
        obj = e.get('output')
    return directory, src, out_args, obj

def make_out_path(directory, obj, outdir):
    # obj is typically relative to the build directory, e.g.
    # CMakeFiles/foo.dir/src/x.cc.o -> <outdir>/CMakeFiles/foo.dir/src/x.cc.ii
    if os.path.isabs(obj):
        rel = os.path.relpath(obj, directory)
    else:
        rel = obj
    rel = rel.lstrip('./')
    if rel.endswith('.o'):
        rel = rel[:-2] + '.ii'
    else:
        rel = rel + '.ii'
    return os.path.join(outdir, rel)

def work(job):
    directory, src, out_args, obj, outdir = job
    out_ii = make_out_path(directory, obj, outdir)
    os.makedirs(os.path.dirname(out_ii), exist_ok=True)
    cmd = out_args + ['-E', '-o', out_ii]
    try:
        r = subprocess.run(cmd, cwd=directory, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=300)
    except Exception as ex:
        return (src, out_ii, 1, f'exec-exception: {ex}'[:300])
    if r.returncode != 0:
        # clean up any partial/empty output so the manifest stays clean
        try:
            if os.path.exists(out_ii) and os.path.getsize(out_ii) == 0:
                os.remove(out_ii)
        except OSError:
            pass
        return (src, out_ii, r.returncode, r.stderr.decode('utf-8', 'replace')[-400:])
    return (src, out_ii, 0, '')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc-json', required=True)
    ap.add_argument('--outdir', required=True)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--log', required=True)
    ap.add_argument('--limit', type=int, default=0, help='only first N (for testing)')
    args = ap.parse_args()

    data = json.load(open(args.cc_json))
    jobs = []
    skipped_nonixx = 0
    seen = set()
    for e in data:
        src = e['file']
        if not src.endswith(CXX_EXT):
            skipped_nonixx += 1
            continue
        directory, src, out_args, obj = parse_entry(e)
        if obj is None:
            continue
        key = (directory, obj)
        if key in seen:
            continue
        seen.add(key)
        jobs.append((directory, src, out_args, obj, args.outdir))
    if args.limit:
        jobs = jobs[:args.limit]

    os.makedirs(args.outdir, exist_ok=True)
    print(f'total C++ entries: {len(jobs)}  (skipped non-C++: {skipped_nonixx})', flush=True)

    ok = 0
    fail = 0
    logf = open(args.log, 'w')
    with mp.Pool(args.jobs) as pool:
        for n, (src, out_ii, rc, err) in enumerate(pool.imap_unordered(work, jobs, chunksize=1), 1):
            if rc == 0:
                ok += 1
            else:
                fail += 1
                logf.write(f'FAIL rc={rc} {src}\n{err}\n{"-"*60}\n')
                logf.flush()
            if n % 50 == 0 or n == len(jobs):
                print(f'  {n}/{len(jobs)}  ok={ok} fail={fail}', flush=True)
    logf.write(f'\nSUMMARY ok={ok} fail={fail} total={len(jobs)}\n')
    logf.close()
    print(f'DONE ok={ok} fail={fail} total={len(jobs)}', flush=True)

if __name__ == '__main__':
    main()
