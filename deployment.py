#!/usr/bin/env python3
"""Package and exercise the modular MCP and Engine from a declared installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request


def run(args, **kwargs):
    return subprocess.run([str(v) for v in args], check=True, text=True, **kwargs)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
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
    source, build, prefix = (Path(config[k]) for k in ('source', 'build', 'prefix'))
    revisions = {}
    for repo in ('Laplace-MCP', 'Laplace-Engine', 'Laplace-Native', 'Laplace-Operations'):
        checkout = source / repo
        if run(['git', '-C', checkout, 'status', '--porcelain'], capture_output=True).stdout.strip():
            raise ValueError('Commit the selected source before packaging: ' + repo)
        revisions[repo] = run(['git', '-C', checkout, 'rev-parse', 'HEAD'], capture_output=True).stdout.strip()
    environment = dict(os.environ, LAPLACE_SRC=str(source), LAPLACE_BUILD=str(build),
                       LAPLACE_NATIVE=str(source / 'Laplace-Native'),
                       LAPLACE_ENGINE_SRC=str(source / 'Laplace-Engine/src'),
                       LAPLACE_OPERATIONS=str(source / 'Laplace-Operations'),
                       LAPLACE_CONNINFO='', OMP_NUM_THREADS=str(config['threads']), MKL_NUM_THREADS='1')
    release_root = prefix / 'releases'
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
        import shutil
        shutil.copytree(source / 'Laplace-Engine/recipes', stage / 'share/recipes')
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
                        dependencies[str(dependency)] = digest(dependency)
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
    return receipt


def request(base, path, data=None):
    req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(),
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=30) as response:
        if response.status != 200:
            raise ValueError('Unexpected service status: ' + str(response.status))
        return json.load(response)


def verify_http(base):
    surface = request(base, '/v1/surface')
    if surface.get('service') != 'Laplace-MCP':
        raise ValueError('Listener is not the modular MCP service')
    search = request(base, '/v1/search', {'input': 'Sherlock Holmes'})
    if not all(isinstance(search.get(k), list) for k in ('physicality', 'claims', 'occurrences', 'attestations', 'constituents')):
        raise ValueError('Database search did not return its typed result collections')
    if not search['physicality'] or not search['constituents']:
        raise ValueError('Configured knowledge did not resolve the declared installation readback')
    initialized = request(base, '/mcp', {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
        'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'installation', 'version': '1'}}})
    if 'result' not in initialized or initialized['result'].get('serverInfo', {}).get('name') != 'laplace':
        raise ValueError('MCP initialization failed')
    return {'surface': 'passed', 'database_search': {k: len(search[k]) for k in ('physicality', 'claims', 'occurrences', 'attestations', 'constituents')},
            'mcp_initialize': 'passed', 'scope': 'Installed protocol and selected native/database readback; no ingestion or whole-product capability claim'}


def probe(config, release):
    verify_package(release)
    environment = runtime_environment(config, release)
    run([Path(release) / 'bin/lpm', 'check'], env=environment, cwd=config['state'])
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryFile(mode='w+') as log:
        process = subprocess.Popen([str(Path(release) / 'bin/lpm'), 'serve', '127.0.0.1', str(port)],
                                   env=environment, cwd=config['state'], stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise ValueError('Candidate service exited before accepting requests')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.2):
                        break
                except OSError:
                    time.sleep(.1)
            return verify_http('http://127.0.0.1:' + str(port))
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


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
        result = probe(config, release) if args.operation == 'probe' else verify_http('http://' + config['bind'] + ':' + str(config['http_port']))
        atomic_json(args.output, result)


if __name__ == '__main__':
    main()
