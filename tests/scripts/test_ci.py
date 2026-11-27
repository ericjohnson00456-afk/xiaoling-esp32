import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import ci


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.catalog = [
            {'board': f'vendor/board-{i}', 'name': f'variant-{i:02d}',
             'target': 'esp32s3' if i < 21 else 'esp32c3'} for i in range(24)]

    def test_batches_cover_each_variant_once_and_do_not_mix_chips(self):
        batches = ci.shards_for(self.catalog, self.catalog)
        actual = [item for batch in batches for item in batch['variants']]
        self.assertEqual(sorted(item['name'] for item in actual),
                         sorted(item['name'] for item in self.catalog))
        for batch in batches:
            self.assertLessEqual(len(batch['variants']), ci.BATCH_SIZE)
            self.assertEqual({item['target'] for item in batch['variants']}, {batch['target']})

    def test_partial_build_preserves_the_full_build_cache_shard(self):
        full = ci.shards_for(self.catalog, self.catalog)
        selected = self.catalog[7]
        expected = next(batch['id'] for batch in full if selected in batch['variants'])
        self.assertEqual(ci.shards_for(self.catalog, [selected]),
                         [{'id': expected, 'target': selected['target'], 'variants': [selected]}])
        self.assertEqual(ci.shards_for(self.catalog, []), [])

    def test_event_selection_and_missing_diff_failure(self):
        event = {'before': 'a', 'after': 'b', 'ref': 'refs/heads/dev',
                 'pull_request': {'base': {'sha': 'a'}, 'head': {'sha': 'b'}}}
        diff = lambda *_: ['main/boards/vendor/board-7/config.h']
        self.assertEqual(ci.select_for_event(self.catalog, 'push', event, diff), [self.catalog[7]])
        self.assertEqual(ci.select_for_event(self.catalog, 'pull_request', event, diff), [self.catalog[7]])
        event['ref'] = 'refs/heads/main'
        self.assertEqual(ci.select_for_event(self.catalog, 'push', event, diff), self.catalog)
        self.assertEqual(ci.select_for_event(self.catalog, 'push', event, lambda *_: ['README.md']), [])
        event['before'] = '0' * 40
        self.assertEqual(ci.select_for_event(self.catalog, 'push', event, diff), self.catalog)
        for name in ('release', 'workflow_dispatch'):
            self.assertEqual(ci.select_for_event(self.catalog, name, {}, None), self.catalog)
        with self.assertRaises(RuntimeError):
            ci.select_for_event(self.catalog, 'pull_request', event,
                                lambda *_: (_ for _ in ()).throw(RuntimeError('missing base')))

    def test_absent_push_base_is_fetched_or_requests_a_full_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, checkout = root / 'source', root / 'checkout'
            subprocess.run(['git', 'init', '--quiet', str(source)], check=True)

            def git(*args):
                return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()

            path = source / 'main/boards/vendor/board-7/config.h'
            path.parent.mkdir(parents=True)
            for content in ('before', 'after'):
                path.write_text(content)
                git('add', '.')
                git('-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                    '-c', 'commit.gpgsign=false', 'commit', '--quiet', '-m', content)
                if content == 'before':
                    base = git('rev-parse', 'HEAD')
            head = git('rev-parse', 'HEAD')
            subprocess.run(['git', 'clone', '--quiet', '--no-checkout', '--depth=1',
                            source.as_uri(), str(checkout)], check=True)
            self.assertNotEqual(subprocess.run(
                ['git', '-C', str(checkout), 'cat-file', '-e', base + '^{commit}'],
                stderr=subprocess.DEVNULL).returncode, 0)
            event = {'before': base, 'after': head, 'ref': 'refs/heads/dev'}
            with patch.object(ci, 'ROOT', checkout):
                self.assertEqual(ci.select_for_event(self.catalog, 'push', event, ci.changed_paths),
                                 [self.catalog[7]])
                event['before'] = 'f' * 40
                self.assertEqual(ci.select_for_event(self.catalog, 'push', event, ci.changed_paths),
                                 self.catalog)

    def test_shared_board_files_do_not_silently_skip_builds(self):
        for path in ('main/boards/Kconfig.projbuild', 'main/boards/common/board.cc',
                     'main/application.cc', 'sdkconfig.defaults', '.github/workflows/build.yml'):
            self.assertEqual(ci.build.select_changed(self.catalog, [path]), self.catalog)
        for path in ('docs/custom-board.md', 'tests/audio/test_wake_recording.cc'):
            self.assertEqual(ci.build.select_changed(self.catalog, [path]), [])


class ReleaseAssetsTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.source = Path(self.work.name) / 'input'
        self.output = Path(self.work.name) / 'assets'
        self.source.mkdir()
        self.variant = {'board': 'vendor/board', 'name': 'variant', 'target': 'esp32s3'}
        self.archive = self.source / 'xiaoling_variant_v2.1.1.zip'
        self.archive.write_bytes(b'firmware')
        self.record = {**self.variant, 'success': True,
                       'sha256': hashlib.sha256(b'firmware').hexdigest()}
        self.manifest = {'sha': 'tested', 'variants': [self.record]}
        self.patches = [patch.object(ci.build, '_collect_variants', return_value=[self.variant]),
                        patch.object(ci.build, 'get_project_version', return_value='2.1.1'),
                        patch.dict(os.environ, {'GITHUB_SHA': 'tested'})]
        for item in self.patches:
            item.start()
            self.addCleanup(item.stop)

    def check(self):
        (self.source / 'batch.json').write_text(json.dumps(self.manifest))
        ci.release_assets(self.source, self.output)

    def test_complete_matching_build_is_ready_for_upload(self):
        self.check()
        self.assertEqual((self.output / self.archive.name).read_bytes(), b'firmware')

    def test_incomplete_stale_corrupt_failed_duplicate_or_wrong_board_never_uploads(self):
        cases = [lambda: self.manifest.update(sha='other'),
                 lambda: self.manifest.update(variants=[]),
                 lambda: self.record.update(success=False),
                 lambda: self.record.update(board='other/board'),
                 lambda: self.manifest['variants'].append(self.record),
                 lambda: self.archive.write_bytes(b'corrupt')]
        for mutate in cases:
            with self.subTest(mutate=mutate):
                original = json.loads(json.dumps(self.manifest))
                mutate()
                with self.assertRaises(ValueError):
                    self.check()
                self.assertFalse(self.output.exists())
                self.manifest = original
                self.record = self.manifest['variants'][0]
                self.archive.write_bytes(b'firmware')


class ReuseTests(unittest.TestCase):
    def setUp(self):
        self.catalog = [{'board': 'vendor/board', 'name': 'variant', 'target': 'esp32s3'}]
        self.run = {'id': 42, 'head_sha': 'tested', 'event': 'push', 'head_branch': 'main',
                    'status': 'completed', 'conclusion': 'success',
                    'head_repository': {'full_name': 'owner/repo'}}
        self.artifacts = [{'name': 'ci-plan', 'expired': False},
                          {'name': 'firmware-esp32s3-01', 'expired': False}]

    def lookup(self):
        def request(path):
            if '/workflows/' in path:
                return {'workflow_runs': [self.run]}
            self.assertIn('/runs/42/artifacts?', path)
            return {'artifacts': self.artifacts}
        return ci.find_reusable_run(self.catalog, 'tested', 'owner/repo', request)

    def test_only_complete_successful_same_commit_main_run_is_reused(self):
        self.assertEqual(self.lookup(), '42')
        for key, value in [('head_sha', 'stale'), ('event', 'pull_request'),
                           ('head_branch', 'dev'), ('status', 'in_progress'),
                           ('conclusion', 'failure'),
                           ('head_repository', {'full_name': 'fork/repo'})]:
            with self.subTest(key=key):
                previous = self.run[key]
                self.run[key] = value
                self.assertEqual(self.lookup(), '')
                self.run[key] = previous
        self.artifacts[1]['expired'] = True
        self.assertEqual(self.lookup(), '')
        self.artifacts.pop()
        self.assertEqual(self.lookup(), '')


class BatchTests(unittest.TestCase):
    def test_one_failure_keeps_building_and_preserves_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '--quiet', str(root)], check=True)
            (root / 'tracked.txt').write_text('canonical source')
            (root / 'asset.bin').write_bytes(b'pointer stored in git')
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test',
                            '-c', 'user.email=test@example.com', '-c', 'commit.gpgsign=false',
                            'commit', '--quiet', '-m', 'fixture'], check=True)
            # The populated checkout has actual resource bytes, unlike its Git blob.
            (root / 'asset.bin').write_bytes(b'materialized resource bytes')
            (root / 'sdkconfig').write_text('outside-build settings')
            (root / 'dependencies.lock').write_text('outside-build dependency state')
            variants = [{'board': 'vendor/' + name, 'name': name, 'target': 'esp32c3'}
                        for name in ('broken', 'working')]
            calls = []

            real_run = subprocess.run

            def run(command, **kwargs):
                if command[0] == 'git':
                    return real_run(command, **kwargs)
                workspace = Path(kwargs['cwd'])
                self.assertNotEqual(workspace, root)
                self.assertEqual(kwargs['env']['CCACHE_BASEDIR'], str(workspace))
                self.assertFalse((workspace / 'sdkconfig').exists())
                self.assertFalse((workspace / 'dependencies.lock').exists())
                self.assertFalse((workspace / 'build/CMakeCache.txt').exists())
                self.assertFalse((workspace / 'managed_components').exists())
                self.assertEqual((workspace / 'tracked.txt').read_text(), 'canonical source')
                self.assertEqual((workspace / 'asset.bin').read_bytes(), b'materialized resource bytes')
                self.assertEqual(subprocess.check_output(
                    ['git', '-C', str(workspace), 'rev-parse', 'HEAD']),
                    subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD']))
                # Pollute both the build directory and files outside Kconfig.
                (workspace / 'build').mkdir()
                (workspace / 'build/CMakeCache.txt').write_text('board-specific cache')
                (workspace / 'managed_components').mkdir()
                (workspace / 'sdkconfig').write_text('board-specific settings')
                (workspace / 'dependencies.lock').write_text('board-specific dependencies')
                (workspace / 'tracked.txt').write_text('board-specific generated source')
                name = command[command.index('--name') + 1]
                calls.append(name)
                kwargs['stdout'].write('build output\n')
                if name == 'working':
                    (workspace / 'releases').mkdir()
                    (workspace / 'releases/v2.1.1_working.zip').write_bytes(b'current')
                return type('Result', (), {'returncode': int(name == 'broken')})()

            with patch.object(ci, 'ROOT', root), patch.object(ci.subprocess, 'run', side_effect=run), \
                 patch.object(ci.build, 'get_project_version', return_value='2.1.1'), \
                 patch.dict(os.environ, {'CI_VARIANTS': json.dumps(variants), 'GITHUB_SHA': 'tested',
                                         'GITHUB_STEP_SUMMARY': str(root / 'summary')}):
                self.assertEqual(ci.run_batch('esp32c3-01'), 1)
            self.assertEqual(calls, ['broken', 'working'])
            self.assertFalse((root / '.cache/ci-source').exists())
            self.assertEqual((root / 'sdkconfig').read_text(), 'outside-build settings')
            output = root / 'ci-artifacts/esp32c3-01'
            manifest = json.loads((output / 'esp32c3-01.json').read_text())
            self.assertEqual([item['success'] for item in manifest['variants']], [False, True])
            self.assertEqual((output / 'working.log').read_text(), 'build output\n')
            self.assertEqual((output / 'xiaoling_working_v2.1.1.zip').read_bytes(), b'current')


if __name__ == '__main__':
    unittest.main()
