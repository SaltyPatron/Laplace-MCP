"""Verify release integrity and real candidate-process startup/teardown.

The native listener checks require the actual lpm executable as the first argument.
They exercise health, authentication and MCP initialization without database access.
"""
import hashlib
import importlib.util
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('deployment', ROOT / 'deployment.py')
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)
BINARY = Path(sys.argv.pop(1)).resolve(strict=True)


class Deployment(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='lpm-deployment-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.environment = dict(os.environ, LAPLACE_CONNINFO=
            'host=127.0.0.1 port=1 dbname=unit_no_database user=unit connect_timeout=1')

    def test_runtime_resources_are_selected_from_the_release(self):
        release = self.work / 'immutable-release'
        config = dict(connection='selected database', tier0='/work/tier0', flags='/work/flags',
                      highway='/work/highway', grammars='/build/grammars', ucd='/data/UCD', state='/runtime/state',
                      bind='127.0.0.1', http_port=5188, threads=2, batch_mb=64)
        environment = deployment.runtime_environment(config, release)
        for name in ('TIER0', 'FLAGS', 'HIGHWAY', 'GRAMMARS', 'MANIFEST', 'RECIPES', 'FIRMWARE'):
            self.assertTrue(Path(environment['LAPLACE_' + name]).is_relative_to(release))
        self.assertEqual(environment['LAPLACE_CONNINFO'], config['connection'])

    def test_source_revision_changes_preserve_unchanged_files_and_reject_user_edits(self):
        source = self.work / 'selected-sources'
        repository = source / 'Laplace-MCP'
        repository.mkdir(parents=True)
        subprocess.run(['git', 'init', '--quiet', repository], check=True)
        def commit(message):
            subprocess.run(['git', '-C', repository, 'add', '.'], check=True)
            subprocess.run(['git', '-C', repository, '-c', 'user.name=Cache acceptance',
                            '-c', 'user.email=cache@example.invalid', 'commit', '--quiet', '-m', message], check=True)
            return subprocess.check_output(['git', '-C', repository, 'rev-parse', 'HEAD'], text=True).strip()
        (repository / 'reader.c').write_text('int reader(void){return 1;}')
        first = commit('Initial source')
        snapshot = self.work / 'incremental/sources'
        deployment.synchronize_sources(source, snapshot, {'Laplace-MCP': first})
        compiled_input = snapshot / 'Laplace-MCP/reader.c'
        timestamp = compiled_input.stat().st_mtime_ns
        (repository / 'README.md').write_text('Documentation changed')
        second = commit('Documentation only')
        deployment.synchronize_sources(source, snapshot, {'Laplace-MCP': second})
        self.assertEqual(timestamp, compiled_input.stat().st_mtime_ns)
        compiled_input.write_text('unfinished operator edit')
        with self.assertRaisesRegex(ValueError, 'snapshot was modified'):
            deployment.synchronize_sources(source, snapshot, {'Laplace-MCP': first})
        self.assertEqual(compiled_input.read_text(), 'unfinished operator edit')

    def test_resources_are_shared_without_linking_mutable_build_outputs(self):
        source = self.work / 'sources'
        artifact_owner = Path(os.environ.get('LAPLACE_OPERATIONS', ROOT.parent / 'Laplace-Operations')) / 'installation/artifact_store.py'
        target = source / 'Laplace-Operations/installation/artifact_store.py'
        target.parent.mkdir(parents=True)
        shutil.copyfile(artifact_owner, target)
        for relative in ('Laplace-Engine/recipes/order', 'Laplace-Engine/firmware/program.firmware',
                         'Laplace-Engine/recipes/formats/grammars/input/grammar.js', 'Laplace-Native/manifest/banks.tsv'):
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(relative.encode())
        inputs = self.work / 'build-inputs'
        inputs.mkdir()
        for name in ('tier0.bin', 'tier0.flags', 'tier0.flags.layout', 'tier0.highway', 'grammars/parser.so'):
            target = inputs / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(name.encode())
        config = dict(prefix=str(self.work / 'installed'), tier0=str(inputs / 'tier0.bin'),
                      flags=str(inputs / 'tier0.flags'), highway=str(inputs / 'tier0.highway'), grammars=str(inputs / 'grammars'))
        first, second = self.work / 'first', self.work / 'second'
        deployment.package_resources(config, source, first)
        deployment.package_resources(config, source, second)
        for relative in ('share/prepared/tier0.bin', 'share/grammars/parser.so', 'share/manifest/banks.tsv'):
            self.assertTrue(os.path.samefile(first / relative, second / relative))
        self.assertFalse((first / 'share/recipes/formats/grammars').exists())
        self.assertFalse(os.path.samefile(first / 'share/grammars/parser.so', inputs / 'grammars/parser.so'))
        (inputs / 'grammars/parser.so').write_bytes(b'changed compiler output')
        self.assertEqual((first / 'share/grammars/parser.so').read_bytes(), b'grammars/parser.so')
        selected = self.work / 'selected-engine-grammars'
        selected.mkdir()
        (selected / 'parser.so').write_bytes(b'the pinned Engine grammar')
        current = self.work / 'current'
        deployment.package_resources(config, source, current, selected)
        self.assertEqual((current / 'share/grammars/parser.so').read_bytes(), b'the pinned Engine grammar')
        self.assertEqual((first / 'share/grammars/parser.so').read_bytes(), b'grammars/parser.so')

    def release(self, controller=True):
        content = Path(deployment.__file__).read_bytes()
        receipt = {'schema': 'laplace.modular-package/v1', 'revisions': {},
                   'host_dependencies': {}, 'files': {deployment.CONTROLLER:
                   hashlib.sha256(content).hexdigest()} if controller else {}}
        resources = ('share/prepared/tier0.bin', 'share/prepared/tier0.flags', 'share/prepared/tier0.flags.layout',
                     'share/prepared/tier0.highway', 'share/manifest/banks.tsv', 'share/firmware/program.firmware',
                     'share/grammars/test.so')
        receipt['files'].update({name: hashlib.sha256(b'fixture').hexdigest() for name in resources})
        identity = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
        release = self.work / identity
        release.mkdir()
        (release / 'package.json').write_text(json.dumps(receipt))
        for name in resources:
            path = release / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture')
        if controller:
            path = release / deployment.CONTROLLER
            path.parent.mkdir()
            path.write_bytes(content)
        return release

    def test_controller_is_a_verified_release_member(self):
        release = self.release()
        self.assertIn(deployment.CONTROLLER, deployment.verify_package(release)['files'])
        (release / deployment.CONTROLLER).write_text('different controller')
        with self.assertRaisesRegex(ValueError, 'Package file differs'):
            deployment.verify_package(release)

    def test_older_release_requires_rebuild_instead_of_checkout_controller(self):
        with self.assertRaises(deployment.PackageUpgradeRequired):
            deployment.verify_package(self.release(controller=False))

    def test_cache_retention_preserves_current_and_unowned_directories(self):
        root = self.work / 'builds'
        root.mkdir()
        origin = self.work / 'source'
        subprocess.run(['git', 'init', '--quiet', str(origin)], check=True)
        (origin / 'source.txt').write_text('recorded source')
        subprocess.run(['git', '-C', str(origin), 'add', 'source.txt'], check=True)
        subprocess.run(['git', '-C', str(origin), '-c', 'user.name=Cache acceptance',
                        '-c', 'user.email=cache@example.invalid', 'commit', '--quiet', '-m', 'Source'], check=True)
        revision = subprocess.check_output(['git', '-C', str(origin), 'rev-parse', 'HEAD'], text=True).strip()
        builds = []
        for i in range(5):
            path = root / format(i, '064x')
            path.mkdir()
            marker = path / 'build-cache.json'
            marker.write_text(json.dumps({'schema': deployment.BUILD_CACHE_SCHEMA,
                                          'identity': path.name, 'revisions': {'Laplace-MCP': revision}}))
            subprocess.run(['git', 'clone', '--quiet', '--shared', str(origin),
                            str(path / 'sources/Laplace-MCP')], check=True)
            os.utime(marker, ns=(i + 1, i + 1))
            builds.append(path)
        unowned = root / ('f' * 64)
        unowned.mkdir()
        (unowned / 'source.txt').write_text('unowned work')
        (builds[2] / 'sources/Laplace-MCP/source.txt').write_text('unfinished edit')
        removed = deployment.prune_build_cache(root, builds[0])
        self.assertEqual([builds[1].name], removed)
        self.assertEqual({builds[0], builds[2], builds[3], builds[4], unowned}, set(root.iterdir()))

    @unittest.skipIf(os.name == 'nt', 'Linux packaging lock and symlink boundaries')
    def test_cache_lock_excludes_competing_builder_and_links(self):
        root = self.work / 'builds'
        with deployment.build_cache_lock(root):
            self.assertEqual(0o660, (root / '.package.lock').stat().st_mode & 0o777)
            with self.assertRaisesRegex(ValueError, 'Another modular package build'):
                with deployment.build_cache_lock(root):
                    self.fail('Competing build acquired cache')
        link = self.work / 'linked-builds'
        link.symlink_to(root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'unlinked directory'):
            with deployment.build_cache_lock(link):
                self.fail('Followed linked cache')

    @unittest.skipIf(os.name == 'nt', 'Linux package lifetime lock')
    def test_packaged_client_pins_its_real_release_until_it_exits(self):
        import fcntl
        stage = self.work / 'release'
        (stage / 'bin').mkdir(parents=True)
        for name in ('lpm', 'laplace'):
            shutil.copyfile(BINARY, stage / 'bin' / name)
            (stage / 'bin' / name).chmod(0o755)
        prepared = stage / 'share/prepared'
        prepared.mkdir(parents=True)
        shutil.copyfile(os.environ['LAPLACE_TIER0'], prepared / 'tier0.bin')
        deployment.runtime_launchers(stage)
        alias = self.work / 'current'
        alias.symlink_to(stage, target_is_directory=True)
        with deployment.candidate_listener([alias / 'bin/lpm'], self.environment, self.work) as (base, token):
            self.assertEqual({'ok': True}, deployment.request(base, '/health'))
            with (stage / '.runtime-lease').open() as lease:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with (stage / '.runtime-lease').open() as lease:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_missing_selected_cache_is_not_replaced_by_another_installation(self):
        environment = dict(self.environment, LAPLACE_TIER0=str(self.work / 'absent-tier0.bin'),
                           LAPLACE_MCP_TOKEN='cache-selection-acceptance-token-0123456789')
        for arguments in (['text', 'Argentina'], ['mcp'], ['serve', '127.0.0.1', '0']):
            with self.subTest(arguments=arguments):
                process = subprocess.run([str(BINARY), *arguments], env=environment, input='',
                                         cwd=self.work, capture_output=True, text=True, timeout=15)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn('tier 0 did not map', process.stderr)

    def test_actual_native_listener_authenticates_with_candidate_token(self):
        subprocess.run([str(BINARY), 'check'], env=self.environment, cwd=self.work,
                       check=True, capture_output=True, timeout=15)
        with deployment.candidate_listener([BINARY], self.environment, self.work) as (base, token):
            self.assertEqual({'ok': True}, deployment.request(base, '/health'))
            self.assertTrue(deployment.refused(base, '/v1/surface', None, None))
            self.assertTrue(deployment.refused(base, '/v1/surface', None, 'wrong' * 16))
            surface = deployment.request(base, '/v1/surface', token=token)
            self.assertEqual('Laplace-MCP', surface['service'])
            result = deployment.request(base, '/mcp', {'jsonrpc': '2.0', 'id': 1,
                'method': 'initialize', 'params': {'protocolVersion': '2025-11-25',
                'capabilities': {}, 'clientInfo': {'name': 'deployment', 'version': '1'}}}, token)
            self.assertEqual('laplace', result['result']['serverInfo']['name'])
        with self.assertRaises(OSError):
            deployment.request(base, '/health', timeout=.5)

    def test_dead_process_diagnostic_preserves_exit_and_redacts_environment(self):
        code = ('import os,sys; print(os.environ["LAPLACE_MCP_TOKEN"], flush=True); '
                'print(os.environ["LAPLACE_CONNINFO"], flush=True); '
                'print("refusing to listen unauthenticated", flush=True); sys.exit(2)')
        with self.assertRaisesRegex(ValueError, 'exit=2') as caught:
            with deployment.candidate_listener([sys.executable, '-c', code], self.environment, self.work):
                self.fail('Dead candidate reached verification')
        detail = str(caught.exception)
        self.assertIn('refusing to listen unauthenticated', detail)
        self.assertIn('[redacted]', detail)
        self.assertNotIn(self.environment['LAPLACE_CONNINFO'], detail)
        self.assertNotRegex(detail, r'\b[0-9a-f]{64}\b')

    def test_startup_deadline_tears_down_the_real_child(self):
        pid = self.work / 'child.pid'
        code = ('import os,pathlib,time; pathlib.Path(' + repr(str(pid)) +
                ').write_text(str(os.getpid())); time.sleep(60)')
        started = time.monotonic()
        with self.assertRaisesRegex(ValueError, 'startup timed out'):
            with deployment.candidate_listener([sys.executable, '-c', code], self.environment,
                                               self.work, timeout=.5):
                self.fail('Unready candidate reached verification')
        self.assertLess(time.monotonic() - started, 5)
        self.assertTrue(pid.is_file())
        if os.name != 'nt':
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid.read_text()), 0)


if __name__ == '__main__':
    unittest.main()
