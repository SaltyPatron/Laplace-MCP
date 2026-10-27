#!/usr/bin/env python3
"""Package and exercise the modular MCP and Engine from a declared installation."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


CONTROLLER = 'libexec/laplace-mcp-deployment.py'
BUILD_CACHE_SCHEMA = 'laplace.modular-build/v1'


@contextmanager
def build_cache_lock(root):
    # Packaging is Linux-only. Keep the protocol helpers importable on Windows.
    import fcntl
    root = Path(root)
    if not root.is_absolute() or root == Path('/') or any(p.is_symlink() for p in (root, *root.parents)):
        raise ValueError('Build cache must be an absolute, unlinked directory')
    root.mkdir(parents=True, exist_ok=True)
    lock_path = root / '.package.lock'
    if lock_path.is_symlink():
        raise ValueError('Build cache lock must not be a link')
    descriptor = os.open(lock_path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o660)
    with os.fdopen(descriptor, 'a') as lock:
        if os.fstat(lock.fileno()).st_uid == os.geteuid():
            os.fchmod(lock.fileno(), 0o660)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('Another modular package build owns ' + str(root)) from error
        yield


def prune_build_cache(root, current, keep=3):
    """Under build_cache_lock, retain the current build and the newest other builds."""
    root, current = Path(root), Path(current)
    if (keep < 1 or not root.is_absolute() or root == Path('/')
            or any(p.is_symlink() for p in (root, *root.parents))
            or current.parent != root or not current.is_dir() or current.is_symlink()):
        raise ValueError('Invalid build cache retention boundary')
    candidates = []
    for path in root.iterdir():
        if path.is_symlink() or not path.is_dir() or not re.fullmatch(r'[0-9a-f]{64}', path.name):
            continue
        marker = path / 'build-cache.json'
        if marker.is_symlink() or not marker.is_file():
            continue
        try:
            receipt = json.loads(marker.read_text())
        except (ValueError, OSError):
            continue
        if receipt.get('schema') == BUILD_CACHE_SCHEMA and receipt.get('identity') == path.name:
            candidates.append((marker.stat().st_mtime_ns, path))
    others = sorted((entry for entry in candidates if entry[1] != current), reverse=True)
    removed = []
    for _, path in others[max(0, keep - 1):]:
        if path.resolve().parent != root.resolve():
            raise ValueError('Build cache entry escaped its directory: ' + str(path))
        receipt = json.loads((path / 'build-cache.json').read_text())
        revisions = receipt.get('revisions')
        if not isinstance(revisions, dict) or not revisions:
            continue
        clean = True
        for repo, revision in revisions.items():
            if not re.fullmatch(r'Laplace-[A-Za-z0-9_.-]+', repo):
                clean = False
                break
            source = path / 'sources' / repo
            if source.is_symlink() or not (source / '.git').is_dir():
                clean = False
                break
            command = ['git', '--no-optional-locks', '-c', 'safe.directory=' + str(source), '-C', source]
            try:
                if (run([*command, 'rev-parse', 'HEAD'], capture_output=True).stdout.strip() != revision
                        or run([*command, 'status', '--porcelain'], capture_output=True).stdout.strip()):
                    clean = False
                    break
            except (OSError, subprocess.CalledProcessError):
                clean = False
                break
        if not clean:
            continue
        shutil.rmtree(path)
        removed.append(path.name)
    return removed


class PackageUpgradeRequired(ValueError):
    """An older package lacks the code needed to operate its own protocol."""


def run(args, **kwargs):
    return subprocess.run([str(v) for v in args], check=True, text=True, **kwargs)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        os.fchmod(stream.fileno(), 0o644)
        json.dump(value, stream, indent=2)
        stream.write('\n')
        name = stream.name
    os.replace(name, path)


def load(path):
    value = json.loads(Path(path).read_text())
    if value.get('schema') != 'laplace.modular-installation/v1':
        raise ValueError('Unknown modular installation schema')
    for key in ('source', 'build', 'prefix', 'state', 'tier0', 'flags', 'highway', 'grammars', 'ucd'):
        item = value[key]
        if not isinstance(item, str) or not Path(item).is_absolute() or '..' in Path(item).parts or any(c in item for c in '\n\r\0'):
            raise ValueError('Invalid absolute installation path: ' + key)
    for key in ('threads', 'build_jobs', 'http_port', 'connection_limit'):
        if type(value[key]) is not int or value[key] <= 0:
            raise ValueError('Invalid installation allocation: ' + key)
    if value['bind'] != '127.0.0.1' or value['http_port'] > 65535:
        raise ValueError('The modular backend requires a loopback listener behind its authenticated gateway')
    return value


def runtime_environment(config, release):
    # Explicit installation state, independent of the builder's shell and checkout.
    return {
        'PATH': '/usr/local/bin:/usr/bin:/bin', 'LANG': 'C.UTF-8',
        'LAPLACE_CONNINFO': config['connection'],
        'LAPLACE_TIER0': config['tier0'], 'LAPLACE_FLAGS': config['flags'],
        'LAPLACE_HIGHWAY': config['highway'], 'LAPLACE_GRAMMARS': config['grammars'],
        'LAPLACE_UCD': config['ucd'], 'LAPLACE_WORK': config['state'],
        'LAPLACE_ENGINE': str(Path(release) / 'bin/laplace'),
        'LAPLACE_RECIPES': str(Path(release) / 'share/recipes'),
        'LAPLACE_MCP_HOST': config['bind'], 'LAPLACE_MCP_PORT': str(config['http_port']),
        'OMP_NUM_THREADS': str(config['threads']), 'MKL_NUM_THREADS': '1',
        'LAPLACE_BATCH_MB': str(config['batch_mb']),
    }


def package(config, output):
    if os.geteuid() == 0:
        raise ValueError('Build and package as the declared deployment operator, never root')
    os.umask(0o002)
    with build_cache_lock(config['build']):
        _package(config, output)


def _package(config, output):
    source, build, prefix = (Path(config[k]) for k in ('source', 'build', 'prefix'))
    revisions = {}
    for repo in ('Laplace-MCP', 'Laplace-Engine', 'Laplace-Native', 'Laplace-Operations'):
        checkout = source / repo
        if run(['git', '-C', checkout, 'status', '--porcelain', '--untracked-files=no'], capture_output=True).stdout.strip():
            raise ValueError('Commit the selected source before packaging: ' + repo)
        revisions[repo] = run(['git', '-C', checkout, 'rev-parse', 'HEAD'], capture_output=True).stdout.strip()
    build_identity = hashlib.sha256(json.dumps({'source': str(source.resolve()), 'builder': os.geteuid(), 'revisions': revisions}, sort_keys=True).encode()).hexdigest()
    build = build / build_identity
    atomic_json(build / 'build-cache.json', {'schema': BUILD_CACHE_SCHEMA, 'identity': build_identity,
                                           'revisions': revisions})
    # Build exact Git trees. Untracked developer files and the old in-source
    # serving binaries stay untouched and cannot enter the installed package.
    snapshot = build / 'sources'
    snapshot.mkdir(parents=True, exist_ok=True)
    for repo, revision in revisions.items():
        destination = snapshot / repo
        if not destination.exists():
            run(['git', 'clone', '--quiet', '--shared', '--no-checkout', source / repo, destination])
            run(['git', '-C', destination, 'checkout', '--quiet', '--detach', revision])
        if run(['git', '-C', destination, 'rev-parse', 'HEAD'], capture_output=True).stdout.strip() != revision or run(['git', '-C', destination, 'status', '--porcelain'], capture_output=True).stdout.strip():
            raise ValueError('Build source snapshot was modified: ' + repo)
    source = snapshot
    environment = dict(os.environ, LAPLACE_SRC=str(source), LAPLACE_BUILD=str(build),
                       LAPLACE_NATIVE=str(source / 'Laplace-Native'),
                       LAPLACE_ENGINE_SRC=str(source / 'Laplace-Engine/src'),
                       LAPLACE_OPERATIONS=str(source / 'Laplace-Operations'),
                       LAPLACE_CONNINFO='', OMP_NUM_THREADS=str(config['threads']), MKL_NUM_THREADS='1')
    release_root = prefix / 'releases'
    # Setup provisions a shared setgid prefix. Existing release directories may
    # belong to the operator; a group-authorized runner can publish there but
    # cannot chmod another user's directory. New directories inherit its group.
    release_root.mkdir(parents=True, exist_ok=True)
    # Native's paths/flags consume the selected Operations configuration. The caller
    # supplies the toolchain session; service processes never inherit that session.
    with tempfile.TemporaryDirectory(prefix='.package-', dir=release_root) as temporary:
        stage = Path(temporary) / 'root'
        for repo in ('Laplace-Engine', 'Laplace-MCP'):
            tree = build / repo
            run(['cmake', '-S', source / repo, '-B', tree, '-G', 'Ninja',
                 '-DCMAKE_C_COMPILER=icx', '-DCMAKE_CXX_COMPILER=icpx',
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_INSTALL_LIBDIR=lib',
                 '-DLAPLACE_CONNINFO='], env=environment)
            run(['cmake', '--build', tree, '--parallel', config['build_jobs']], env=environment)
            run(['cmake', '--install', tree, '--prefix', stage], env=environment)
        controller = stage / CONTROLLER
        controller.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / 'Laplace-MCP/deployment.py', controller)
        for directory in ('recipes', 'firmware'):
            shutil.copytree(source / 'Laplace-Engine' / directory, stage / 'share' / directory)
        dependencies = {}
        for relative in ('bin/lpm', 'bin/laplace', 'lib/liblpm.so'):
            report = run(['ldd', stage / relative], capture_output=True).stdout
            if 'not found' in report:
                raise ValueError('Unresolved runtime dependency: ' + relative)
            for line in report.splitlines():
                fields = line.split()
                if '=>' in fields and len(fields) >= 3 and fields[2].startswith('/'):
                    dependency = Path(fields[2]).resolve()
                    if not dependency.is_relative_to(stage):
                        dependencies[fields[2]] = digest(dependency)
        files = {str(p.relative_to(stage)): digest(p) for p in sorted(stage.rglob('*')) if p.is_file()}
        receipt = {'schema': 'laplace.modular-package/v1', 'revisions': revisions,
                   'files': files, 'host_dependencies': dependencies}
        identity = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
        release = release_root / identity
        if release.exists():
            verify_package(release)
        else:
            atomic_json(stage / 'package.json', receipt)
            stage.rename(release)
        atomic_json(output, {'release': str(release), 'package_id': identity, 'revisions': revisions})
    removed = prune_build_cache(build.parent, build)
    if removed:
        print('Removed ' + str(len(removed)) + ' expired modular build caches', flush=True)


def verify_package(release):
    release = Path(release)
    receipt = json.loads((release / 'package.json').read_text())
    if receipt.get('schema') != 'laplace.modular-package/v1':
        raise ValueError('Unknown package schema')
    identity = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
    if release.name != identity:
        raise ValueError('Package identity differs')
    for relative, expected in receipt['files'].items():
        path = release / relative
        if not path.resolve().is_relative_to(release.resolve()) or digest(path) != expected:
            raise ValueError('Package file differs: ' + relative)
    for path, expected in receipt['host_dependencies'].items():
        if digest(path) != expected:
            raise ValueError('Installed dependency differs: ' + path)
    if CONTROLLER not in receipt['files']:
        raise PackageUpgradeRequired('Modular package lacks its deployment controller; rebuild a compatible release')
    return receipt


def service_token(config):
    """The installed listener's token: LAPLACE_MCP_TOKEN, else the file setup made beside the environment file."""
    value = os.environ.get('LAPLACE_MCP_TOKEN', '')
    if not value:
        path = Path(config.get('token_file') or Path(config['environment_file']).parent / 'secrets/modular-mcp.env')
        if not path.is_file():
            raise ValueError('The listener token is absent; setup makes it: ' + str(path))
        for line in path.read_text().splitlines():
            if line.startswith('LAPLACE_MCP_TOKEN='):
                value = line.partition('=')[2].strip().strip('"\'')
                break
    if len(value) < 32 or any(c.isspace() for c in value):
        raise ValueError('The listener token is malformed')
    return value


def request(base, path, data=None, token=None, timeout=30):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(), headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        if response.status != 200:
            raise ValueError('Unexpected service status: ' + str(response.status))
        return json.load(response)


def refused(base, path, data, token):
    try:
        request(base, path, data, token)
    except urllib.error.HTTPError as error:
        try:
            return error.code == 401
        finally:
            error.close()
    return False


def verify_http(base, token):
    # The listener refuses a caller without the token, and one with another, before anything it serves.
    for path, data in (('/v1/surface', None), ('/mcp', {'jsonrpc': '2.0', 'id': 1, 'method': 'tools/list'})):
        if not refused(base, path, data, None) or not refused(base, path, data, 'x' * 64):
            raise ValueError('The listener served a request without its token: ' + path)
    surface = request(base, '/v1/surface', token=token)
    if surface.get('service') != 'Laplace-MCP':
        raise ValueError('Listener is not the modular MCP service')
    search = request(base, '/v1/search', {'input': 'Sherlock Holmes'}, token)
    if not all(isinstance(search.get(k), list) for k in ('physicality', 'claims', 'occurrences', 'attestations', 'constituents')):
        raise ValueError('Database search did not return its typed result collections')
    if not search['constituents']:
        raise ValueError('Configured knowledge did not resolve the declared installation readback')
    initialized = request(base, '/mcp', {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
        'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'installation', 'version': '1'}}}, token)
    if 'result' not in initialized or initialized['result'].get('serverInfo', {}).get('name') != 'laplace':
        raise ValueError('MCP initialization failed')
    engine = {}
    for operation, arguments in (
        ('text', {'input': 'Sherlock Holmes'}),
        ('hop', {'input': 'Sherlock Holmes'}),
        ('translate', {'input': 'dog', 'from': 'en', 'to': 'de'}),
        ('degrees', {'input': 'Sherlock Holmes', 'other': 'Sherlock Holmes'}),
        ('pull', {'input': 'Sherlock Holmes'}),
    ):
        result = request(base, '/v1/' + operation, arguments, token)
        if result.get('exit') != 0 or not isinstance(result.get('log'), str) or not result['log']:
            raise ValueError('Installed Engine operation failed: ' + operation)
        engine[operation] = {'exit': result['exit'], 'output_bytes': len(result['log'].encode())}
    return {'surface': 'passed', 'database_search': {k: len(search[k]) for k in ('physicality', 'claims', 'occurrences', 'attestations', 'constituents')},
            'mcp_initialize': 'passed', 'unauthenticated': 'refused', 'engine_operations': engine,
            'scope': 'Installed protocol, Engine operations and selected native/database readback; no ingestion or whole-product capability claim'}


@contextmanager
def candidate_listener(command, environment, work, timeout=10):
    """Own one candidate process through authenticated verification and teardown."""
    environment = dict(environment)
    token = secrets.token_hex(32)
    environment['LAPLACE_MCP_TOKEN'] = token
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    base = 'http://127.0.0.1:' + str(port)
    with tempfile.TemporaryFile(mode='w+', encoding='utf-8', errors='replace') as log:
        process = subprocess.Popen([*[str(item) for item in command], 'serve', '127.0.0.1', str(port)],
                                   env=environment, cwd=work, stdout=log, stderr=log)

        def failure(message):
            log.seek(0, os.SEEK_END)
            log.seek(max(0, log.tell() - 8192))
            detail = log.read()
            for value in (token, environment.get('LAPLACE_CONNINFO')):
                if value:
                    detail = detail.replace(value, '[redacted]')
            return ValueError(f'{message} (exit={process.poll()}): ' + detail.strip())

        try:
            deadline = time.monotonic() + timeout
            while True:
                if process.poll() is not None:
                    raise failure('Candidate service exited before accepting requests')
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise failure('Candidate service startup timed out')
                try:
                    if request(base, '/health', timeout=min(.2, remaining)) != {'ok': True}:
                        raise failure('Candidate service returned an invalid health response')
                    break
                except urllib.error.HTTPError as error:
                    error.close()
                    raise failure('Candidate service refused its health request') from None
                except (urllib.error.URLError, TimeoutError, ConnectionError):
                    time.sleep(min(.1, max(0, deadline - time.monotonic())))
            yield base, token
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def probe(config, release):
    verify_package(release)
    environment = runtime_environment(config, release)
    command = [Path(release) / 'bin/lpm']
    run([*command, 'check'], env=environment, cwd=config['state'])
    with candidate_listener(command, environment, config['state']) as (base, token):
        return verify_http(base, token)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('package', 'probe', 'verify'))
    parser.add_argument('--config', required=True)
    parser.add_argument('--release')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    config = load(args.config)
    if args.operation == 'package':
        package(config, args.output)
    else:
        release = Path(args.release) if args.release else (Path(config['prefix']) / 'current').resolve()
        verify_package(release)
        result = probe(config, release) if args.operation == 'probe' else verify_http('http://' + config['bind'] + ':' + str(config['http_port']), service_token(config))
        atomic_json(args.output, result)


if __name__ == '__main__':
    main()
