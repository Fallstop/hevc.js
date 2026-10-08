#!/usr/bin/env python3
"""Compare hevc-decode against ffmpeg over the JCT-VC HEVC_v1 conformance suite.

The bitstreams are not committed (~150 MB). This script downloads them once
into a cache directory, decodes each with the given hevc-decode CLI and with
ffmpeg (the reference, as for every oracle in tests/), and reports which match.
Where the output differs from ffmpeg but matches the whole-YUV MD5 that the
suite ships with the stream, the status is "pass-suite" (ffmpeg is wrong or
does not support the stream).

Usage:
  tools/jctvc_conformance.py <hevc-decode> [--cache DIR] [--out results.json]
                             [--filter SUBSTR] [--jobs N] [--compare base.json]

--compare prints the streams whose status differs from an earlier results
file, which is how a change is checked for conformance regressions.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

BASE = 'https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/'


def md5_file(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def wanted(member):
    base = os.path.basename(member).lower()
    if '__MACOSX' in member or not base:
        return False
    return base.endswith(('.bit', '.bin')) or 'md5' in base


def extract(zpath, dest_dir):
    with zipfile.ZipFile(zpath) as z:
        for member in z.namelist():
            # bitstreams and MD5 files only, never paths outside dest_dir
            if not wanted(member):
                continue
            base = os.path.basename(member)
            with z.open(member) as src, open(os.path.join(dest_dir, base), 'wb') as dst:
                dst.write(src.read())
    open(os.path.join(dest_dir, '.extracted'), 'w').close()


def fetch(cache):
    os.makedirs(cache, exist_ok=True)
    listing = urllib.request.urlopen(BASE, timeout=60).read().decode('utf-8', 'replace')
    names = sorted(set(re.findall(r'HEVC_v1/([^"/]+\.zip)"', listing)))
    for name in names:
        dest_dir = os.path.join(cache, name[:-4])
        zpath = os.path.join(cache, name)
        if os.path.exists(os.path.join(dest_dir, '.extracted')):
            continue
        if not os.path.exists(zpath):
            print('download', name, file=sys.stderr)
            urllib.request.urlretrieve(BASE + name, zpath)
        os.makedirs(dest_dir, exist_ok=True)
        extract(zpath, dest_dir)
    return sorted(d for d in os.listdir(cache) if os.path.isdir(os.path.join(cache, d)))


def suite_md5(cache, name):
    """The suite's MD5 of the whole decoded YUV, when it ships exactly one."""
    files = [p for p in glob.glob(os.path.join(cache, name, '*'))
             if 'md5' in os.path.basename(p).lower() and os.path.getsize(p) <= 4096]
    yuv = [p for p in files if 'yuv' in os.path.basename(p).lower()]
    digests = set()
    for path in yuv or files:
        with open(path, 'rb') as f:
            for line in f.read().splitlines():
                if re.search(rb'\.(bit|bin)\b', line, re.I):
                    continue  # the MD5 of the bitstream itself
                digests.update(d.lower() for d in re.findall(rb'\b[0-9a-fA-F]{32}\b', line))
    return digests.pop().decode() if len(digests) == 1 else None


def bitstream(cache, name):
    found = sorted(glob.glob(os.path.join(cache, name, '*.bit')) +
                   glob.glob(os.path.join(cache, name, '*.bin')))
    return found[0] if found else None


def reference(cache, name, bs, refs):
    if name in refs:
        return refs[name]
    pix = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=pix_fmt', '-of', 'csv=p=0', bs],
                         capture_output=True, text=True).stdout.strip() or 'yuv420p'
    tmp = os.path.join(cache, f'.ref_{name}.yuv')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', bs, '-f', 'rawvideo', '-pix_fmt', pix, tmp],
                   capture_output=True, timeout=900)
    ok = os.path.exists(tmp)
    refs[name] = {'pix_fmt': pix, 'md5': md5_file(tmp) if ok else None,
                  'size': os.path.getsize(tmp) if ok else 0}
    if ok:
        os.remove(tmp)
    return refs[name]


def run_one(args, refs, name):
    bs = bitstream(args.cache, name)
    if not bs:
        return name, {'status': 'nobitstream'}
    ref = reference(args.cache, name, bs, refs)
    tmp = os.path.join(args.cache, f'.out_{name}.yuv')
    try:
        proc = subprocess.run([args.decoder, bs, '-o', tmp], capture_output=True, timeout=args.timeout)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        return name, {'status': 'timeout'}
    got = md5_file(tmp) if os.path.exists(tmp) else None
    size = os.path.getsize(tmp) if os.path.exists(tmp) else 0
    if os.path.exists(tmp):
        os.remove(tmp)
    if got and got == ref['md5']:
        status = 'pass'
    elif got and got == suite_md5(args.cache, name):
        status = 'pass-suite'
    else:
        status = 'crash' if rc < 0 else 'fail'
    return name, {'status': status, 'rc': rc, 'size': size, 'ref_size': ref['size']}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('decoder')
    ap.add_argument('--cache', default=os.path.expanduser('~/.cache/hevcjs-jctvc'))
    ap.add_argument('--out', default='jctvc_results.json')
    ap.add_argument('--filter', default='')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--compare')
    args = ap.parse_args()

    names = [n for n in fetch(args.cache) if args.filter in n]
    ref_path = os.path.join(args.cache, 'ffmpeg_refs.json')
    refs = json.load(open(ref_path)) if os.path.exists(ref_path) else {}
    with ThreadPoolExecutor(args.jobs) as ex:
        results = dict(ex.map(lambda n: run_one(args, refs, n), names))
    json.dump(refs, open(ref_path, 'w'), indent=1)
    json.dump(results, open(args.out, 'w'), indent=1, sort_keys=True)

    counts = {}
    for r in results.values():
        counts[r['status']] = counts.get(r['status'], 0) + 1
    print(json.dumps(counts))
    if args.compare:
        before = json.load(open(args.compare))
        for name in sorted(results):
            a = before.get(name, {}).get('status')
            b = results[name]['status']
            if a != b:
                print(f'{name:40s} {a} -> {b}')


if __name__ == '__main__':
    main()
