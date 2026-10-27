"""Verify release integrity and real candidate-process startup/teardown.

The native listener checks require the actual lpm executable as the first argument.
They exercise health, authentication and MCP initialization without database access.
"""
import hashlib
import importlib.util
import json
import os
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

    def release(self, controller=True):
        content = Path(deployment.__file__).read_bytes()
        receipt = {'schema': 'laplace.modular-package/v1', 'revisions': {},
                   'host_dependencies': {}, 'files': {deployment.CONTROLLER:
                   hashlib.sha256(content).hexdigest()} if controller else {}}
        identity = hashlib.sha256(json.dumps(receipt, sort_keys=True).encode()).hexdigest()
        release = self.work / identity
        release.mkdir()
        (release / 'package.json').write_text(json.dumps(receipt))
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
