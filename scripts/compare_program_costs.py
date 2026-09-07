#!/usr/bin/env python3
"""Compare two frozen bench_program_cost binaries with paired process runs.

The caller supplies a disposable local PostgreSQL Docker container and its
localhost URL in NEOGRAPH_COST_POSTGRES_URL. Each database is created uniquely
and only that newly created database is dropped. No application database is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.parse import urlsplit, urlunsplit
import uuid

from run_program_costs import stats, summarize, write


def command(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, timeout=180, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} failed with exit {result.returncode}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--postgres-container')
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--iterations', type=int, default=12)
    parser.add_argument('--warmup', type=int, default=3)
    args = parser.parse_args()
    if not 3 <= args.repeats <= 15 or not 5 <= args.iterations <= 100 or not 1 <= args.warmup <= 20:
        parser.error('repeats: 3..15; iterations: 5..100; warmup: 1..20')
    binaries = {'before': args.before.resolve(), 'after': args.after.resolve()}
    parts = None
    if args.postgres_container:
        parts = urlsplit(os.environ.get('NEOGRAPH_COST_POSTGRES_URL', ''))
        if parts.scheme not in {'postgres', 'postgresql'} or parts.hostname not in {'127.0.0.1', 'localhost'}:
            parser.error('a localhost NEOGRAPH_COST_POSTGRES_URL is required')
        ports = command(['docker', 'port', args.postgres_container, '5432/tcp'])
        if not any(line.rsplit(':', 1)[-1] == str(parts.port or 5432) for line in ports.splitlines()):
            parser.error('URL port does not match the supplied container')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out/'raw').mkdir()
    metadata = {'schema_version': 1, 'status': 'running', 'repeats': args.repeats,
                'iterations': args.iterations, 'warmup': args.warmup,
                'method': 'serial paired processes; AB/BA and forward/reverse case order alternate by repetition',
                'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'binary_sha256': {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in binaries.items()}}
    write(out/'metadata.json', metadata)
    cases = [('direct-0', ['--case', 'direct']),
             ('memory-cpp-0', ['--case', 'lifecycle', '--mode', 'cpp'])]
    backends = ['memory', 'sqlite'] + (['postgres'] if parts else [])
    for backend in backends:
        for payload, commands in [(0, 1), (65536, 1), (0, 16)]:
            cases.append((f'{backend}-javascript-{payload}-commands-{commands}',
                          ['--case', 'lifecycle', '--backend', backend,
                           '--payload-bytes', str(payload), '--commands', str(commands)]))
    if parts:
        cases.append(('postgres-cpp-0', ['--case', 'lifecycle', '--backend', 'postgres', '--mode', 'cpp']))
    write(out/'cases.json', cases)
    records = {name: {'before': [], 'after': []} for name, _ in cases}
    session = uuid.uuid4().hex[:12]
    count = 0
    try:
        for repeat in range(args.repeats):
            for name, flags in (cases if repeat % 2 == 0 else list(reversed(cases))):
                for arm in (['before', 'after'] if repeat % 2 == 0 else ['after', 'before']):
                    db = None
                    env = os.environ.copy()
                    invocation = [str(binaries[arm]), *flags, '--iterations', str(args.iterations),
                                  '--warmup', str(args.warmup)]
                    if name.startswith('sqlite-'):
                        invocation += ['--storage', str(out/f'{name}-{arm}-{repeat}.sqlite')]
                    elif name.startswith('postgres-'):
                        db = f'ng_pair_{session}_{count}'
                        command(['docker', 'exec', args.postgres_container, 'createdb', '-U', 'postgres', db])
                        env['NEOGRAPH_COST_POSTGRES_URL'] = urlunsplit(parts._replace(path='/'+db))
                    try:
                        started = time.monotonic()
                        result = subprocess.run(invocation, env=env, text=True, capture_output=True, timeout=180)
                        elapsed = time.monotonic()-started
                        record_path = out/'raw'/f'{name}-{arm}-{repeat}.json'
                        if result.returncode:
                            error = result.stderr
                            if env.get('NEOGRAPH_COST_POSTGRES_URL'):
                                error = error.replace(env['NEOGRAPH_COST_POSTGRES_URL'], '[redacted]')
                            write(record_path, {'status': 'failed', 'returncode': result.returncode, 'stderr': error})
                            raise RuntimeError(f'{name}/{arm} failed; see {record_path}')
                        record = json.loads(result.stdout)
                        if record.get('status') != 'ok' or record.get('build_type') != 'Release':
                            raise RuntimeError('comparison requires successful Release measurements')
                        record.update(arm=arm, repeat=repeat, process_wall_seconds=elapsed)
                        write(record_path, record)
                        records[name][arm].append(record)
                        print(f'{repeat+1}/{args.repeats} {name} {arm}: {elapsed:.2f}s', flush=True)
                    finally:
                        if db:
                            command(['docker', 'exec', args.postgres_container, 'dropdb', '-U', 'postgres', db])
                    count += 1
        summary = {}
        for name, arms in records.items():
            pairs = [stats([s['total_us'] for s in a['samples']])['median'] /
                     stats([s['total_us'] for s in b['samples']])['median']
                     for a, b in zip(arms['after'], arms['before'])]
            summary[name] = {'before': summarize(arms['before']), 'after': summarize(arms['after']),
                             'paired_after_over_before': stats(pairs), 'paired_ratios': pairs}
        write(out/'summary.json', summary)
        metadata['status'] = 'complete'
    except BaseException as error:
        metadata['status'] = 'incomplete'
        metadata['failure_type'] = type(error).__name__
        raise
    finally:
        metadata['completed_processes'] = count
        metadata['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        write(out/'metadata.json', metadata)


if __name__ == '__main__':
    main()
