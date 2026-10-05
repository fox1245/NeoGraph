#!/usr/bin/env python3
"""Post-pytest, native installed-wheel LGPL replacement qualification.

Run with the wheel's interpreter, after the unchanged full installed-wheel suite:
  python {package}/cmake/test-wheel-lgpl-replacement.py --package {package}
Requires a disposable, exclusively owned writable installation in native
manylinux_2_34 (glibc exactly 2.34), not a build-tree import. Container packages:
 gcc make binutils glibc-devel kernel-headers patch autoconf automake libtool
 pkgconf-pkg-config perl-core openssl; patchelf and pytest must also be available.
No downloads, privilege changes or wheel-archive writes are performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import runpy
import shutil
import signal
import struct
import subprocess
import sys
import sysconfig
import tarfile
import tempfile

ASSET = 'neograph-0.13.0-linux-lgpl-library-only-source.tar.gz'
DIGEST = 'af69f9448a3e14cd35a9814e96545fe4a13fd1fa5eebb3e3f9922c25f7e13c21'
ROOT = ASSET[:-7]
ENV = os.environ.copy()
for _name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'PYTHONPATH', 'PYTHONHOME', 'PYTHONOPTIMIZE', 'PYTEST_ADDOPTS'):
    ENV.pop(_name, None)
# Kerberos must not consume host credential/configuration files.
ENV.update(KRB5_CONFIG='/dev/null', KRB5CCNAME='FILE:/dev/null', KRB5_KTNAME='FILE:/dev/null')
ENV.update(PYTHONDONTWRITEBYTECODE='1', PYTEST_DISABLE_PLUGIN_AUTOLOAD='1')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def command(args, cwd, timeout=120, input_text=None):
    process = subprocess.Popen([str(a) for a in args], cwd=str(cwd), env=ENV,
                               stdin=subprocess.PIPE if input_text is not None else subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        out, err = process.communicate(input_text, timeout=timeout)
    except BaseException:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        raise
    print(out, end='', flush=True)
    print(err, end='', file=sys.stderr, flush=True)
    require(process.returncode == 0, 'Command failed (%s): %s' % (process.returncode, args))
    return out, err


def unpack(asset, temp):
    require(digest(asset) == DIGEST, 'Unverified source asset: update the qualified source/hash mapping; no fallback')
    with tarfile.open(str(asset), 'r:gz') as archive:
        members = archive.getmembers()
        require(len(members) <= 2000 and sum(m.size for m in members) <= 100 * 1024 * 1024,
                'Source archive exceeds bounded extraction limits')
        entries = {}
        for member in members:
            name = PurePosixPath(member.name)
            require(name.parts and not name.is_absolute() and '..' not in name.parts and name.parts[0] == ROOT,
                    'Unsafe source member: ' + member.name)
            canonical = name.as_posix()
            require(canonical not in entries and (member.isfile() or member.isdir() or member.issym()),
                    'Duplicate or special source member: ' + member.name)
            entries[canonical] = member
            require(not member.mode & 0o7000, 'Privileged source mode')
        for name in entries:
            for parent in PurePosixPath(name).parents:
                if parent.as_posix() in entries:
                    require(entries[parent.as_posix()].isdir(), 'Non-directory source member ancestor')
        manifest_member = entries.get(ROOT + '/MANIFEST.json')
        require(manifest_member is not None and manifest_member.isfile(), 'Missing regular source manifest')
        with archive.extractfile(manifest_member) as stream:
            manifest = json.load(stream)
        declarations = {}
        for entry in manifest['materialFiles']:
            if entry['type'] == 'symlink':
                relative = PurePosixPath(entry['file'])
                require(relative.parts and not relative.is_absolute() and '..' not in relative.parts,
                        'Unsafe manifest symlink declaration')
                name = ROOT + '/' + relative.as_posix()
                require(name not in declarations, 'Duplicate manifest symlink declaration')
                declarations[name] = entry['target']
        links = {name: member.linkname for name, member in entries.items() if member.issym()}
        require(links == declarations, 'Source symlinks do not match declared manifest')

        def destination(name, target):
            relative = PurePosixPath(target)
            require(target and not relative.is_absolute(), 'Absolute or empty source symlink target')
            parts = list(PurePosixPath(name).parent.parts)
            for part in relative.parts:
                if part == '..':
                    require(len(parts) > 1, 'Source symlink escapes archive root')
                    parts.pop()
                elif part != '.':
                    parts.append(part)
            resolved = PurePosixPath(*parts).as_posix()
            require(resolved in entries, 'Source symlink target is not delivered')
            return resolved

        for name in links:
            seen = set()
            current = name
            while current in links:
                require(current not in seen, 'Cyclic source symlink')
                seen.add(current)
                current = destination(current, links[current])
            require(entries[current].isfile(), 'Source symlink must resolve to a delivered regular file')
        # Materialize directories/regular files first, never writing through a link.
        for name, member in entries.items():
            if member.issym():
                continue
            target = temp.joinpath(*PurePosixPath(name).parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as src, target.open('xb') as dst:
                    shutil.copyfileobj(src, dst)
                target.chmod(0o700 if member.mode & 0o111 else 0o600)
        for name, target in links.items():
            path = temp.joinpath(*PurePosixPath(name).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target)
    root = temp / ROOT
    checks = {}
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        require(match is not None, 'Malformed source SHA256SUMS')
        expected, name = match.groups()
        relative = PurePosixPath(name)
        require(relative.parts and not relative.is_absolute() and '..' not in relative.parts
                and relative.as_posix() == name and name not in checks,
                'Unsafe or duplicate checksum path')
        require(ROOT + '/' + name in entries and entries[ROOT + '/' + name].isfile(),
                'Checksum entry is not a delivered regular file')
        checks[name] = expected
        require(digest(root / name) == expected, 'Source checksum mismatch: ' + name)
    files = {PurePosixPath(name).relative_to(ROOT).as_posix()
             for name, member in entries.items() if member.isfile()}
    require(files == set(checks) | {'SHA256SUMS'}, 'Unlisted or missing regular source archive files')
    actual_links = {ROOT + '/' + p.relative_to(root).as_posix(): os.readlink(p)
                    for p in root.rglob('*') if p.is_symlink()}
    require(actual_links == declarations, 'Extracted source symlink declarations changed')
    for name in declarations:
        path = temp.joinpath(*PurePosixPath(name).parts)
        require(path.resolve(strict=True).is_relative_to(root), 'Extracted source symlink escaped root')
    for name in files:
        p = root / name
        require(not any(part in ('test', 'tests', '.git') for part in PurePosixPath(name).parts),
                'Excluded test/custody source member: ' + name)
        require(not name.endswith(('.rpm', '.whl', '.tar.gz', '.tar.xz', '.zip', '.so', '.o', '.a')),
                'Non-source container/build member: ' + name)
        with p.open('rb') as stream:
            require(stream.read(8)[:4] != b'\x7fELF', 'Compiled ELF in source archive: ' + name)
    scope = json.loads((root / 'packaging/SOURCE-PACKAGING-SCOPE.json').read_text())
    for entry in scope['preservedLibraryInputs']:
        require(digest(root / entry['file']) == entry['sha256'], 'Changed preferred library input')
    for entry in scope['packagingControls']:
        require(digest(root / entry['file']) == entry['deliveredSHA256'], 'Changed build control')
    return root


def inspect(path, cwd):
    with path.open('rb') as stream:
        header = stream.read(20)
    require(header[:6] == b'\x7fELF\x02\x01', 'Not ELF64 little endian: ' + str(path))
    dynamic, _ = command(['readelf', '-d', path], cwd)
    symbols, _ = command(['readelf', '--dyn-syms', '--wide', path], cwd)
    match = re.search(r'\(SONAME\).*\[([^\]]+)\]', dynamic)
    require(match is not None, 'Missing SONAME')
    exports = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 8 or not re.fullmatch(r'\d+:', fields[0]):
            continue
        # Local/section entries are not public ABI, even if readelf prints a
        # version suffix. Keep every externally visible definition, including
        # weak/unique, protected and unversioned symbols.
        if (fields[3] not in ('SECTION', 'FILE') and fields[4] in ('GLOBAL', 'WEAK', 'UNIQUE')
                and fields[5] in ('DEFAULT', 'PROTECTED') and fields[6] != 'UND'):
            exports.add(fields[7])
    floors = [tuple(map(int, value.split('.'))) for value in re.findall(r'@GLIBC_([0-9.]+)', symbols)]
    require(all(value <= (2, 34) for value in floors),
            'GLIBC floor exceeds 2.34: %s (observed %s)' % (path, max(floors, default=(0, 0))))
    return {'machine': struct.unpack_from('<H', header, 18)[0], 'soname': match.group(1),
            'exports': exports, 'versioned_exports': {name for name in exports if '@' in name},
            'needed': re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic),
            'rpath': re.findall(r'\((?:RPATH|RUNPATH)\).*\[([^\]]*)\]', dynamic),
            'floor': max(floors, default=(0, 0))}


def cold(root, libs, mode, output, baseline=None):
    argv = [str(root / 'qualification/replacement-smoke.py'), str(libs), mode, str(output)]
    if baseline:
        argv.append(str(baseline))
    sys.argv = argv
    runpy.run_path(argv[0], run_name='__main__')
    import neograph_engine as ng
    require(Path(ng.__file__).resolve().is_relative_to(libs.parent) if sys.version_info >= (3, 9)
            else False, 'Engine did not load from installed wheel')
    class Work(ng.GraphNode):
        def get_name(self):
            return 'work'
        def run(self, _input):
            return [ng.ChannelWrite('value', 42)]
    ng.NodeFactory.register_type('lgpl_probe_work', lambda name, config, ctx: Work())
    definition = {'name': 'replacement-graph', 'channels': {'value': {'reducer': 'overwrite', 'initial': 0}},
                  'nodes': {'work': {'type': 'lgpl_probe_work'}},
                  'edges': [{'from': ng.START_NODE, 'to': 'work'}, {'from': 'work', 'to': ng.END_NODE}]}
    graph = ng.GraphEngine.compile(definition, ng.NodeContext()).run(ng.RunConfig(thread_id='replacement', input={}))
    require(graph.output['channels']['value']['value'] == 42, 'Graph computation changed')
    registry = ng.ProgramRegistryBuilder()
    registry.add_registered_node('lgpl_probe_work', '1.0.0', 'sha256:' + '1' * 64)
    registry.add_registered_reducer('overwrite', '1.0.0', 'sha256:' + '2' * 64)
    budget = ng.ProgramRunBudget()
    for key, value in {'wall_time_ms': 10000, 'model_tokens': 1000, 'monetary_microunits': 1000,
                       'max_concurrency': 2, 'max_program_operations': 32, 'max_core_steps': 20,
                       'max_dynamic_compiles': 0, 'max_child_depth': 1, 'max_total_children': 4}.items():
        setattr(budget, key, value)
    ceiling = ng.ProgramRunBudget()
    for key in ('wall_time_ms', 'model_tokens', 'monetary_microunits', 'max_concurrency',
                'max_program_operations', 'max_core_steps', 'max_child_depth', 'max_total_children'):
        setattr(ceiling, key, getattr(budget, key))
    ceiling.max_dynamic_compiles = 2
    source = ng.ProgramSource.from_javascript('replacement.js', '''
export function define() {
 const graph = ng.graph("main");
 graph.channel("value", {reducer: "overwrite", initial: 0});
 graph.node("work", {type: "lgpl_probe_work"}); graph.entry("work"); graph.exit("work"); return graph;
}
export function* main(input) { return yield ng.callCore("main", input, "replacement:main"); }
''')
    host = ng.LocalProgramHost(registry.build(), 'replacement-owner', ceiling, 'replacement/v1')
    version = host.compile_admit(source, budget)
    program = host.run(version, {}, budget, 'replacement-program')
    require(program.status == ng.ProgramTerminalStatus.Completed and program.execution_trace == ['work']
            and program.output['channels']['value']['value'] == 42, 'Program computation changed')
    result = json.loads(output.read_text())
    require('1.6.3' in result['libraryVersion'], 'Unexpected observed keyutils version')
    krbfiles = list(libs.glob('libkrb5-*.so.*'))
    require(len(krbfiles) == 1, 'Ambiguous installed Kerberos library')
    mappings = Path('/proc/self/maps').read_text().splitlines()
    loaded_krb = {line.split()[-1] for line in mappings if '/libkrb5-' in line}
    require(loaded_krb == {str(krbfiles[0])}, 'Kerberos not loaded from exact installed library')
    result['kerberos'] = {'context': 'initialized-and-freed', 'mappedLibrary': str(krbfiles[0])}
    require(result['keyctlResult'] == -1 and result['keyctlErrno'] in (1, 22, 126),
            'Invalid informational keyctl did not fail safely')
    result['combinedWork'] = {'graphValue': 42, 'programValue': 42, 'programTrace': list(program.execution_trace)}
    if baseline:
        require(result['combinedWork'] == json.loads(baseline.read_text())['combinedWork'], 'Combined work changed')
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    args = parser.parse_args()
    package = args.package.resolve()
    require(sys.implementation.name == 'cpython' and (3, 9) <= sys.version_info[:2] <= (3, 13),
            'Qualification requires CPython 3.9–3.13')
    arch = platform.machine()
    require(sys.platform == 'linux' and arch in ('x86_64', 'aarch64'), 'Native Linux x86_64/aarch64 required')
    require(os.confstr('CS_GNU_LIBC_VERSION') == 'glibc 2.34', 'Build inside native glibc 2.34 environment')
    libs = Path(sysconfig.get_path('platlib')).resolve() / 'neograph_engine.libs'
    require(libs.is_dir(), 'Installed repaired wheel libraries missing')
    for tool in ('cc', 'make', 'readelf', 'patch', 'autoconf', 'automake', 'libtoolize', 'pkg-config', 'perl', 'patchelf', 'openssl'):
        require(shutil.which(tool) is not None, 'Missing prerequisite: ' + tool + ' (EL9 requires perl-core, not minimal perl)')
    # SIGTERM/SIGINT must unwind the replacement transaction, including timed builds.
    def interrupted(signum, frame):
        raise InterruptedError('Qualification interrupted by signal %s' % signum)
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, interrupted)
    with tempfile.TemporaryDirectory(prefix='neograph-lgpl-replacement-') as directory:
        temp = Path(directory)
        command(['perl', '-e', 'use v5.14; use warnings; use utf8; use open qw(:std :utf8); use if 0, "warnings"; use re; use FindBin (); use Cwd (); use File::Spec::Functions (); use POSIX (); use Exporter (); use Class::Struct (); print "Perl core prerequisites present\\n";'], temp)
        for tool, minimum in (('autoconf', (2, 69)), ('automake', (1, 14)), ('libtoolize', (2, 4, 6)), ('pkg-config', (0, 29))):
            text, _ = command([tool, '--version'], temp)
            match = re.search(r'\d+\.\d+(?:\.\d+)?', text)
            require(match and tuple(map(int, match.group().split('.'))) >= minimum, 'Insufficient prerequisite version: ' + tool)
        root = unpack(package / 'deps/redistribution' / ASSET, temp)
        mapping = json.loads((root / 'WHEEL-REPLACEMENT-MAP.json').read_text())
        tag = 'cp%d%d' % sys.version_info[:2]
        rows = [row for row in mapping['wheels'] if row['architecture'] == arch and ('-' + tag + '-' + tag + '-') in row['wheel']]
        require(len(rows) == 1, 'Unverified wheel cohort: source/mapping update required')
        originals = {}
        for record in rows[0]['libraries']:
            old = libs / Path(record['member']).name
            require(old.is_file() and not old.is_symlink() and digest(old) == record['sha256'],
                    'Unverified final dependency hash/version: source/mapping update required: ' + str(old))
            info = inspect(old, temp)
            require(info['machine'] == record['machine'] == {'x86_64': 62, 'aarch64': 183}[arch]
                    and info['soname'] == record['soname'] == old.name
                    and info['versioned_exports'] == set(record['exportedVersionedSymbols'])
                    and info['needed'] == record['needed'] and info['rpath'] == record['rpath'],
                    'Unverified observed dependency ABI/repair mapping')
            originals[old] = (record, info)
        for consumer in rows[0]['consumers']:
            path = libs.parent / consumer['member']
            require(path.is_file(), 'Missing mapped consumer: ' + str(path))
            info = inspect(path, temp)
            require(set(consumer['neededLibraries']).issubset(info['needed']) and info['rpath'] == consumer['rpath'],
                    'Consumer replacement routing changed')
        baseline, modified = temp / 'baseline.json', temp / 'modified.json'
        helper = Path(__file__).resolve()
        def smoke(mode, output):
            argv = [sys.executable, helper, '--cold', root, libs, mode, output]
            if mode == 'modified':
                argv.append(baseline)
            out, err = command(argv, temp)
            marker = 'NEOGRAPH-LGPL-LIBXCRYPT-REPLACEMENT'
            require((marker in err) == (mode == 'modified'), 'Missing/unexpected libxcrypt constructor marker')
            # Real owned localhost TLS peers; retain existing consumer-visible assertions.
            tests = package / 'bindings/python/tests'
            command([sys.executable, '-m', 'pytest', '-q', '-p', 'no:cacheprovider',
                     '--basetemp', temp / ('provider-' + mode),
                     str(tests / 'test_typed_provider.py') + '::test_prepared_request_is_consumed_once',
                     str(tests / 'test_typed_provider.py') + '::test_owned_outcome_keeps_raw_and_native_history_after_provider_collection'], temp)
        smoke('baseline', baseline)
        known, _ = command(['openssl', 'passwd', '-6', '-salt', 'neograph-salt', '-stdin'], temp,
                           input_text='neograph-lgpl-recipient-smoke\n')
        require(json.loads(baseline.read_text())['cryptHash'] == known.strip(), 'Known SHA512 password hash mismatch')
        work = temp / 'marked-build'
        command(['sh', root / 'build-shared.sh', work, '--qualification-markers'], temp, timeout=1200)
        for old, (_, info) in originals.items():
            new = work / 'output' / ('libkeyutils.so.1.10' if old.name.startswith('libkeyutils-') else 'libcrypt.so.2.0.0')
            built = inspect(new, temp)
            expected_soname = 'libkeyutils.so.1' if old.name.startswith('libkeyutils-') else 'libcrypt.so.2'
            expected = {'machine': info['machine'], 'soname': expected_soname, 'needed': info['needed']}
            mismatches = {field: {'expected': value, 'actual': built[field]}
                          for field, value in expected.items() if built[field] != value}
            if built['exports'] != info['exports']:
                mismatches['exports'] = {'missing': sorted(info['exports'] - built['exports']),
                                         'added': sorted(built['exports'] - info['exports'])}
            if built['floor'] > (2, 34):
                mismatches['floor'] = {'maximum': (2, 34), 'actual': built['floor']}
            require(not mismatches, 'Marked build ABI/SONAME/system-dependency/floor mismatch: '
                    + str(new) + ' ' + json.dumps(mismatches, sort_keys=True))
        backups = {}
        try:
            for old in originals:
                require(not old.with_name(old.name + '.before-replacement').exists()
                        and not old.with_name(old.name + '.replacement').exists(), 'Replacement leftovers/exclusive install violation')
                backup = temp / (old.name + '.original')
                shutil.copy2(old, backup)
                backups[old] = backup
            command([sys.executable, root / 'replace-libraries.py', libs, work / 'output'], temp)
            for old, (_, info) in originals.items():
                replaced = inspect(old, temp)
                require(replaced['soname'] == old.name and replaced['rpath'] == ['$ORIGIN']
                        and replaced['exports'] == info['exports'] and replaced['machine'] == info['machine'],
                        'Replacement lost exact SONAME/RPATH/ABI')
            smoke('modified', modified)
            require(json.loads(modified.read_text())['cryptHash'] == known.strip(), 'Modified known password hash mismatch')
            report = {'python': tag, 'architecture': arch, 'sourceSHA256': DIGEST,
                      'baseline': json.loads(baseline.read_text()), 'modified': json.loads(modified.read_text()),
                      'providerOracles': ['prepared request consumed once', 'owned raw/native history and replay']}
        finally:
            # Atomic restore in the installed directory, even after partial prepared-helper failure.
            for signum in (signal.SIGINT, signal.SIGTERM):
                signal.signal(signum, signal.SIG_IGN)
            for old, backup in backups.items():
                staging = old.with_name(old.name + '.replacement')
                shutil.copy2(backup, staging)
                os.replace(staging, old)
                require(digest(old) == originals[old][0]['sha256'], 'Original library restoration failed')
                saved = old.with_name(old.name + '.before-replacement')
                if saved.exists():
                    saved.unlink()
        print('EXERCISED LGPL REPLACEMENT (original installed libraries restored)')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--cold':
        values = sys.argv[2:]
        cold(Path(values[0]), Path(values[1]), values[2], Path(values[3]), Path(values[4]) if len(values) == 5 else None)
    else:
        main()
